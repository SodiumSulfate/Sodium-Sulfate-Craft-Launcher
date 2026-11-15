#pragma once

// 简单固定线程池（开发文档 §5：util 层；任务系统与下载器共用）。
// 析构时：停止接收新任务，把队列中已有任务执行完再退出。

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace sscl::util {

class ThreadPool {
public:
    explicit ThreadPool(std::size_t thread_count);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 投递任务；线程安全。
    void submit(std::function<void()> job);

    // 当前排队中（未开始执行）的任务数。
    std::size_t pending_count() const;

private:
    void worker_loop();

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::queue<std::function<void()>> m_jobs;
    std::vector<std::thread> m_threads;
    bool m_stop = false;
};

}  // namespace sscl::util
