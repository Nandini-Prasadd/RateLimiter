#pragma once

#include <string>
#include <chrono>

namespace adaptive_rate_limiter {

struct RateLimitRequest {
    std::string identity;
    std::string plan;
    int cost = 1;
    std::chrono::steady_clock::time_point now;
};

struct RateLimitDecision {
    bool allowed;
    int remaining_tokens;
    int retry_after_ms;
    std::string reason;
    std::string storage_mode;
};

struct RuntimePolicy {
    double capacity;
    double refill_rate; // tokens per second
    std::string priority;
};

} // namespace adaptive_rate_limiter
