#pragma once

// Mod 安装计划与执行（M4-3，D-058）：
// - build_install_plan：纯本地决策（不联网），输入已解析的“根 + 依赖”版本；
//   负责主文件选择、文件名清洗、目标目录、同名/同哈希冲突、重复计划。
// - install_mod_plan：按计划下载 → 校验 → 备份 `.old` → 原子替换，支持取消/进度。
//
// 依赖的“联网解析”由上层 resolver 负责（后续 ModInstallService），
// 这样计划逻辑可以完全离线单测。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

#include "core/download/download_scheduler.hpp"
#include "core/game/instance_paths.hpp"
#include "core/mod/mod_scanner.hpp"
#include "core/modplatform/mod_types.hpp"
#include "util/result.hpp"

namespace sscl::core {

enum class ModPlanAction {
    kInstall = 0,
    kSkipSame = 1,     // 同名且同 SHA1：已安装
    kConflict = 2,     // 同名不同内容：需用户确认覆盖
    kUnavailable = 3,  // 无文件/禁止分发/文件名非法
};

enum class ModPlanRole {
    kUser = 0,
    kRequired = 1,
    kOptional = 2,
    kEmbedded = 3,
};

struct ModResolved {
    ModProject project;
    ModVersion version;
    ModPlanRole role = ModPlanRole::kUser;
    int depth = 0;
};

struct ModPlanItem {
    ModProject project;
    ModVersion version;
    ModFile file;
    std::string file_name;  // 清洗后的安全文件名
    std::filesystem::path target_path;
    ModPlanAction action = ModPlanAction::kInstall;
    ModPlanRole role = ModPlanRole::kUser;
    std::string note;
};

struct ModPlanNotice {
    std::string project_id;
    std::string title;
    ModPlanRole role = ModPlanRole::kOptional;
    std::string reason;
};

struct ModInstallPlan {
    std::vector<ModPlanItem> items;
    std::vector<ModPlanNotice> notices;
    int install_count = 0;
    int skip_count = 0;
    int conflict_count = 0;
    int unavailable_count = 0;

    bool has_conflicts() const { return conflict_count > 0; }
};

struct ModInstallOptions {
    // 已由用户确认覆盖；false 时 install_mod_plan 遇到 conflict 直接失败。
    bool overwrite_conflicts = false;
    int max_dependency_depth = 8;
};

// 第一个元素视为根 Mod；后续为依赖。按顺序决策，重复 project_id 跳过。
ModInstallPlan build_install_plan(const InstanceTarget& target,
                                  const std::vector<ModResolved>& resolved,
                                  const ModInstallOptions& options,
                                  ModHashCache& hash_cache);

struct ModInstallProgress {
    int completed_files = 0;
    int total_files = 0;
    std::string current_file;
    std::uint64_t current_done = 0;
    std::uint64_t current_total = 0;
    std::uint64_t total_done = 0;
    std::uint64_t total_total = 0;
};

using ModInstallProgressFn = std::function<void(const ModInstallProgress&)>;

// 执行计划：冲突未确认时返回 mod.conflict；单个下载失败返回对应错误并清理临时文件。
util::Result<void> install_mod_plan(const ModInstallPlan& plan, const ModInstallOptions& options,
                                    DownloadScheduler& scheduler, ModHashCache& hash_cache,
                                    const std::filesystem::path& temp_dir,
                                    const ModInstallProgressFn& progress = nullptr,
                                    std::stop_token stop = {});

}  // namespace sscl::core
