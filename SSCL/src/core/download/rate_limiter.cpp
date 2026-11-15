#include "core/download/rate_limiter.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace sscl::core {

namespace {
constexpr std::int64_t kRefillIntervalMs = 100;  // §3：每 100ms 注入
constexpr std::int64_t kMinBucketCapacity = 64 * 1024;
}

RateLimiter::RateLimiter(std::int64_t bytes_per_second)
    : m_rate(bytes_per_second < 0 ? 0 : bytes_per_second),
      m_tokens(static_cast<double>(bytes_per_second < 0 ? 0 : bytes_per_second)),  // 初始满桶
      m_last_refill(std::chrono::steady_clock::now()) {}

void RateLimiter::set_rate(std::int64_t bytes_per_second) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_rate = bytes_per_second < 0 ? 0 : bytes_per_second;
    m_last_refill = std::chrono::steady_clock::now();
    m_tokens = static_cast<double>(m_rate);  // 满桶
    m_cv.notify_all();
}

std::int64_t RateLimiter::rate() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_rate;
}

void RateLimiter::refill_locked(std::chrono::steady_clock::time_point now) {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_refill).count();
    if (elapsed < kRefillIntervalMs) {
        return;
    }
    const double added = static_cast<double>(m_rate) * static_cast<double>(elapsed) / 1000.0;
    const double capacity = static_cast<double>((std::max)(m_rate, kMinBucketCapacity));
    // 至少容纳一次 64KiB 读取，避免低速时令牌永远达不到 consume 所需数量。
    m_tokens = (std::min)(m_tokens + added, capacity);
    m_last_refill = now;
}

bool RateLimiter::consume(std::size_t bytes, std::stop_token stop) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_rate == 0) {
        return !stop.stop_requested();
    }

    const double needed = static_cast<double>(bytes);
    for (;;) {
        refill_locked(std::chrono::steady_clock::now());
        if (m_tokens >= needed) {
            m_tokens -= needed;
            return true;
        }
        if (stop.stop_requested()) {
            return false;
        }
        // 等到下次注入窗口
        m_cv.wait_for(lock, std::chrono::milliseconds(kRefillIntervalMs), [&] {
            return stop.stop_requested();
        });
        if (stop.stop_requested()) {
            return false;
        }
    }
}

}  // namespace sscl::core
