#pragma once

#include "metrics/SlidingWindowMetrics.hpp"
#include "adaptive/AdaptiveController.hpp"
#include <string>

namespace adaptive_rate_limiter {

class PrometheusExporter {
public:
    static std::string export_metrics(const SlidingWindowMetrics& metrics,
                                      const AdaptiveController& controller,
                                      uint64_t storage_errors);
};

} // namespace adaptive_rate_limiter
