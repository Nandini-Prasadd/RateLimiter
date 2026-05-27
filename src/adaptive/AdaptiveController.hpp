#pragma once

#include "metrics/SlidingWindowMetrics.hpp"
#include <memory>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <chrono>

namespace adaptive_rate_limiter {

class AdaptiveController {
public:
    AdaptiveController(std::shared_ptr<SlidingWindowMetrics> metrics,
                       double target_p95_latency_ms = 120.0,
                       double max_error_rate = 0.05,
                       double min_multiplier = 0.25,
                       double max_multiplier = 1.50,
                       double decrease_step = 0.15,
                       double increase_step = 0.05,
                       int cooldown_ms = 3000);
    ~AdaptiveController();

    void start();
    void stop();

    double get_multiplier() const { return multiplier_.load(); }
    void reset();

    // Priority load-shedding decision helper
    bool is_plan_shed(const std::string& plan) const;

    // Get current adaptive parameters (for metrics/logging)
    double target_p95_latency_ms() const { return target_p95_latency_ms_; }
    double max_error_rate() const { return max_error_rate_; }
    int cooldown_ms() const { return cooldown_ms_; }

private:
    std::shared_ptr<SlidingWindowMetrics> metrics_;
    
    // Adaptive Throttling Constants
    double target_p95_latency_ms_;
    double max_error_rate_;
    double min_multiplier_;
    double max_multiplier_;
    double decrease_step_;
    double increase_step_;
    int cooldown_ms_;

    // Current State
    std::atomic<double> multiplier_{1.0};
    std::chrono::steady_clock::time_point last_decrease_time_;
    mutable std::mutex state_mutex_;

    // Background Thread Orchestration
    std::thread worker_thread_;
    std::atomic<bool> running_{false};
    std::condition_variable cv_;
    std::mutex cv_mutex_;

    void run();
};

} // namespace adaptive_rate_limiter
