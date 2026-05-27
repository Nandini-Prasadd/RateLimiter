#include "adaptive/AdaptiveController.hpp"
#include <iostream>
#include <algorithm>

namespace adaptive_rate_limiter {

AdaptiveController::AdaptiveController(std::shared_ptr<SlidingWindowMetrics> metrics,
                                       double target_p95_latency_ms,
                                       double max_error_rate,
                                       double min_multiplier,
                                       double max_multiplier,
                                       double decrease_step,
                                       double increase_step,
                                       int cooldown_ms)
    : metrics_(std::move(metrics)),
      target_p95_latency_ms_(target_p95_latency_ms),
      max_error_rate_(max_error_rate),
      min_multiplier_(min_multiplier),
      max_multiplier_(max_multiplier),
      decrease_step_(decrease_step),
      increase_step_(increase_step),
      cooldown_ms_(cooldown_ms),
      multiplier_(1.0),
      last_decrease_time_(std::chrono::steady_clock::now()) {}

AdaptiveController::~AdaptiveController() {
    stop();
}

void AdaptiveController::start() {
    std::unique_lock<std::mutex> lock(cv_mutex_);
    if (running_) return;
    running_ = true;
    worker_thread_ = std::thread(&AdaptiveController::run, this);
}

void AdaptiveController::stop() {
    {
        std::unique_lock<std::mutex> lock(cv_mutex_);
        if (!running_) return;
        running_ = false;
        cv_.notify_all();
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void AdaptiveController::reset() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    multiplier_.store(1.0);
    last_decrease_time_ = std::chrono::steady_clock::now();
}

bool AdaptiveController::is_plan_shed(const std::string& plan) const {
    double m = multiplier_.load();
    if (plan == "free") {
        return m < 0.8;
    } else if (plan == "pro") {
        return m < 0.5;
    }
    return false; // Premium is never shed
}

void AdaptiveController::run() {
    while (running_) {
        std::unique_lock<std::mutex> lock(cv_mutex_);
        // Sleep for 1 second or until notified of shutdown
        cv_.wait_for(lock, std::chrono::seconds(1), [this]() { return !running_; });

        if (!running_) break;

        // Perform adaptive calculations
        auto snapshot = metrics_->get_snapshot();
        auto now = std::chrono::steady_clock::now();

        double current_mult = multiplier_.load();
        double target_mult = current_mult;

        bool is_stressed = false;
        std::string reason;

        // Only adapt if we have registered some request metrics or if latency is abnormally high
        if (snapshot.p95_latency_ms > target_p95_latency_ms_) {
            is_stressed = true;
            reason = "p95 latency (" + std::to_string(snapshot.p95_latency_ms) + " ms) > target";
        } else if (snapshot.error_rate > max_error_rate_ && snapshot.total_requests > 0) {
            is_stressed = true;
            reason = "error rate (" + std::to_string(snapshot.error_rate * 100.0) + "%) > max";
        }

        std::lock_guard<std::mutex> state_lock(state_mutex_);
        if (is_stressed) {
            target_mult = std::max(min_multiplier_, current_mult - decrease_step_);
            if (target_mult < current_mult) {
                last_decrease_time_ = now;
                std::cout << "[AdaptiveController] Stress detected: " << reason 
                          << ". Multiplier decreased from " << current_mult 
                          << " to " << target_mult << std::endl;
            }
        } else {
            // Healthy system. Can we increase?
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_decrease_time_).count();
            if (elapsed_ms >= cooldown_ms_) {
                target_mult = std::min(max_multiplier_, current_mult + increase_step_);
                if (target_mult > current_mult) {
                    std::cout << "[AdaptiveController] System healthy. Multiplier increased from " 
                              << current_mult << " to " << target_mult << std::endl;
                }
            }
        }

        multiplier_.store(target_mult);
    }
}

} // namespace adaptive_rate_limiter
