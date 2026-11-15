#pragma once

// 后台任务中心（D-098）：
// - 「附加」页的 Mod 下载/安装不再占用前台页面，统一提交到这里由工作线程执行；
// - 任务与页面解耦：切页、关弹层、退出详情都不影响下载继续；
// - 下载页（TaskPage）与右下角悬浮窗只订阅快照，不拥有任务；
// - 进度上报自行节流（≈12Hz），避免每次分块回调都推一次 UI 重绘。
//
// 线程模型：submit() 在调用线程登记并起一个 detach 线程；work 在该线程执行；
// listener 也在该线程触发，UI 侧必须自行 post 到 UI 线程（§5.1）。

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

#include "util/result.hpp"

namespace sscl::core {

enum class JobState {
    kQueued,
    kRunning,
    kSucceeded,
    kFailed,
    kCancelled,
};

struct JobProgress {
    std::uint64_t done_bytes = 0;
    std::uint64_t total_bytes = 0;  // 0 = 未知
    std::uint64_t current_done = 0; // 当前文件已下载
    std::uint64_t current_total = 0;
    int done_files = 0;
    int total_files = 0;
    std::string phase;         // 阶段文案（"下载并安装 Mod"）
    std::string current_file;  // 当前文件（显示在实时区/终端）

    float fraction() const {
        if (total_bytes > 0) {
            const float value = static_cast<float>(
                static_cast<double>(done_bytes) / static_cast<double>(total_bytes));
            return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        }
        if (total_files > 0) {
            const float value = static_cast<float>(
                static_cast<double>(done_files) / static_cast<double>(total_files));
            return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        }
        return 0.0f;
    }
};

struct JobSnapshot {
    std::uint64_t id = 0;
    std::string title;     // "安装 Mod 到 1.20.1-forge"
    std::string subtitle;  // 目标实例 / 文件说明
    JobState state = JobState::kQueued;
    JobProgress progress;
    std::string message;  // 结束文案（失败原因 / "已取消"）

    bool valid() const { return id != 0; }
    bool finished() const {
        return state == JobState::kSucceeded || state == JobState::kFailed ||
               state == JobState::kCancelled;
    }
};

class TaskCenter;

// 工作体上下文：取消检查 + 进度上报（可在任意线程调用）。
class JobContext {
public:
    bool cancelled() const;
    std::stop_token stop_token() const;
    void report(const JobProgress& progress);
    void report(int done_files, int total_files, std::uint64_t done_bytes,
                std::uint64_t total_bytes, std::string current_file, std::string phase = {});

private:
    friend class TaskCenter;
    TaskCenter* m_center = nullptr;
    std::uint64_t m_id = 0;
};

// 返回 ok → kSucceeded；返回错误码含 "cancelled" → kCancelled；其余 → kFailed。
using JobWork = std::function<util::Result<void>(JobContext&)>;

class TaskCenter {
public:
    static TaskCenter& instance();

    TaskCenter(const TaskCenter&) = delete;
    TaskCenter& operator=(const TaskCenter&) = delete;

    // 提交后台任务，返回任务 id（0 = 失败，不会发生）。
    std::uint64_t submit(std::string title, std::string subtitle, JobWork work);

    // 请求取消（线程安全）；任务真正结束仍以工作体返回为准。
    bool cancel(std::uint64_t id);

    bool has_active() const;
    JobSnapshot active() const;               // 最近一个未结束任务
    JobSnapshot job(std::uint64_t id) const;  // 指定任务（含已结束）
    std::vector<JobSnapshot> jobs() const;    // 全部任务，新→旧

    // 进度/状态变化回调（任意线程触发）；UI 侧自行 post_to_ui。
    void set_listener(std::function<void()> listener);
    void clear_listener();

private:
    TaskCenter() = default;

    struct Record;

    void update_progress(std::uint64_t id, JobProgress progress);
    bool is_cancelled(std::uint64_t id) const;
    std::stop_token token_of(std::uint64_t id) const;
    void notify();
    void trim_locked();

    mutable std::mutex m_mutex;
    std::map<std::uint64_t, std::shared_ptr<Record>> m_jobs;
    std::function<void()> m_listener;
    std::uint64_t m_next_id = 0;

    friend class JobContext;
};

// 状态文案（日志/调试用）。
const char* job_state_name(JobState state);

}  // namespace sscl::core
