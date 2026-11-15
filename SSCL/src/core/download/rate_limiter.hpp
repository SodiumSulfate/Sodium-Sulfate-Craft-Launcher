#pragma once

// 全局限速：令牌桶（knowledge/download-mirrors.md §3：每 100ms 注入 rate/10 令牌）。
// rate = 0 表示不限速；consume 可被 stop_token 中断。

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>

namespace sscl::core {

class RateLimiter {
public:
    explicit RateLimiter(std::int64_t bytes_per_second = 0);

    void set_rate(std::int64_t bytes_per_second);
    std::int64_t rate() const;

    // 阻塞直到攒够 bytes 令牌；被中断返回 false（调用方应中止下载）。
    bool consume(std::size_t bytes, std::stop_token stop);

private:
    void refill_locked(std::chrono::steady_clock::time_point now);

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::int64_t m_rate = 0;        // 字节/秒；0 = 不限速
    double m_tokens = 0.0;          // 当前令牌
    std::chrono::steady_clock::time_point m_last_refill;
};

}  // namespace sscl::core
