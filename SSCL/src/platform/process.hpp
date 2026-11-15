#pragma once

// 进程工具：运行外部进程并捕获输出（Java 检测、加载器安装处理器等场景使用）。
// 游戏本体的托管启动（流式日志、守护）在 M1 另建接口，不在此文件。

#include <cstddef>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "util/result.hpp"

namespace sscl::platform {

struct ProcessResult {
    int exit_code = -1;
    std::string stdout_text;  // UTF-8（从系统 ANSI 代码页转换）
    std::string stderr_text;  // UTF-8（从系统 ANSI 代码页转换）
};

// 按 Windows CreateProcess 引用规则给单个参数加引号/转义（UTF-8 进 UTF-8 出）。
std::string quote_process_arg(std::string_view arg);

// 运行 executable（可用 "cmd.exe" 这类名字，由系统搜索），等待结束并收集输出。
// arguments 为原始参数（不含 exe 本身）；working_dir 为空则继承当前目录。
// timeout_ms < 0 表示无限等待；超时会强制结束进程并返回 code=process.timeout
// （detail 中带已收集的输出）。隐藏子进程控制台窗口（CREATE_NO_WINDOW）。
util::Result<ProcessResult> run_process(const std::filesystem::path& executable,
                                        const std::vector<std::string>& arguments,
                                        const std::filesystem::path& working_dir,
                                        int timeout_ms);

// 扩展选项：协作式取消 + 逐行输出回调 + 捕获上限（加载器处理器需要，design §6.6）。
struct ProcessOptions {
    int timeout_ms = -1;   // <0 无限
    std::stop_token stop{};  // 置位 ⇒ 终止子进程并返回 process.cancelled

    // 逐行回调（去掉行尾 \r；UTF-8）。**在调用 run_process_ex 的线程上调用**（非读线程），
    // 因此回调内可直接访问调用方状态；不要从回调里再调用 run_process_ex 造成重入等待。
    std::function<void(const std::string& line)> on_output;

    // 每路输出保留的尾部字节上限（超出丢弃更早的内容）；0 = 不保留。
    std::size_t max_capture_bytes = 1u << 20;
};

// 与 run_process 相同，但支持取消/逐行回调/捕获上限。
// 错误码：process.timeout（超时）、process.cancelled（取消）、process.create_failed、process.spawn_failed。
util::Result<ProcessResult> run_process_ex(const std::filesystem::path& executable,
                                           const std::vector<std::string>& arguments,
                                           const std::filesystem::path& working_dir,
                                           const ProcessOptions& options);

}  // namespace sscl::platform
