#pragma once

#include "core/RateLimiterCore.hpp"

namespace adaptive_rate_limiter {

class IRateLimitStore {
public:
    virtual ~IRateLimitStore() = default;
    virtual RateLimitDecision consume(const RateLimitRequest& request, const RuntimePolicy& policy) = 0;
};

} // namespace adaptive_rate_limiter
