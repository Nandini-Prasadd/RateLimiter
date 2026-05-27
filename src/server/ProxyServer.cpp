#include "httplib.h"
#include "nlohmann/json.hpp"
#include "storage/InMemoryStore.hpp"
#include "storage/RedisStore.hpp"
#include "metrics/SlidingWindowMetrics.hpp"
#include "metrics/PrometheusExporter.hpp"
#include "adaptive/AdaptiveController.hpp"
#include <iostream>
#include <fstream>
#include <memory>
#include <chrono>
#include <thread>
#include <cmath>

using json = nlohmann::json;
using namespace adaptive_rate_limiter;

// Global rate limiter components
std::shared_ptr<SlidingWindowMetrics> g_metrics;
std::shared_ptr<AdaptiveController> g_controller;
std::shared_ptr<IRateLimitStore> g_store;
std::shared_ptr<InMemoryStore> g_local_store; // Keep raw pointer reference for admin / local queries
uint64_t g_redis_errors_override = 0; // Backup counter if store is local-only

// Configuration settings
std::string g_storage_mode = "memory";
std::string g_backend_host = "127.0.0.1";
int g_backend_port = 8081;

struct PlanPolicy {
    double refill_rate;
    double capacity;
    std::string priority;
};
std::unordered_map<std::string, PlanPolicy> g_plans;

void load_configuration() {
    const char* env_config = std::getenv("CONFIG_FILE");
    std::string config_path = env_config ? env_config : "config/default.json";
    std::cout << "[ProxyServer] Reading configuration from " << config_path << "..." << std::endl;
    std::ifstream f(config_path);
    if (!f.is_open()) {
        std::cerr << "[ProxyServer] ERROR: Could not open config file, using hardcoded defaults." << std::endl;
        g_plans["free"] = {20.0, 40.0, "low"};
        g_plans["pro"] = {100.0, 200.0, "medium"};
        g_plans["premium"] = {300.0, 600.0, "high"};
        return;
    }

    try {
        json config = json::parse(f);
        
        // 1. Storage Configuration
        g_storage_mode = config["storage"]["mode"].get<std::string>();
        std::string redis_host = config["storage"]["redis_host"].get<std::string>();
        int redis_port = config["storage"]["redis_port"].get<int>();
        int redis_timeout = config["storage"]["redis_timeout_ms"].get<int>();

        // 2. Server Configuration
        g_backend_host = config["server"]["backend_host"].get<std::string>();
        g_backend_port = config["server"]["backend_port"].get<int>();

        // 3. Plans Configuration
        for (auto& [name, policy] : config["plans"].items()) {
            g_plans[name] = PlanPolicy{
                .refill_rate = policy["refill_rate"].get<double>(),
                .capacity = policy["capacity"].get<double>(),
                .priority = policy["priority"].get<std::string>()
            };
        }

        // 4. Adaptive Configuration
        double target_p95 = config["adaptive"]["target_p95_latency_ms"].get<double>();
        double max_err = config["adaptive"]["max_error_rate"].get<double>();
        double min_mult = config["adaptive"]["min_multiplier"].get<double>();
        double max_mult = config["adaptive"]["max_multiplier"].get<double>();
        double dec_step = config["adaptive"]["decrease_step"].get<double>();
        double inc_step = config["adaptive"]["increase_step"].get<double>();
        int cooldown = config["adaptive"]["cooldown_ms"].get<int>();

        g_metrics = std::make_shared<SlidingWindowMetrics>();
        g_controller = std::make_shared<AdaptiveController>(
            g_metrics, target_p95, max_err, min_mult, max_mult, dec_step, inc_step, cooldown
        );

        g_local_store = std::make_shared<InMemoryStore>();

        if (g_storage_mode == "redis") {
            std::cout << "[ProxyServer] Initializing in REDIS distributed mode pointing to " 
                      << redis_host << ":" << redis_port << std::endl;
            auto redis_client = std::make_shared<RedisClient>(redis_host, redis_port, redis_timeout);
            g_store = std::make_shared<RedisStore>(redis_client, g_local_store);
        } else {
            std::cout << "[ProxyServer] Initializing in MEMORY sharded mode." << std::endl;
            g_store = g_local_store;
        }
    } catch (const std::exception& e) {
        std::cerr << "[ProxyServer] Exception parsing configuration: " << e.what() << std::endl;
        throw;
    }
}

