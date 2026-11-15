#pragma once

// 子任务组合（开发文档 §5.1）：按加入顺序依次执行；任一子任务失败即整体失败。
// 进度按“已完成子任务数/总数”聚合，message 为当前子任务名。

#include <memory>
#include <vector>

#include "core/task/task.hpp"

namespace sscl::core {

class TaskGroup : public Task {
public:
    // 运行前调用；运行中加入的行为未定义。
    void add(std::shared_ptr<Task> child);

protected:
    util::Result<void> do_run() override;
    void do_cancel() override;

private:
    std::mutex m_children_mutex;
    std::vector<std::shared_ptr<Task>> m_children;
};

}  // namespace sscl::core
