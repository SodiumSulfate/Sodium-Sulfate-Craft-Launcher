#pragma once

// Task 抽象（开发文档 §5.1）：所有耗时操作（下载、安装、启动前检查）统一为 Task，
// 支持进度、取消、失败重试、子任务组合；UI 只订阅状态（on_state_changed / on_progress）。
//
// 线程模型：run() 在工作线程执行；listener 回调发生在工作线程，
// UI 侧适配器须自行 PostMessage 回 UI 线程后再更新控件（§5.1）。

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

#include "util/result.hpp"

namespace sscl::core {

enum class TaskState {
    kPending,
    kRunning,
    kSucceeded,
    kFailed,
    kCancelled,
};

struct TaskProgress {
    std::uint64_t current = 0;
    std::uint64_t total = 0;  // 0 表示总量未知（显示不确定进度）
    std::string message;      // 当前阶段描述（用户可读）
};

// 任务事件订阅者。回调发生在工作线程；实现方不拥有监听器，须保证生命周期覆盖任务执行期。
class TaskListener {
public:
    virtual ~TaskListener() = default;
    virtual void on_state_changed(TaskState state) { (void)state; }
    virtual void on_progress(const TaskProgress& progress) { (void)progress; }
};

class Task {
public:
    Task() = default;
    virtual ~Task() = default;

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    // 单次运行（在工作线程或调用方线程执行）。
    // max_retries：do_run 失败后的额外重试次数（取消不重试）。
    // 成功返回 ok；失败/取消返回对应 Error（code：task.cancelled / 子类自定义）。
    util::Result<void> run(int max_retries = 0);

    // 请求取消（线程安全）；已取消/已完成时无副作用。
    void cancel();

    bool is_cancelled() const;

    TaskState state() const;
    TaskProgress progress() const;

    void set_listener(TaskListener* listener);
    void set_name(std::string name);
    std::string name() const;

    // 取消任务的标准错误（子类返回失败时可复用）。
    static util::Error cancelled_error();

protected:
    // 子类实现实际工作；应定期检查 is_cancelled() 并尽快返回。
    virtual util::Result<void> do_run() = 0;

    // 取消时的资源清理（可选；在 cancel() 调用线程执行）。
    virtual void do_cancel() {}

    // 更新进度并通知监听器（线程安全）。
    void set_progress(std::uint64_t current, std::uint64_t total, std::string message = {});

private:
    void set_state_and_notify(TaskState state);
    void notify_progress(const TaskProgress& progress);

    mutable std::mutex m_mutex;
    TaskState m_state = TaskState::kPending;
    TaskProgress m_progress;
    TaskListener* m_listener = nullptr;
    std::atomic<bool> m_cancelled{false};
    std::string m_name;
};

}  // namespace sscl::core
