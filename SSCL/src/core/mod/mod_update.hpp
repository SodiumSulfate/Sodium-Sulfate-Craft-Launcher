#pragma once

// PCL 本地 Mod 信息/更新检查（D-059）：
// - Modrinth：POST version_files → update_individual（精确 MC + 本地加载器）；
// - CurseForge：POST fingerprints/432 → mods（latestFilesIndexes）→ mods/files；
// - 结果只读，不修改 Mod 文件；UI 决定是否更新。
//
// 网络由 ModCatalog 提供；本模块负责批量组织、校验 hash、选择最新文件。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <stop_token>
#include <vector>

#include "core/download/download_scheduler.hpp"
#include "core/mod/mod_scanner.hpp"
#include "core/modplatform/mod_catalog.hpp"
#include "core/modplatform/mod_types.hpp"
#include "util/result.hpp"

namespace sscl::core {

struct ModUpdateInfo {
    LocalModEntry local;
    std::string sha1;
    std::uint32_t curseforge_fingerprint = 0;
    ModProject project;
    ModVersion current;  // 本地文件对应版本（可能为空）
    ModVersion latest;   // 最新兼容版本（可能为空）
    bool has_update = false;
};

struct ModUpdateReport {
    std::vector<ModUpdateInfo> items;
    std::string warning;
    int checked = 0;
    int with_update = 0;
};

struct ModUpdateOptions {
    std::string game_version;
    ModLoader loader = ModLoader::kUnknown;
    bool include_disabled = true;
    int max_requests = 500;  // 防异常列表
};

// 检查 mods_dir；失败返回 mod.* 错误；单个平台失败时保留另一平台结果并写 warning。
util::Result<ModUpdateReport> check_mod_updates(ModCatalog& catalog,
                                                const std::filesystem::path& mods_dir,
                                                const ModUpdateOptions& options,
                                                ModHashCache& hash_cache,
                                                const std::stop_token& stop = {});

// 纯函数：从 CF latestFilesIndexes 选精确 MC/加载器的最新文件 id；无匹配返回空。
std::string choose_curseforge_latest_file(const ModProject& project,
                                          const std::string& game_version,
                                          ModLoader local_loader);

// 纯函数：最新版本是否应替换本地文件（发布时间更晚且 hash 不同）。
bool should_update_mod(const ModVersion& current, const ModVersion& latest,
                       const std::string& local_sha1);

// PCL 更新文件名：去掉新旧文件名的共同前缀/后缀段，用剩余部分替换旧名核心。
std::string pcl_update_file_name(const std::string& current_name,
                                 const std::string& latest_name);

struct ModUpdateProgress {
    int index = 0;
    int total = 0;
    std::string file_name;
};

// 执行更新：下载到 temp_dir，删除旧文件（回收站），复制新文件到旧目录。
// PCL 不创建 .old；单个失败会保留旧文件并继续后续项。
util::Result<int> apply_mod_updates(const std::vector<ModUpdateInfo>& updates,
                                    DownloadScheduler& scheduler,
                                    const std::filesystem::path& temp_dir,
                                    const std::function<void(const ModUpdateProgress&)>& progress,
                                    const std::stop_token& stop = {});

}  // namespace sscl::core
