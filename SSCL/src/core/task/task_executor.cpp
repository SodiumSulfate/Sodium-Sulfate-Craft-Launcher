#include "core/task/task_executor.hpp"

#include <utility>

namespace sscl::core {

TaskExecutor::TaskExecutor(std::size_t thread_count) : m_pool(thread_count) {}

TaskExecutor::~TaskExecutor() = default;

void TaskExecutor::submit(std::shared_ptr<Task> task, int max_retries) {
    if (task == nullptr) {
        return;
    }
    m_pool.submit([task = std::move(task), max_retries] { (void)task->run(max_retries); });
}

std::size_t TaskExecutor::pending_count() const {
    return m_pool.pending_count();
}

}  // namespace sscl::core
