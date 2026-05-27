#pragma once

#include <vector>
#include <array>
#include <atomic>
#include <mutex>
#include <chrono>

namespace adaptive_rate_limiter {

class LatencyBuffer {
private:
    static constexpr size_t CAPACITY = 1000;
    std::array<double, CAPACITY> buffer_{};
    std::atomic<size_t> write_index_{0};
    std::atomic<size_t> count_{0};

public:
    void record(double latency_ms);
    double get_p95() const;
    double get_average() const;
    void reset();
};

class SlidingWindowMetrics {
public:
    SlidingWindowMetrics();
    ~SlidingWindowMetrics() = default;

    void record_request(const std::string& plan, bool allowed, bool is_error, double latency_ms);

    // Aggregated metrics over the last 60 seconds
    struct Snapshot {
        uint64_t total_requests;
        uint64_t total_rejections;
        uint64_t total_errors;
        double p95_latency_ms;
        double avg_latency_ms;
        double error_rate;
    };

    Snapshot get_snapshot() const;
    
    // Global metrics (monotonically increasing)
    uint64_t get_global_requests() const { return global_requests_.load(); }
    uint64_t get_global_rejections() const { return global_rejections_.load(); }
    uint64_t get_global_errors() const { return global_errors_.load(); }

    // Plan-specific counters
    uint64_t get_allowed_requests(const std::string& plan) const {
        if (plan == "free") return global_allowed_free_.load();
        if (plan == "pro") return global_allowed_pro_.load();
        if (plan == "premium") return global_allowed_premium_.load();
        return 0;
    }

    uint64_t get_rejected_requests(const std::string& plan) const {
        if (plan == "free") return global_rejected_free_.load();
        if (plan == "pro") return global_rejected_pro_.load();
        if (plan == "premium") return global_rejected_premium_.load();
        return 0;
    }
    
    void increment_global_active_requests(int delta) {
        global_active_requests_.fetch_add(delta);
    }
    int get_global_active_requests() const {
        return global_active_requests_.load();
    }

private:
    struct MetricBucket {
        uint64_t requests = 0;
        uint64_t rejections = 0;
        uint64_t errors = 0;
        int64_t timestamp_sec = 0;
        mutable std::mutex mutex;
    };

    static constexpr size_t WINDOW_SIZE = 60;
    mutable std::array<MetricBucket, WINDOW_SIZE> buckets_;
    LatencyBuffer latency_buffer_;

    // Global accumulators
    std::atomic<uint64_t> global_requests_{0};
    std::atomic<uint64_t> global_rejections_{0};
    std::atomic<uint64_t> global_errors_{0};
    std::atomic<int> global_active_requests_{0};

    // Plan specific global counters
    std::atomic<uint64_t> global_allowed_free_{0};
    std::atomic<uint64_t> global_rejected_free_{0};
    std::atomic<uint64_t> global_allowed_pro_{0};
    std::atomic<uint64_t> global_rejected_pro_{0};
    std::atomic<uint64_t> global_allowed_premium_{0};
    std::atomic<uint64_t> global_rejected_premium_{0};

    void get_active_bucket(int64_t now_sec, MetricBucket*& out_bucket);
    void clean_stale_buckets(int64_t now_sec) const;
};

} // namespace adaptive_rate_limiter
