#pragma once

// 系统负载采样（批次 43）：CPU 占用率 + 物理内存占用。
// - CPU：GetSystemTimes 相邻两次采样的 idle/kernel/user 差值（kernel 含 idle）；
// - 内存：GlobalMemoryStatusEx 的物理内存总量/可用量；
// - 只在 UI 线程调用（页面 0.1s 定时刷新），不做后台轮询线程。

#include <cstdint>

namespace sscl::platform {

struct SystemLoad {
    double cpu_percent = 0.0;       // 0..100（首次采样为 0，需要两次采样才能算差值）
    double memory_percent = 0.0;    // 0..100
    std::uint64_t memory_used_bytes = 0;
    std::uint64_t memory_total_bytes = 0;
};

// D-107「内存分配：自动配置」：按当前可用物理内存推算 Xmx（MB，256MB 步进）。
// 取可用内存的 45%，不超过物理内存一半与 16GB，且给系统至少留 2GB。
int recommended_game_memory_mb();

class SystemMonitor {
public:
    SystemMonitor();  // 建立 CPU 基线，第一次 sample() 即可得到约 100ms 的占用率

    SystemLoad sample();

private:
    bool m_has_previous = false;
    std::uint64_t m_prev_idle = 0;
    std::uint64_t m_prev_kernel = 0;
    std::uint64_t m_prev_user = 0;
};

}  // namespace sscl::platform
