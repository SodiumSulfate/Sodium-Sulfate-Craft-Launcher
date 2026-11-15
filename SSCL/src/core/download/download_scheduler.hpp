#pragma once

// 下载调度器（开发文档 §7.4 + knowledge/download-mirrors.md §3/§4）：
// - 任务粒度文件级；单个大文件按 2MB/5MB/10MB 分级启用 2/4/6 线程 Range 分块
//   （D-076；服务器不支持 Range 时回退单流），文件级并发由上层自适应池控制；
// - 官方 → BMCLAPI 源回退（镜像映射见 mirror_map.hpp）；最多 3 次重试轮；
// - SHA1 优先、MD5 兜底校验；校验失败换源；
// - 全局限速令牌桶；源失败统计与禁用（BMCLAPI 403/429 限频不禁用）；
//   禁用带冷却 TTL（D-086），到期自动半开恢复，不再需要重启进程；
// - 协作式取消（std::stop_token）。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

#include "core/download/rate_limiter.hpp"
#include "core/task/task.hpp"
#include "util/result.hpp"

namespace sscl::core {

struct DownloadRequest {
    std::string url;                        // 官方 URL（内部换算镜像候选）
    std::filesystem::path destination;
    std::string expected_sha1;              // 优先校验
    std::string expected_md5;               // 无 SHA1 时兜底（§7.4/知识库）
    std::int64_t expected_size = -1;        // 清单声明大小（字节；-1 = 未知）。用于进度总量预计算
    std::string display_name;
    // D-100：调用方提示的分块线程数（0 = 按大小分级 download_piece_threads）。
    // 仅用于「小而多」的社区资源（Mod）：PCL/HMCL 从 256KB 起就分段，
    // 全部走单流时，国内高延迟链路上 1-3MB 的 Mod 会明显偏慢。
    int piece_threads = 0;
};

struct SchedulerConfig {
      int max_concurrent_downloads = 32;      // 文件级并发上限（自适应池的目标 ≤ 32）
      int max_retries = 3;                    // §7.4 最多 3 次
      std::int64_t speed_limit_bps = 0;       // 0 = 不限速
      // D-100：30s 太长 —— 连接卡死时用户会盯着「0 B/s」等半分钟。
      // PCL/HMCL 的量级是 10~15s（读空闲超时），这里取 15s，配合 3 次重试与换源。
      int request_timeout_ms = 15000;
    int source_disable_threshold = 8;       // 连续失败禁用阈值
    int source_disable_cooldown_s = 60;     // 禁用冷却（秒）；到期后清零并重新尝试
};

// 进度：(downloaded_bytes, total_bytes)，total 未知时为 0。
using DownloadProgressFn = std::function<void(std::uint64_t, std::uint64_t)>;

// 单文件分块线程数（负责人 2026-11-06 规则）：
// ≤2MB → 1（单流）；>2MB 且 ≤5MB → 2；>5MB 且 ≤10MB → 4；>10MB → 6。
// 纯函数，便于单元测试；实际并发还会受进程级分块线程预算限制。
int download_piece_threads(std::int64_t bytes);

class DownloadScheduler {
public:
    explicit DownloadScheduler(SchedulerConfig config = {});

    // 同步下载一个文件；内部完成源回退/分块/校验/原子落盘。
    util::Result<void> download_file(const DownloadRequest& request,
                                     const DownloadProgressFn& progress = nullptr,
                                     std::stop_token stop = {});

    void set_speed_limit(std::int64_t bytes_per_second);
    const SchedulerConfig& config() const { return m_config; }

    // 单源状态（测试可检）
    struct SourceState {
        int fail_count = 0;
        bool disabled = false;
        bool supports_range = true;
        // 冷却到期时间；disabled=false 时无意义（D-086）。
        std::chrono::steady_clock::time_point disabled_until{};
    };
    std::map<std::string, SourceState> source_states() const;

private:
    util::Result<void> try_source(const std::string& source_url, const DownloadRequest& request,
                                  const DownloadProgressFn& progress, std::stop_token stop);
    util::Result<void> download_single(const std::string& source_url,
                                       const std::filesystem::path& temp,
                                       const std::filesystem::path& destination,
                                       const DownloadProgressFn& progress, std::stop_token stop);

    // 单文件多线程分块：预分配临时文件，每线程一个 Range 请求 + 独立 HANDLE 定位写。
    // 服务器不支持 Range（返回 200 或分段大小不符）时返回 download.range_unsupported，
    // 由 try_source 回退单流，不影响换源/重试语义。
    util::Result<void> download_ranged(const std::string& source_url,
                                       const std::filesystem::path& temp,
                                       const DownloadRequest& request, int threads,
                                       const DownloadProgressFn& progress, std::stop_token stop);
    // progress_mutex 串行化多分块线程的进度回调：上层回调常按引用维护 last，
    // 并发调用属数据竞争（D-086）。可为 nullptr（无进度回调时）。
    util::Result<void> download_piece(const std::string& source_url,
                                      const std::filesystem::path& temp, std::int64_t start,
                                      std::int64_t end, std::int64_t total,
                                      std::atomic<std::uint64_t>* done,
                                      const DownloadProgressFn& progress,
                                      std::mutex* progress_mutex, std::stop_token stop,
                                      std::atomic<bool>* failed);
    util::Result<void> verify_hash(const DownloadRequest& request);
    void record_failure(const std::string& source_url, const util::Error& error);
    void record_success(const std::string& source_url);

    SchedulerConfig m_config;
    RateLimiter m_limiter;
    mutable std::mutex m_state_mutex;
    std::map<std::string, SourceState> m_sources;
    std::atomic<int> m_active_downloads{0};
};

// Task 包装：接入任务系统（进度/取消/重试），供 TaskExecutor 调度。
class ScheduledDownloadTask : public Task {
public:
    explicit ScheduledDownloadTask(DownloadRequest request, SchedulerConfig config = {});

protected:
    util::Result<void> do_run() override;
    void do_cancel() override;

private:
    DownloadRequest m_request;
    DownloadScheduler m_scheduler;
    std::stop_source m_stop_source;
};

}  // namespace sscl::core