// Resolve user identity in X-API-Key -> X-User-ID -> client IP order
std::string resolve_identity(const httplib::Request& req) {
    if (req.has_header("X-API-Key")) {
        return req.get_header_value("X-API-Key");
    }
    if (req.has_header("X-User-ID")) {
        return req.get_header_value("X-User-ID");
    }
    return req.remote_addr; // client IP fallback
}

// Resolve plan from header, identity prefix, or fallback to free
std::string resolve_plan(const httplib::Request& req, const std::string& identity) {
    if (req.has_header("X-Test-Plan")) {
        std::string p = req.get_header_value("X-Test-Plan");
        if (g_plans.find(p) != g_plans.end()) return p;
    }
    // Parse from key string e.g. key_premium_123
    for (const std::string& plan : {"premium", "pro", "free"}) {
        if (identity.find(plan) != std::string::npos) {
            return plan;
        }
    }
    return "free"; // fallback default plan
}

void set_rate_limit_headers(httplib::Response& res, 
                             const std::string& plan, 
                             const std::string& mode, 
                             int limit, 
                             int remaining, 
                             int retry_after_ms) {
    res.set_header("X-RateLimit-Plan", plan);
    res.set_header("X-RateLimit-Mode", mode);
    res.set_header("X-RateLimit-Limit", std::to_string(limit));
    res.set_header("X-RateLimit-Remaining", std::to_string(remaining));
    res.set_header("X-RateLimit-Retry-After-Ms", std::to_string(retry_after_ms));
}

