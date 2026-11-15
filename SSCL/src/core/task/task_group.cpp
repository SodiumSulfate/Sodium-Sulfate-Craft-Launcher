#include "core/task/task_group.hpp"

#include <utility>

namespace sscl::core {

void TaskGroup::add(std::shared_ptr<Task> child) {
    if (child == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_children_mutex);
    m_children.push_back(std::move(child));
}

util::Result<void> TaskGroup::do_run() {
    std::vector<std::shared_ptr<Task>> children;
    {
        std::lock_guard<std::mutex> lock(m_children_mutex);
        children = m_children;
    }

    const std::uint64_t total = children.size();
    set_progress(0, total, {});

    for (std::size_t i = 0; i < children.size(); ++i) {
        if (is_cancelled()) {
            return util::Result<void>::fail(cancelled_error());
        }
        Task& child = *children[i];
        set_progress(i, total, child.name());

        util::Result<void> result = child.run();
        if (result.is_error()) {
            return result;
        }
    }

    set_progress(total, total, {});
    return util::Result<void>::ok();
}

void TaskGroup::do_cancel() {
    std::vector<std::shared_ptr<Task>> children;
    {
        std::lock_guard<std::mutex> lock(m_children_mutex);
        children = m_children;
    }
    for (const std::shared_ptr<Task>& child : children) {
        child->cancel();
    }
}

}  // namespace sscl::core
