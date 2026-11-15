#include "core/task/task_center.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

#include "util/log.hpp"

namespace sscl::core {

namespace {

constexpr std::size_t kMaxJobs = 12;             // 保留最近任务条数（含已结束）
constexpr auto kNotifyInterval = std::chrono::milliseconds(100);  // ≈10Hz，与下载页刷新同频

JobSnapshot make_snapshot(const std::uint64_t id, const std::string& title,
                          const std::string& subtitle, const JobState state,
                          const JobProgress& progress, const std::string& message) {
    JobSnapshot snap;
    snap.id = id;
    snap.title = title;
    snap.subtitle = subtitle;
    snap.state = state;
    snap.progress = progress;
    snap.message = message;
    return snap;
}

bool cancelled_code(const std::string& code) {
    return code.find("cancelled") != std::string::npos ||
           code.find("canceled") != std::string::npos;
}

}  // namespace

struct TaskCenter::Record {
    std::uint64_t id = 0;
    std::string title;
    std::string subtitle;
    std::string message;
    JobState state = JobState::kQueued;
    JobProgress progress;
    std::stop_source stop;
    std::chrono::steady_clock::time_point last_notify{};
    std::string last_file;

    JobSnapshot snapshot() const {
        return make_snapshot(id, title, subtitle, state, progress, message);
    }
};

// ————————————————————— JobContext —————————————————————

bool JobContext::cancelled() const {
    return m_center != nullptr && m_center->is_cancelled(m_id);
}

std::stop_token JobContext::stop_token() const {
    return m_center != nullptr ? m_center->token_of(m_id) : std::stop_token{};
}

void JobContext::report(const JobProgress& progress) {
    if (m_center != nullptr) {
        m_center->update_progress(m_id, progress);
    }
}

void JobContext::report(const int done_files, const int total_files,
                        const std::uint64_t done_bytes, const std::uint64_t total_bytes,
                        std::string current_file, std::string phase) {
    JobProgress progress;
    progress.done_files = done_files;
    progress.total_files = total_files;
    progress.done_bytes = done_bytes;
    progress.total_bytes = total_bytes;
    progress.current_file = std::move(current_file);
    progress.phase = std::move(phase);
    report(progress);
}

// ————————————————————— TaskCenter —————————————————————

TaskCenter& TaskCenter::instance() {
    static TaskCenter center;
    return center;
}

std::uint64_t TaskCenter::submit(std::string title, std::string subtitle, JobWork work) {
    auto record = std::make_shared<Record>();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        record->id = ++m_next_id;
        record->title = std::move(title);
        record->subtitle = std::move(subtitle);
        record->state = JobState::kQueued;
        m_jobs[record->id] = record;
        trim_locked();
    }
    const std::uint64_t id = record->id;
    util::log_info("[task] submit #" + std::to_string(id) + " " + record->title);
    notify();

    std::thread([this, record, work = std::move(work)]() mutable {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (record->state != JobState::kQueued) {
                return;  // 提交后立刻被取消
            }
            record->state = JobState::kRunning;
            record->last_notify = std::chrono::steady_clock::now();
        }
        notify();

        JobContext context;
        context.m_center = this;
        context.m_id = record->id;

        util::Result<void> result = util::Result<void>::ok();
        try {
            if (work) {
                result = work(context);
            }
        } catch (...) {
            // 核心层不抛异常（§5.1）：工作体泄漏的异常一律记为失败。
            result = util::Result<void>::fail(
                util::Error::make("task.exception", "后台任务异常终止"));
        }

        JobState state = JobState::kSucceeded;
        std::string message;
        if (result.is_error()) {
            if (cancelled_code(result.error().code)) {
                state = JobState::kCancelled;
                message = "已取消";
            } else {
                state = JobState::kFailed;
                message = result.error().message.empty() ? result.error().code
                                                         : result.error().message;
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            record->state = state;
            record->message = message;
            if (state == JobState::kSucceeded && record->progress.total_bytes > 0) {
                record->progress.done_bytes = record->progress.total_bytes;
                record->progress.done_files = record->progress.total_files;
            }
        }
        util::log_info("[task] done #" + std::to_string(record->id) + " state=" +
                       job_state_name(state));
        notify();
    }).detach();