int main() {
    load_configuration();
    g_controller->start();

    httplib::Server svr;

    // 1. GET /health
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.status = 200;
        res.set_content("{\"status\":\"healthy\"}", "application/json");
    });

    // 2. GET /metrics
    svr.Get("/metrics", [](const httplib::Request&, httplib::Response& res) {
        uint64_t storage_errs = 0;
        if (g_storage_mode == "redis") {
            auto redis_store = std::dynamic_pointer_cast<RedisStore>(g_store);
            if (redis_store) {
                storage_errs = redis_store->get_storage_errors();
            }
        } else {
            storage_errs = g_redis_errors_override;
        }

        std::string payload = PrometheusExporter::export_metrics(*g_metrics, *g_controller, storage_errs);
        res.status = 200;
        res.set_content(payload, "text/plain");
    });

    // 3. GET /admin/state
    svr.Get("/admin/state", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("identity")) {
            res.status = 400;
            res.set_content("{\"error\":\"Missing 'identity' parameter\"}", "application/json");
            return;
        }
        std::string identity = req.get_param_value("identity");
        std::string plan = resolve_plan(req, identity);
        auto policy = g_plans[plan];

        double tokens = g_local_store->get_tokens(identity, policy.capacity);

        json payload = {
            {"identity", identity},
            {"plan", plan},
            {"local_tokens", tokens},
            {"capacity", policy.capacity},
            {"refill_rate", policy.refill_rate},
            {"priority", policy.priority},
            {"system_multiplier", g_controller->get_multiplier()}
        };
        res.status = 200;
        res.set_content(payload.dump(), "application/json");
    });

    // 4. POST /admin/adaptive/reset
    svr.Post("/admin/adaptive/reset", [](const httplib::Request&, httplib::Response& res) {
        g_controller->reset();
        res.status = 200;
        res.set_content("{\"status\":\"Adaptive limits reset successfully\"}", "application/json");
    });

    // 5. ANY /proxy/* -> Rate limit and forward to Backend Simulator
    auto proxy_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::string identity = resolve_identity(req);
        std::string plan = resolve_plan(req, identity);
        auto policy = g_plans[plan];

        double multiplier = g_controller->get_multiplier();
        double effective_refill_rate = policy.refill_rate * multiplier;

        // A. Priority Load Shedding Check
        if (g_controller->is_plan_shed(plan)) {
            g_metrics->record_request(plan, false, false, -1.0);
            res.status = 503;
            set_rate_limit_headers(res, plan, g_storage_mode, static_cast<int>(effective_refill_rate), 0, 3000);
            res.set_content("Service Unavailable (Load Shedding Active for plan: " + plan + ")", "text/plain");
            return;
        }

        // B. Cost query param extraction
        int cost = 1;
        if (req.has_param("cost")) {
            try {
                cost = std::stoi(req.get_param_value("cost"));
            } catch (...) {}
        }

        // C. Rate Limit enforcement
        RateLimitRequest rl_req{
            .identity = identity,
            .plan = plan,
            .cost = cost,
            .now = std::chrono::steady_clock::now()
        };

        RuntimePolicy rl_policy{
            .capacity = policy.capacity,
            .refill_rate = effective_refill_rate,
            .priority = policy.priority
        };

        RateLimitDecision decision = g_store->consume(rl_req, rl_policy);

        if (!decision.allowed) {
            g_metrics->record_request(plan, false, false, -1.0);
            res.status = 429;
            set_rate_limit_headers(res, plan, decision.storage_mode, 
                                   static_cast<int>(effective_refill_rate), 
                                   decision.remaining_tokens, 
                                   decision.retry_after_ms);
            res.set_content("Too Many Requests (Rate Limit Exhausted). Retry after: " 
                            + std::to_string(decision.retry_after_ms) + " ms\n", "text/plain");
            return;
        }

        // D. Forward Allowed Request to Backend Simulator
        g_metrics->increment_global_active_requests(1);

        // Build target path
        std::string path = req.path;
        if (path.rfind("/proxy", 0) == 0) {
            path = path.substr(6); // remove "/proxy" prefix
        }
        if (path.empty()) path = "/";

        // Include original query parameters
        if (!req.target.empty() && req.target.find("/proxy") == 0) {
            path = req.target.substr(6);
        }

        httplib::Client cli(g_backend_host, g_backend_port);
        cli.set_connection_timeout(std::chrono::seconds(5));
        cli.set_read_timeout(std::chrono::seconds(30));

        // Copy inbound headers (excluding rate limiting specific internal controls)
        httplib::Headers headers;
        for (const auto& [key, val] : req.headers) {
            if (key != "Host" && key != "X-Test-Plan") {
                headers.insert({key, val});
            }
        }

        auto start_time = std::chrono::steady_clock::now();
        httplib::Result backend_res;

        // Dispatch matching HTTP method
        if (req.method == "GET") {
            backend_res = cli.Get(path, headers);
        } else if (req.method == "POST") {
            backend_res = cli.Post(path, headers, req.body, req.get_header_value("Content-Type"));
        } else if (req.method == "PUT") {
            backend_res = cli.Put(path, headers, req.body, req.get_header_value("Content-Type"));
        } else if (req.method == "DELETE") {
            backend_res = cli.Delete(path, headers);
        } else {
            // Fallback for others
            backend_res = cli.Get(path, headers);
        }

        auto end_time = std::chrono::steady_clock::now();
        double latency_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

        g_metrics->increment_global_active_requests(-1);

        bool is_error = false;
        if (!backend_res) {
            is_error = true;
            g_metrics->record_request(plan, true, true, latency_ms);
            res.status = 502; // Bad Gateway
            set_rate_limit_headers(res, plan, decision.storage_mode, 
                                   static_cast<int>(effective_refill_rate), 
                                   decision.remaining_tokens, 0);
            res.set_content("Bad Gateway (Backend Simulator Unreachable)\n", "text/plain");
            return;
        }

        if (backend_res->status >= 500) {
            is_error = true;
        }

        // Record metrics
        g_metrics->record_request(plan, true, is_error, latency_ms);

        // E. Return Backend response to client
        res.status = backend_res->status;
        for (const auto& [key, val] : backend_res->headers) {
            if (key != "Content-Length" && key != "Transfer-Encoding") {
                res.set_header(key, val);
            }
        }
        set_rate_limit_headers(res, plan, decision.storage_mode, 
                               static_cast<int>(effective_refill_rate), 
                               decision.remaining_tokens, 0);
        res.set_content(backend_res->body, backend_res->get_header_value("Content-Type"));
    };

    svr.Get("/proxy/*", proxy_handler);
    svr.Post("/proxy/*", proxy_handler);
    svr.Put("/proxy/*", proxy_handler);
    svr.Delete("/proxy/*", proxy_handler);

    std::cout << "[ProxyServer] Starting rate limiter proxy server on port 8080..." << std::endl;
    if (!svr.listen("0.0.0.0", 8080)) {
        std::cerr << "Failed to start proxy server!" << std::endl;
        g_controller->stop();
        return 1;
    }

    g_controller->stop();
    return 0;
}
