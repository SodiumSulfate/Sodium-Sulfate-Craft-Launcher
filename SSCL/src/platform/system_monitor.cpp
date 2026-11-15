#include "platform/system_monitor.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>

namespace sscl::platform {

namespace {

std::uint64_t filetime_to_u64(const FILETIME& value) {
    ULARGE_INTEGER integer{};
    integer.LowPart = value.dwLowDateTime;
    integer.HighPart = value.dwHighDateTime;
    return integer.QuadPart;
}

double clamp_percent(double value) {
    return (std::max)(0.0, (std::min)(100.0, value));
}

}  // namespace

// D-107：必须定义在匿名命名空间之外，否则是内部链接，
// SSCL.Core.lib 不导出符号，UI 工程会报 LNK2019/LNK2001（编译回传 #86）。
int recommended_game_memory_mb() {
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (::GlobalMemoryStatusEx(&memory) == FALSE) {
        return 2048;
    }
    constexpr std::uint64_t kMb = 1024ull * 1024ull;
    const std::uint64_t total = static_cast<std::uint64_t>(memory.ullTotalPhys) / kMb;
    const std::uint64_t available = static_cast<std::uint64_t>(memory.ullAvailPhys) / kMb;
    std::uint64_t target = (available * 45) / 100;
    const std::uint64_t ceiling = (std::min)(total / 2, std::uint64_t(16384));
    if (total > 2048) {
        target = (std::min)(target, total - 2048);
    }
    target = (std::min)(target, ceiling);
    if (target < 1024) {
        target = 1024;
    }
    return static_cast<int>((target / 256) * 256);  // 256MB 步进
}

SystemMonitor::SystemMonitor() {
    // 构造时采一次基线；100ms 后第一次 tick 就有真实 CPU 占用率。
    (void)sample();
}

SystemLoad SystemMonitor::sample() {
    SystemLoad load;

    FILETIME idle_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    const bool times_ok =
        ::GetSystemTimes(&idle_time, &kernel_time, &user_time) != FALSE;
    if (times_ok) {
        const std::uint64_t idle = filetime_to_u64(idle_time);
        const std::uint64_t kernel = filetime_to_u64(kernel_time);
        const std::uint64_t user = filetime_to_u64(user_time);
        if (m_has_previous && idle >= m_prev_idle && kernel >= m_prev_kernel &&
            user >= m_prev_user) {
            const std::uint64_t idle_delta = idle - m_prev_idle;
            const std::uint64_t kernel_delta = kernel - m_prev_kernel;
            const std::uint64_t user_delta = user - m_prev_user;
            const std::uint64_t total = kernel_delta + user_delta;  // kernel 已包含 idle
            if (total > 0) {
                const std::uint64_t busy = total > idle_delta ? total - idle_delta : 0;
                load.cpu_percent =
                    clamp_percent(100.0 * static_cast<double>(busy) / static_cast<double>(total));
            }
        }
        m_prev_idle = idle;
        m_prev_kernel = kernel;
        m_prev_user = user;
        m_has_previous = true;
    }

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (::GlobalMemoryStatusEx(&memory) != FALSE) {
        load.memory_total_bytes = static_cast<std::uint64_t>(memory.ullTotalPhys);
        const std::uint64_t available = static_cast<std::uint64_t>(memory.ullAvailPhys);
        load.memory_used_bytes =
            load.memory_total_bytes > available ? load.memory_total_bytes - available : 0;
        if (load.memory_total_bytes > 0) {
            load.memory_percent =
                clamp_percent(100.0 * static_cast<double>(load.memory_used_bytes) /
                              static_cast<double>(load.memory_total_bytes));
        }
    }

    return load;
}

}  // namespace sscl::platform
