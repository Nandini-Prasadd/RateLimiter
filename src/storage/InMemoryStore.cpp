#include "storage/InMemoryStore.hpp"
#include <cmath>
#include <algorithm>

namespace adaptive_rate_limiter {

InMemoryStore::InMemoryStore(size_t num_shards) : shards_(num_shards) {}

size_t InMemoryStore::get_shard_index(const std::string& key) const {
    std::hash<std::string> hasher;
    return hasher(key) % shards_.size();
}

RateLimitDecision InMemoryStore::consume(const RateLimitRequest& request, const RuntimePolicy& policy) {
    size_t shard_idx = get_shard_index(request.identity);
    auto& shard = shards_[shard_idx];
    std::lock_guard<std::mutex> lock(shard.mutex);

    auto now = request.now;
    auto it = shard.buckets.find(request.identity);

    if (it == shard.buckets.end()) {
        // First request for this user
        if (policy.capacity >= request.cost) {
            Bucket bucket{policy.capacity - request.cost, now};
            shard.buckets[request.identity] = bucket;
            return RateLimitDecision{
                .allowed = true,
                .remaining_tokens = static_cast<int>(std::floor(policy.capacity - request.cost)),
                .retry_after_ms = 0,
                .reason = "",
                .storage_mode = "memory"
            };
        } else {
            // Cost is higher than plan capacity
            return RateLimitDecision{
                .allowed = false,
                .remaining_tokens = 0,
                .retry_after_ms = -1, // Permanent block for this cost size
                .reason = "Cost exceeds plan capacity",
                .storage_mode = "memory"
            };
        }
    }

    // Existing bucket, refill first
    auto& bucket = it->second;
    double elapsed_seconds = std::chrono::duration<double>(now - bucket.last_refill).count();
    
    // Add tokens based on elapsed time and refill rate
    if (elapsed_seconds > 0.0) {
        bucket.tokens = std::min(policy.capacity, bucket.tokens + elapsed_seconds * policy.refill_rate);
        bucket.last_refill = now;
    }

    if (bucket.tokens >= request.cost) {
        bucket.tokens -= request.cost;
        return RateLimitDecision{
            .allowed = true,
            .remaining_tokens = static_cast<int>(std::floor(bucket.tokens)),
            .retry_after_ms = 0,
            .reason = "",
            .storage_mode = "memory"
        };
    } else {
        // Not enough tokens
        double missing = request.cost - bucket.tokens;
        double wait_sec = missing / policy.refill_rate;
        int retry_ms = std::max(1, static_cast<int>(std::ceil(wait_sec * 1000.0)));

        return RateLimitDecision{
            .allowed = false,
            .remaining_tokens = static_cast<int>(std::floor(bucket.tokens)),
            .retry_after_ms = retry_ms,
            .reason = "Token exhaustion",
            .storage_mode = "memory"
        };
    }
}

double InMemoryStore::get_tokens(const std::string& identity, double capacity) const {
    size_t shard_idx = get_shard_index(identity);
    auto& shard = shards_[shard_idx];
    std::lock_guard<std::mutex> lock(shard.mutex);

    auto it = shard.buckets.find(identity);
    if (it == shard.buckets.end()) {
        return capacity;
    }
    return it->second.tokens;
}

} // namespace adaptive_rate_limiter
