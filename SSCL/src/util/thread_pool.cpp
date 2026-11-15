#include "util/thread_pool.hpp"

#include <utility>

namespace sscl::util {

ThreadPool::ThreadPool(std::size_t thread_count) {
    if (thread_count == 0) {
        thread_count = 1;
    }
    m_threads.reserve(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i) {
        m_threads.emplace_back([this] { worker_loop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    for (std::thread& thread : m_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void ThreadPool::submit(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.push(std::move(job));
    }
    m_cv.notify_one();
}

std::size_t ThreadPool::pending_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_jobs.size();
}

void ThreadPool::worker_loop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
            if (m_stop && m_jobs.empty()) {
                return;  // 停机且队列已清空
            }
            job = std::move(m_jobs.front());
            m_jobs.pop();
        }
        job();
    }
}

}  // namespace sscl::util
