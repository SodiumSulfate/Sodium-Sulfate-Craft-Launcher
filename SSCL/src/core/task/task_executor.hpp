#pragma once

// 任务执行器：把 Task 提交到线程池异步执行（失败重试由 Task::run(max_retries) 支持）。
// UI 只订阅 Task 的 listener，不直接等待本对象（§5.1）。

#include <cstddef>
#include <memory>

#include "core/task/task.hpp"
#include "util/thread_pool.hpp"

namespace sscl::core {

class TaskExecutor {
public:
    explicit TaskExecutor(std::size_t thread_count);
    ~TaskExecutor();

    TaskExecutor(const TaskExecutor&) = delete;
    TaskExecutor& operator=(const TaskExecutor&) = delete;

    // 异步执行；max_retries 为失败后的额外重试次数（取消不重试）。
    // task 由执行器持有（shared_ptr）直到运行结束。
    void submit(std::shared_ptr<Task> task, int max_retries = 0);

    // 当前排队中任务数。
    std::size_t pending_count() const;

private:
    util::ThreadPool m_pool;
};

}  // namespace sscl::core
