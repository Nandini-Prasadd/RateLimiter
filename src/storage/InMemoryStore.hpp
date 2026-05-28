#pragma once
#include <unordered_map>
#include <string>
#include <mutex>
#include "../core/TokenBucket.hpp"

class InMemoryStore {
private:
    std::unordered_map<std::string, TokenBucket> buckets;
    std::mutex mtx;

    // 🔥 API key → (capacity, refill_rate)
    std::unordered_map<std::string, std::pair<int, double>> config = {
        {"userA", {5, 1}},       // free user
        {"userB", {10, 2}},      // better user
        {"premium", {20, 5}}     // premium user
    };

public:
    TokenBucket& getBucket(const std::string& user) {
        std::lock_guard<std::mutex> lock(mtx);

        auto it = buckets.find(user);
        if (it != buckets.end()) {
            return it->second;
        }

        // 🔥 Get config for user
        int cap = 5;
        double rate = 1;

        auto cfg = config.find(user);
        if (cfg != config.end()) {
            cap = cfg->second.first;
            rate = cfg->second.second;
        }

        auto result = buckets.emplace(user, TokenBucket(cap, rate));
        return result.first->second;
    }

    int getLimit(const std::string& user) {
        auto cfg = config.find(user);
        if (cfg != config.end()) {
            return cfg->second.first;
        }
        return 5;
    }
};
