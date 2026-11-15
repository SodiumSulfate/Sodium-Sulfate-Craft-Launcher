#pragma once

// 游戏启动编排（knowledge/version-json.md §2 环检测 + launch-args.md §4）。
// - resolve_version_chain：按 inheritsFrom 递归加载合并，环检测；
// - launch_game：创建可见的游戏进程（不用 CREATE_NO_WINDOW）。

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core/launch/launch_command.hpp"
#include "core/launch/version_json.hpp"
#include "util/result.hpp"

namespace sscl::core {

// 版本加载器：按 id 返回版本 JSON 文本（通常读 versions/<id>/<id>.json）。
using VersionTextLoader = std::function<util::Result<std::string>(const std::string& id)>;

// 解析 inheritsFrom 链并合并（子在下、父在上；环检测 → version.inherit_cycle）。
util::Result<VersionJson> resolve_version_chain(const VersionTextLoader& loader,
                                                const std::string& root_id);

struct GameProcess {
    unsigned long pid = 0;
    void* process_handle = nullptr;
    void* thread_handle = nullptr;
};

// 创建游戏进程（CREATE_NO_WINDOW：不弹命令行窗口，游戏图形窗口正常显示；
// 命令行来自 LaunchPlan）。
util::Result<GameProcess> launch_game(const LaunchPlan& plan,
                                      const std::filesystem::path& working_dir);

// 关闭句柄（进程退出后调用）。
void close_game_process(GameProcess& process);

}  // namespace sscl::core
