#pragma once

// 启动命令组装（knowledge/launch-args.md §1/§2；开发文档 §7.1）。
// - 组装顺序：内存 → 编码 → log4j → 默认/版本 JVM 参数 → mainClass → 游戏参数；
// - 占位符全表替换（键包含 ${}，未知占位符原样保留）；
// - 超长命令行写 Java 9+ @argfile（§7.1，HMCL 未实现项）。

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "core/launch/version_json.hpp"
#include "util/result.hpp"

namespace sscl::core {

struct LaunchAccount {
    std::string player_name;
    std::string uuid_compact;  // 32 位无连字符
    std::string access_token;
    std::string user_type = "msa";       // OPEN_QUESTIONS #16 默认
    std::string user_properties = "{}";
};

struct LaunchPaths {
    std::filesystem::path java_exe;
    std::filesystem::path game_dir;        // 运行目录
    std::filesystem::path natives_dir;
    std::filesystem::path assets_dir;
    std::filesystem::path libraries_dir;
    std::filesystem::path version_jar;     // 客户端 jar（primary_jar）
    bool has_log4j = false;
    std::filesystem::path log4j_config;
};

struct LaunchSettings {
    int memory_mb = 1024;
    int width = 0;
    int height = 0;
    bool fullscreen = false;
    std::string language = "zh-CN";
    // D-107「辅助功能」：启动前把 options.txt 的 lang 设为 zh_cn（默认开）。
    // 关闭时不改动玩家自己的语言设置。
    bool auto_chinese_language = true;
    int java_major_version = 0;  // 0 = 未知；影响编码参数形态

    // Java 选择（M3，config.json 透传）：java_path 非空 = 手动指定（java.exe/bin/根目录）；
    // java_scan_dirs = 额外扫描根（便携 JDK，如 "D:\\JDK"）。
    std::string java_path;
    std::vector<std::string> java_scan_dirs;
    // D-115：设置页排好的 Java 优先级（有序），启动时按顺序挑第一个满足版本要求的。
    std::vector<std::string> java_candidates;

    // V5 每版本覆盖：JVM/游戏追加参数、GC 模式、游戏内信息、窗口标题。
    std::string jvm_args;
    std::string game_args;
    int gc_mode = 0;  // 0 跟随全局 / 1 ZGC / 2 分代 ZGC / 3 G1 / 4 调优 G1 / 5 不指定
    std::string version_info;
    std::string window_title;
    std::string server_enter;

    // D-104 全局启动选项（由设置页写入 LaunchRequest.settings）：
    // process_priority 0 低 / 1 中 / 2 高；maximize_window 交给启动后的窗口 watcher。
    int process_priority = 1;
    bool maximize_window = false;
};

struct LaunchContext {
    LaunchAccount account;
    LaunchPaths paths;
    LaunchSettings settings;
    std::string version_name;
    std::string version_type;
    std::string launcher_name = "SSCL";
    std::string launcher_version = "0.1.0";
    std::string client_id;   // ${clientid}：微软应用 Client ID（C-7，Axolotl 方案）
    std::string auth_xuid = "0";  // ${auth_xuid}：无 Xbox xuid 时 "0"（C-7）
    OsInfo os;
    std::map<std::string, bool> features;
};

struct LaunchPlan {
    std::vector<std::string> arguments;  // 不含 java.exe；jvm + mainClass + game
    std::string command_line;            // 完整命令行（Windows 引用规则）
    bool used_argfile = false;
    std::filesystem::path argfile_path;

    // D-104：不走命令行的启动后行为（由 launch_game 应用）。
    int process_priority = 1;       // → SetPriorityClass
    bool maximize_window = false;   // → ShowWindow(SW_MAXIMIZE)
    std::string window_title;       // → SetWindowTextW（空 = 不更改）
};

// 按顺序组装启动计划；mainClass 缺失返回 launch.main_class_missing。
util::Result<LaunchPlan> build_launch_plan(const VersionJson& version, const LaunchContext& context);

// 占位符替换：values 的键包含 ${}；未知占位符原样保留（StringArgument.java:45-53 行为）。
std::string replace_placeholders(const std::string& text,
                                 const std::map<std::string, std::string>& values);

// 生成占位符值表（launch-args.md §2 全表）。
std::map<std::string, std::string> build_placeholder_map(const VersionJson& version,
                                                         const LaunchContext& context,
                                                         const std::string& classpath);

// classpath：rules 允许的库 + 客户端 jar 末尾（`;` 连接）。
std::string build_classpath(const VersionJson& version, const LaunchContext& context);

// 写 Java 9+ argfile（UTF-8）；命令行使用 `java @<file>`。
util::Result<std::filesystem::path> write_argfile(const std::filesystem::path& file,
                                                  const std::vector<std::string>& arguments);

}  // namespace sscl::core
