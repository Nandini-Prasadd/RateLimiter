#pragma once

#include "storage/IRateLimitStore.hpp"
#include <unordered_map>
#include <mutex>
#include <vector>
#include <string>

namespace adaptive_rate_limiter {

class InMemoryStore : public IRateLimitStore {
public:
    explicit InMemoryStore(size_t num_shards = 64);
    ~InMemoryStore() override = default;

    RateLimitDecision consume(const RateLimitRequest& request, const RuntimePolicy& policy) override;

    // Helper for debugging/admin to inspect current tokens
    double get_tokens(const std::string& identity, double capacity) const;

private:
    struct Bucket {
        double tokens;
        std::chrono::steady_clock::time_point last_refill;
    };

    struct Shard {
        std::unordered_map<std::string, Bucket> buckets;
        mutable std::mutex mutex;
    };

    std::vector<Shard> shards_;
    size_t get_shard_index(const std::string& key) const;
};

} // namespace adaptive_rate_limiter
