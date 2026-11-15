// 任务系统单元测试（M0 批次 3）

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/task/task.hpp"
#include "core/task/task_executor.hpp"
#include "core/task/task_group.hpp"

using sscl::core::Task;
using sscl::core::TaskExecutor;
using sscl::core::TaskGroup;
using sscl::core::TaskListener;
using sscl::core::TaskProgress;
using sscl::core::TaskState;
namespace util = sscl::util;  // 测试内简化引用（勿与命名空间 sscl::util 混淆）

namespace {

class RecordingListener : public TaskListener {
public:
    void on_state_changed(TaskState state) override {
        m_states.push_back(state);
    }
    void on_progress(const TaskProgress& progress) override {
        m_messages.push_back(progress.message);
    }

    std::vector<TaskState> m_states;
    std::vector<std::string> m_messages;
};

class OkTask : public Task {
public:
    explicit OkTask(std::string label) : m_label(std::move(label)) { set_name(m_label); }

protected:
    util::Result<void> do_run() override {
        set_progress(1, 1, m_label);
        return util::Result<void>::ok();
    }

private:
    std::string m_label;
};

class FailNTimesTask : public Task {
public:
    explicit FailNTimesTask(int fail_count) : m_fail_count(fail_count) {}

protected:
    util::Result<void> do_run() override {
        if (m_attempts++ < m_fail_count) {
            return util::Result<void>::fail(util::Error::make("test.temp_fail", "临时失败", {}));
        }
        return util::Result<void>::ok();
    }

    int m_attempts = 0;
    int m_fail_count;
};

class FailTask : public Task {
protected:
    util::Result<void> do_run() override {
        return util::Result<void>::fail(util::Error::make("test.always_fail", "总是失败", {}));
    }
};

class WaitUntilCancelledTask : public Task {
protected:
    util::Result<void> do_run() override {
        while (!is_cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return util::Result<void>::fail(cancelled_error());
    }
};

}  // namespace

TEST_CASE("task: 成功状态机与监听事件") {
    auto task = std::make_shared<OkTask>("写文件");
    RecordingListener listener;
    task->set_listener(&listener);

    auto result = task->run();
    CHECK(result.is_ok());
    CHECK(task->state() == TaskState::kSucceeded);

    REQUIRE(listener.m_states.size() == 2);
    CHECK(listener.m_states[0] == TaskState::kRunning);
    CHECK(listener.m_states[1] == TaskState::kSucceeded);

    // 单次运行
    auto again = task->run();
    CHECK(again.is_error());
    CHECK(again.error().code == "task.already_started");
}

TEST_CASE("task: 失败重试") {
    // 失败 2 次后成功，允许 2 次重试
    auto task = std::make_shared<FailNTimesTask>(2);
    auto result = task->run(2);
    CHECK(result.is_ok());
    CHECK(task->state() == TaskState::kSucceeded);

    // 无重试则失败
    auto task2 = std::make_shared<FailNTimesTask>(1);
    auto result2 = task2->run(0);
    CHECK(result2.is_error());
    CHECK(task2->state() == TaskState::kFailed);

    // 重试用尽仍失败
    auto task3 = std::make_shared<FailNTimesTask>(5);
    auto result3 = task3->run(1);
    CHECK(result3.is_error());
    CHECK(task3->state() == TaskState::kFailed);
}

TEST_CASE("task: 取消") {
    // 预先取消：不执行 do_run
    auto task = std::make_shared<OkTask>("x");
    task->cancel();
    auto result = task->run();
    CHECK(result.is_error());
    CHECK(result.error().code == "task.cancelled");
    CHECK(task->state() == TaskState::kCancelled);

    // 运行中协作式取消
    auto task2 = std::make_shared<WaitUntilCancelledTask>();
    std::thread worker([&] { (void)task2->run(); });
    while (task2->state() != TaskState::kRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    task2->cancel();
    worker.join();
    CHECK(task2->state() == TaskState::kCancelled);
}

TEST_CASE("task.group: 顺序执行、进度聚合、失败即停") {
    TaskGroup group;
    group.set_name("安装组");

    std::atomic<bool> third_ran{false};
    struct FlagTask : Task {
        std::atomic<bool>* m_flag;
        explicit FlagTask(std::atomic<bool>* flag) : m_flag(flag) {}
        util::Result<void> do_run() override {
            m_flag->store(true);
            return util::Result<void>::ok();
        }
    };

    auto first = std::make_shared<OkTask>("步骤1");
    auto second = std::make_shared<FailTask>();
    auto third = std::make_shared<FlagTask>(&third_ran);

    group.add(first);
    group.add(second);
    group.add(third);

    auto result = group.run();
    CHECK(result.is_error());
    CHECK(result.error().code == "test.always_fail");
    CHECK(group.state() == TaskState::kFailed);
    CHECK(!third_ran.load());  // 第二个子任务失败后不再继续
}

TEST_CASE("task.group: 全部成功") {
    TaskGroup group;
    RecordingListener listener;
    group.set_listener(&listener);
    group.add(std::make_shared<OkTask>("a"));
    group.add(std::make_shared<OkTask>("b"));

    auto result = group.run();
    CHECK(result.is_ok());
    CHECK(group.state() == TaskState::kSucceeded);
    // message 应包含子任务名
    bool has_a = false;
    bool has_b = false;
    for (const std::string& m : listener.m_messages) {
        if (m == "a") has_a = true;
        if (m == "b") has_b = true;
    }
    CHECK(has_a);
    CHECK(has_b);
}

TEST_CASE("task.executor: 异步提交") {
    TaskExecutor executor(2);
    std::promise<void> done;
    std::future<void> finished = done.get_future();

    struct PromiseTask : Task {
        std::promise<void>* m_done;
        explicit PromiseTask(std::promise<void>* d) : m_done(d) {}
        util::Result<void> do_run() override {
            m_done->set_value();
            return util::Result<void>::ok();
        }
    };

    executor.submit(std::make_shared<PromiseTask>(&done));
    CHECK(finished.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
}
