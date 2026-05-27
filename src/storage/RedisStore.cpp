#include "storage/RedisStore.hpp"
#include <chrono>
#include <iostream>

namespace adaptive_rate_limiter {

RedisStore::RedisStore(std::shared_ptr<RedisClient> redis_client, 
                       std::shared_ptr<InMemoryStore> local_fallback)
    : redis_(std::move(redis_client)), fallback_(std::move(local_fallback)) {
    init_lua_script();
}

void RedisStore::init_lua_script() {
    // Lua script executes HMGET, calculates elapsed time, refills tokens,
    // and checks if we have enough tokens to consume.
    // Returns {allowed (0/1), remaining_tokens, retry_after_ms}
    lua_script_ = R"LUA(
        local key = KEYS[1]
        local capacity = tonumber(ARGV[1])
        local refill_rate_ms = tonumber(ARGV[2])
        local cost = tonumber(ARGV[3])
        local now_ms = tonumber(ARGV[4])

        local data = redis.call('HMGET', key, 'tokens', 'last_refill_ms')
        local tokens = tonumber(data[1])
        local last_refill_ms = tonumber(data[2])

        if not tokens then
            -- First request
            if capacity >= cost then
                tokens = capacity - cost
                last_refill_ms = now_ms
                redis.call('HMSET', key, 'tokens', tokens, 'last_refill_ms', last_refill_ms)
                redis.call('EXPIRE', key, 86400) -- TTL of 24h
                return {1, math.floor(tokens), 0}
            else
                return {0, 0, -1}
            end
        else
            local elapsed = now_ms - last_refill_ms
            if elapsed > 0 then
                tokens = math.min(capacity, tokens + elapsed * refill_rate_ms)
                last_refill_ms = now_ms
            end

            if tokens >= cost then
                tokens = tokens - cost
                redis.call('HMSET', key, 'tokens', tokens, 'last_refill_ms', last_refill_ms)
                redis.call('EXPIRE', key, 86400) -- refresh TTL
                return {1, math.floor(tokens), 0}
            else
                local missing = cost - tokens
                local wait_ms = math.ceil(missing / refill_rate_ms)
                return {0, math.floor(tokens), wait_ms}
            end
        end
    )LUA";
}

RateLimitDecision RedisStore::consume(const RateLimitRequest& request, const RuntimePolicy& policy) {
    // Generate key name
    std::string key = "rl:" + request.identity;

    // Get current Unix epoch time in ms
    auto now_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    // Policy refill rate is tokens/sec. Convert to tokens/ms.
    double refill_rate_ms = policy.refill_rate / 1000.0;

    std::vector<long long> result;
    bool success = false;

    try {
        success = redis_->execute_token_bucket(
            lua_script_,
            key,
            policy.capacity,
            refill_rate_ms,
            request.cost,
            now_epoch,
            result
        );
    } catch (const std::exception& e) {
        std::cerr << "Exception in RedisStore: " << e.what() << std::endl;
        success = false;
    }

    if (success && result.size() == 3) {
        bool allowed = (result[0] == 1);
        int remaining = static_cast<int>(result[1]);
        int retry_ms = static_cast<int>(result[2]);

        return RateLimitDecision{
            .allowed = allowed,
            .remaining_tokens = remaining,
            .retry_after_ms = retry_ms,
            .reason = allowed ? "" : "Token exhaustion (Redis)",
            .storage_mode = "redis"
        };
    }

    // Redis communication failed! Degrade to local fallback.
    storage_errors_.fetch_add(1);
    
    // Invoke local fallback in-memory store
    RateLimitDecision decision = fallback_->consume(request, policy);
    decision.storage_mode = "degraded-local";
    decision.reason = decision.allowed ? "Fallback: Redis down (allowed locally)" : "Fallback: Redis down (exhausted locally)";
    return decision;
}

} // namespace adaptive_rate_limiter
