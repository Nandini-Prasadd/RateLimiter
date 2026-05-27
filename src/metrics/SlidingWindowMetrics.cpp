#include "metrics/SlidingWindowMetrics.hpp"
#include <algorithm>
#include <numeric>

namespace adaptive_rate_limiter {

// ================= LatencyBuffer Implementation =================

void LatencyBuffer::record(double latency_ms) {
    size_t idx = write_index_.fetch_add(1, std::memory_order_relaxed) % CAPACITY;
    buffer_[idx] = latency_ms;
    if (count_ < CAPACITY) {
        count_.fetch_add(1, std::memory_order_relaxed);
    }
}

double LatencyBuffer::get_p95() const {
    size_t current_count = std::min(count_.load(std::memory_order_relaxed), CAPACITY);
    if (current_count == 0) return 0.0;

    std::vector<double> samples;
    samples.reserve(current_count);
    for (size_t i = 0; i < current_count; ++i) {
        samples.push_back(buffer_[i]);
    }
    std::sort(samples.begin(), samples.end());
    size_t p95_idx = static_cast<size_t>(0.95 * (samples.size() - 1));
    return samples[p95_idx];
}

double LatencyBuffer::get_average() const {
    size_t current_count = std::min(count_.load(std::memory_order_relaxed), CAPACITY);
    if (current_count == 0) return 0.0;

    double sum = 0.0;
    for (size_t i = 0; i < current_count; ++i) {
        sum += buffer_[i];
    }
    return sum / current_count;
}

void LatencyBuffer::reset() {
    write_index_.store(0);
    count_.store(0);
    buffer_.fill(0.0);
}

// ================= SlidingWindowMetrics Implementation =================

SlidingWindowMetrics::SlidingWindowMetrics() {
    // Force initialize buckets with timestamps from the past
    for (auto& bucket : buckets_) {
        bucket.timestamp_sec = -1;
    }
}

void SlidingWindowMetrics::get_active_bucket(int64_t now_sec, MetricBucket*& out_bucket) {
    size_t idx = now_sec % WINDOW_SIZE;
    out_bucket = &buckets_[idx];

    std::lock_guard<std::mutex> lock(out_bucket->mutex);
    if (out_bucket->timestamp_sec != now_sec) {
        out_bucket->requests = 0;
        out_bucket->rejections = 0;
        out_bucket->errors = 0;
        out_bucket->timestamp_sec = now_sec;
    }
}

void SlidingWindowMetrics::record_request(const std::string& plan, bool allowed, bool is_error, double latency_ms) {
    auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();

    global_requests_.fetch_add(1);
    if (!allowed) {
        global_rejections_.fetch_add(1);
    }
    if (is_error) {
        global_errors_.fetch_add(1);
    }

    // Plan specific increments
    if (plan == "free") {
        if (allowed) global_allowed_free_.fetch_add(1);
        else global_rejected_free_.fetch_add(1);
    } else if (plan == "pro") {
        if (allowed) global_allowed_pro_.fetch_add(1);
        else global_rejected_pro_.fetch_add(1);
    } else if (plan == "premium") {
        if (allowed) global_allowed_premium_.fetch_add(1);
        else global_rejected_premium_.fetch_add(1);
    }

    MetricBucket* bucket = nullptr;
    get_active_bucket(now_sec, bucket);
    
    if (bucket) {
        std::lock_guard<std::mutex> lock(bucket->mutex);
        bucket->requests++;
        if (!allowed) {
            bucket->rejections++;
        }
        if (is_error) {
            bucket->errors++;
        }
    }

    if (allowed && latency_ms >= 0.0) {
        latency_buffer_.record(latency_ms);
    }
}

SlidingWindowMetrics::Snapshot SlidingWindowMetrics::get_snapshot() const {
    auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();

    uint64_t total_requests = 0;
    uint64_t total_rejections = 0;
    uint64_t total_errors = 0;

    for (const auto& bucket : buckets_) {
        std::lock_guard<std::mutex> lock(bucket.mutex);
        // Only count buckets within the 60-second window
        if (now_sec - bucket.timestamp_sec < static_cast<int64_t>(WINDOW_SIZE) && bucket.timestamp_sec >= 0) {
            total_requests += bucket.requests;
            total_rejections += bucket.rejections;
            total_errors += bucket.errors;
        }
    }

    double error_rate = 0.0;
    uint64_t allowed = total_requests - total_rejections;
    if (allowed > 0) {
        error_rate = static_cast<double>(total_errors) / allowed;
    }

    return Snapshot{
        .total_requests = total_requests,
        .total_rejections = total_rejections,
        .total_errors = total_errors,
        .p95_latency_ms = latency_buffer_.get_p95(),
        .avg_latency_ms = latency_buffer_.get_average(),
        .error_rate = error_rate
    };
}

} // namespace adaptive_rate_limiter