    return id;
}

bool TaskCenter::cancel(const std::uint64_t id) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_jobs.find(id);
        if (it == m_jobs.end()) {
            return false;
        }
        Record& record = *it->second;
        if (record.state != JobState::kRunning && record.state != JobState::kQueued) {
            return false;
        }
        record.stop.request_stop();
    }
    notify();
    return true;
}

bool TaskCenter::has_active() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& entry : m_jobs) {
        const JobState state = entry.second->state;
        if (state == JobState::kRunning || state == JobState::kQueued) {
            return true;
        }
    }
    return false;
}

JobSnapshot TaskCenter::active() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    JobSnapshot best;
    for (const auto& entry : m_jobs) {
        const Record& record = *entry.second;
        if (record.state != JobState::kRunning && record.state != JobState::kQueued) {
            continue;
        }
        if (!best.valid() || record.id > best.id) {
            best = record.snapshot();
        }
    }
    return best;
}

JobSnapshot TaskCenter::job(const std::uint64_t id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_jobs.find(id);
    if (it == m_jobs.end()) {
        return JobSnapshot{};
    }
    return it->second->snapshot();
}

std::vector<JobSnapshot> TaskCenter::jobs() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<JobSnapshot> list;
    list.reserve(m_jobs.size());
    for (auto it = m_jobs.rbegin(); it != m_jobs.rend(); ++it) {
        list.push_back(it->second->snapshot());
    }
    return list;
}

void TaskCenter::set_listener(std::function<void()> listener) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_listener = std::move(listener);
}

void TaskCenter::clear_listener() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_listener = nullptr;
}

void TaskCenter::update_progress(const std::uint64_t id, JobProgress progress) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_jobs.find(id);
        if (it == m_jobs.end()) {
            return;
        }
        Record& record = *it->second;
        record.progress = std::move(progress);
        record.last_file = record.progress.current_file;

        const auto now = std::chrono::steady_clock::now();
        if (record.last_notify.time_since_epoch().count() != 0 &&
            now - record.last_notify < kNotifyInterval) {
            return;  // 数据已更新，通知按 12Hz 节流
        }
        record.last_notify = now;
        changed = true;
    }
    if (changed) {
        notify();
    }
}

bool TaskCenter::is_cancelled(const std::uint64_t id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_jobs.find(id);
    if (it == m_jobs.end()) {
        return true;
    }
    return it->second->stop.stop_requested();
}

std::stop_token TaskCenter::token_of(const std::uint64_t id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_jobs.find(id);
    if (it == m_jobs.end()) {
        return std::stop_token{};
    }
    return it->second->stop.get_token();
}

void TaskCenter::notify() {
    std::function<void()> listener;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        listener = m_listener;
    }
    if (listener) {
        listener();
    }
}

void TaskCenter::trim_locked() {
    if (m_jobs.size() <= kMaxJobs) {
        return;
    }
    for (auto it = m_jobs.begin(); it != m_jobs.end() && m_jobs.size() > kMaxJobs;) {
        const JobState state = it->second->state;
        if (state == JobState::kRunning || state == JobState::kQueued) {
            ++it;
            continue;
        }
        it = m_jobs.erase(it);
    }
}

const char* job_state_name(const JobState state) {
    switch (state) {
        case JobState::kQueued:
            return "queued";
        case JobState::kRunning:
            return "running";
        case JobState::kSucceeded:
            return "succeeded";
        case JobState::kFailed:
            return "failed";
        case JobState::kCancelled:
            return "cancelled";
    }
    return "unknown";
}

}  // namespace sscl::core
