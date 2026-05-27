#pragma once

#include "storage/IRateLimitStore.hpp"
#include "storage/RedisClient.hpp"
#include "storage/InMemoryStore.hpp"
#include <memory>
#include <atomic>

namespace adaptive_rate_limiter {

class RedisStore : public IRateLimitStore {
public:
    RedisStore(std::shared_ptr<RedisClient> redis_client, 
               std::shared_ptr<InMemoryStore> local_fallback);
    ~RedisStore() override = default;

    RateLimitDecision consume(const RateLimitRequest& request, const RuntimePolicy& policy) override;

    uint64_t get_storage_errors() const { return storage_errors_.load(); }
    void reset_storage_errors() { storage_errors_.store(0); }

private:
    std::shared_ptr<RedisClient> redis_;
    std::shared_ptr<InMemoryStore> fallback_;
    std::string lua_script_;
    mutable std::atomic<uint64_t> storage_errors_{0};

    void init_lua_script();
};

} // namespace adaptive_rate_limiter
