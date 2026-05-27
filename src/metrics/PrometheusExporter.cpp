#include "metrics/PrometheusExporter.hpp"
#include <sstream>
#include <iomanip>

namespace adaptive_rate_limiter {

std::string PrometheusExporter::export_metrics(const SlidingWindowMetrics& metrics,
                                                const AdaptiveController& controller,
                                                uint64_t storage_errors) {
    std::ostringstream ss;
    
    // Set double precision
    ss << std::fixed << std::setprecision(3);

    auto snapshot = metrics.get_snapshot();
    double multiplier = controller.get_multiplier();

    // 1. rate_limiter_requests_total
    ss << "# HELP rate_limiter_requests_total Total number of rate limiter requests\n"
       << "# TYPE rate_limiter_requests_total counter\n";
    
    for (const std::string& plan : {"free", "pro", "premium"}) {
        uint64_t allowed = metrics.get_allowed_requests(plan);
        uint64_t rejected = metrics.get_rejected_requests(plan);
        ss << "rate_limiter_requests_total{decision=\"allowed\",plan=\"" << plan << "\"} " << allowed << "\n"
           << "rate_limiter_requests_total{decision=\"rejected\",plan=\"" << plan << "\"} " << rejected << "\n";
    }

    // 2. rate_limiter_rejections_total (approximate breakdown globally or plan level)
    ss << "# HELP rate_limiter_rejections_total Total number of rate limiter rejections\n"
       << "# TYPE rate_limiter_rejections_total counter\n";
    for (const std::string& plan : {"free", "pro", "premium"}) {
        uint64_t rejected = metrics.get_rejected_requests(plan);
        // Expose overall plan-level rejection. Reasons can be "token_exhaustion" or "load_shedding"
        // Let's divide by shed state to give a descriptive metric
        bool is_shed = controller.is_plan_shed(plan);
        std::string reason = is_shed ? "load_shedding" : "token_exhaustion";
        ss << "rate_limiter_rejections_total{reason=\"" << reason << "\",plan=\"" << plan << "\"} " << rejected << "\n";
    }

    // 3. rate_limiter_backend_latency_ms
    ss << "# HELP rate_limiter_backend_latency_p95_ms 95th percentile of backend latency in milliseconds\n"
       << "# TYPE rate_limiter_backend_latency_p95_ms gauge\n"
       << "rate_limiter_backend_latency_p95_ms " << snapshot.p95_latency_ms << "\n"
       << "# HELP rate_limiter_backend_latency_avg_ms Average backend latency in milliseconds\n"
       << "# TYPE rate_limiter_backend_latency_avg_ms gauge\n"
       << "rate_limiter_backend_latency_avg_ms " << snapshot.avg_latency_ms << "\n";

    // 4. rate_limiter_backend_errors_total
    ss << "# HELP rate_limiter_backend_errors_total Total number of backend simulator errors\n"
       << "# TYPE rate_limiter_backend_errors_total counter\n"
       << "rate_limiter_backend_errors_total " << metrics.get_global_errors() << "\n";

    // 5. rate_limiter_adaptive_multiplier
    ss << "# HELP rate_limiter_adaptive_multiplier Current adaptive rate limiter multiplier\n"
       << "# TYPE rate_limiter_adaptive_multiplier gauge\n";
    for (const std::string& plan : {"free", "pro", "premium"}) {
        ss << "rate_limiter_adaptive_multiplier{plan=\"" << plan << "\"} " << multiplier << "\n";
    }

    // 6. rate_limiter_storage_errors_total
    ss << "# HELP rate_limiter_storage_errors_total Total number of storage communication errors\n"
       << "# TYPE rate_limiter_storage_errors_total counter\n"
       << "rate_limiter_storage_errors_total " << storage_errors << "\n";

    // 7. rate_limiter_active_requests
    ss << "# HELP rate_limiter_active_requests Current active requests in the reverse proxy\n"
       << "# TYPE rate_limiter_active_requests gauge\n"
       << "rate_limiter_active_requests " << metrics.get_global_active_requests() << "\n";

    return ss.str();
}

} // namespace adaptive_rate_limiter
