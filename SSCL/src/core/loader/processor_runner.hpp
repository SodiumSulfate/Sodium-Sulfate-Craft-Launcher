#pragma once

// 安装器 processors 执行（knowledge/mod-loader-install.md §5.1）：
//   java -cp <classpath;processor.jar> <jar 清单 Main-Class> <args...>
//   工作目录 = 游戏根；输出已就绪（SHA1 一致）即跳过；退出码非 0 失败。
// 本单元只依赖 platform（zip/process/hash），不负责下载：MOJMAPS 由回调交给上层下载。

#include <filesystem>
#include <functional>
#include <map>
#include <stop_token>
#include <string>
#include <vector>

#include "core/launch/version_json.hpp"
#include "util/result.hpp"

namespace sscl::core {

// 处理器运行环境（变量已解析成最终字符串；不含花括号）。
struct ProcessorEnvironment {
    std::filesystem::path game_dir;        // ROOT：工作目录
    std::filesystem::path libraries_dir;   // LIBRARY_DIR
    std::filesystem::path installer_path;  // INSTALLER
    std::filesystem::path minecraft_jar;   // MINECRAFT_JAR
    std::string minecraft_version;         // MINECRAFT_VERSION
    std::string java_executable;
    std::map<std::string, std::string> variables;  // data 的 client 取值 + 内置变量
};

struct ProcessorCommand {
    std::vector<std::filesystem::path> classpath;  // 已解析的 jar 路径（不含 -cp 分隔）
    std::string main_class;
    std::vector<std::string> args;
};

// install_profile.data 取值解析：'literal' → 字面量；[coords] → 库文件路径；
// /path/in/jar → 从安装器抽出到 temp_dir；其它 → 原样返回。
util::Result<std::string> resolve_installer_data_value(
    const std::string& raw, const std::filesystem::path& installer_path,
    const std::filesystem::path& libraries_dir, const std::filesystem::path& temp_dir,
    std::stop_token stop = {});

// 单条文本解析：{KEY} → 变量；[coords] → 库路径；'literal' → 去引号；其它 → 全文替换 {KEY}。
util::Result<std::string> resolve_processor_text(const std::string& text,
                                                 const ProcessorEnvironment& env);

// 构造单个处理器的命令（读 processor jar 的 Main-Class）；classpath = profile 的 classpath + jar。
util::Result<ProcessorCommand> build_processor_command(const InstallerProcessor& processor,
                                                       const ProcessorEnvironment& env);

using ProcessorLineFn = std::function<void(const std::string&)>;
// 处理器进度回调：index（0 起）/ total / 可读标签（task 名或 jar 名），用于"不要干等"提示。
using ProcessorProgressFn =
    std::function<void(std::size_t index, std::size_t total, const std::string& label)>;
// MOJMAPS 下载器：输出路径 + 期望 SHA1（可能为空）。返回成功即视为已就绪。
using MojmapsDownloadFn =
    std::function<util::Result<void>(const std::filesystem::path&, const std::string&)>;

// 顺序执行 client 侧 processors（幂等：输出就绪则跳过）。on_line 收到子进程每行输出。
util::Result<void> run_processors(const std::vector<InstallerProcessor>& processors,
                                  const ProcessorEnvironment& env, std::stop_token stop,
                                  const ProcessorLineFn& on_line,
                                  const MojmapsDownloadFn& download_mojmaps,
                                  const ProcessorProgressFn& on_progress = nullptr);

}  // namespace sscl::core
