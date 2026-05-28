#pragma once
#include <chrono>
#include <algorithm>

class TokenBucket {
private:
    int capacity;
    double tokens;
    double refill_rate;
    std::chrono::steady_clock::time_point last_refill;

public:
    TokenBucket(int cap, double rate)
        : capacity(cap), tokens(cap), refill_rate(rate),
          last_refill(std::chrono::steady_clock::now()) {}

    bool allow() {
        refill();

        if (tokens >= 1) {
            tokens -= 1;
            return true;
        }
        return false;
    }

    int getRemaining() const {
        return (int)tokens;
    }

    double getTimeToNextToken() {
        if (tokens >= 1) return 0.0;
        return (1.0 - tokens) / refill_rate;
    }

private:
    void refill() {
        auto now = std::chrono::steady_clock::now();
        double seconds =
            std::chrono::duration<double>(now - last_refill).count();

        tokens = std::min((double)capacity, tokens + seconds * refill_rate);
        last_refill = now;
    }
};
