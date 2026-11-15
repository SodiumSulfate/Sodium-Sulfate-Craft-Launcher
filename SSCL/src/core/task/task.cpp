#include "core/task/task.hpp"

namespace sscl::core {

util::Result<void> Task::run(int max_retries) {
    if (max_retries < 0) {
        max_retries = 0;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state != TaskState::kPending) {
            return util::Result<void>::fail(
                util::Error::make("task.already_started", "任务已启动，不可重复运行", m_name));
        }
        m_state = TaskState::kRunning;
    }
    set_state_and_notify(TaskState::kRunning);

    if (m_cancelled.load()) {
        set_state_and_notify(TaskState::kCancelled);
        return util::Result<void>::fail(cancelled_error());
    }

    util::Result<void> result = util::Result<void>::ok();
    for (int attempt = 0;; ++attempt) {
        result = do_run();
        if (result.is_ok() || m_cancelled.load() || attempt >= max_retries) {
            break;
        }
    }

    if (result.is_ok()) {
        set_state_and_notify(TaskState::kSucceeded);
        return result;
    }
    if (m_cancelled.load()) {
        set_state_and_notify(TaskState::kCancelled);
        return util::Result<void>::fail(cancelled_error());
    }
    set_state_and_notify(TaskState::kFailed);
    return result;
}

void Task::cancel() {
    m_cancelled.store(true);
    do_cancel();
}

bool Task::is_cancelled() const {
    return m_cancelled.load();
}

TaskState Task::state() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

TaskProgress Task::progress() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_progress;
}

void Task::set_listener(TaskListener* listener) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_listener = listener;
}

void Task::set_name(std::string name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_name = std::move(name);
}

std::string Task::name() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_name;
}

util::Error Task::cancelled_error() {
    return util::Error::make("task.cancelled", "任务已取消", {});
}

void Task::set_progress(std::uint64_t current, std::uint64_t total, std::string message) {
    TaskProgress progress;
    progress.current = current;
    progress.total = total;
    progress.message = std::move(message);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress = progress;
    }
    notify_progress(progress);
}

void Task::set_state_and_notify(TaskState state) {
    TaskListener* listener = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = state;
        listener = m_listener;
    }
    if (listener != nullptr) {
        listener->on_state_changed(state);
    }
}

void Task::notify_progress(const TaskProgress& progress) {
    TaskListener* listener = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        listener = m_listener;
    }
    if (listener != nullptr) {
        listener->on_progress(progress);
    }
}

}  // namespace sscl::core
