#include "httplib.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <random>
#include <string>

int main() {
    httplib::Server svr;

    // Random number generator for error rates
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.status = 200;
        res.set_content("OK", "text/plain");
    });

    svr.Get("/work", [&](const httplib::Request& req, httplib::Response& res) {
        double latency_ms = 0.0;
        double error_rate = 0.0;

        if (req.has_param("latency_ms")) {
            try {
                latency_ms = std::stod(req.get_param_value("latency_ms"));
            } catch (...) {}
        }

        if (req.has_param("error_rate")) {
            try {
                error_rate = std::stod(req.get_param_value("error_rate"));
            } catch (...) {}
        }

        // 1. Simulate Latency
        if (latency_ms > 0.0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(latency_ms)));
        }

        // 2. Simulate Random Failures
        if (dist(gen) < error_rate) {
            res.status = 500;
            res.set_content("Internal Server Error (Simulated)", "text/plain");
            std::cout << "[BackendSimulator] Serviced /work with latency: " << latency_ms 
                      << "ms -> Status: 500 (Simulated)" << std::endl;
        } else {
            res.status = 200;
            res.set_content("Success (Simulated)", "text/plain");
            std::cout << "[BackendSimulator] Serviced /work with latency: " << latency_ms 
                      << "ms -> Status: 200" << std::endl;
        }
    });

    std::cout << "[BackendSimulator] Starting simulator server on port 8081..." << std::endl;
    if (!svr.listen("0.0.0.0", 8081)) {
        std::cerr << "Failed to start simulator server!" << std::endl;
        return 1;
    }
    return 0;
}
