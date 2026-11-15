#pragma once

// 整合包导入（D-089，P0：M4 验收缺口修复）。
//
// 支持两种主流格式：
//   ① Modrinth `.mrpack`：zip 内 `modrinth.index.json`，files[] 带 downloads 直链 + hashes；
//   ② CurseForge `manifest.json` zip：files[] 只有 projectID/fileID，需要 CF API 解析下载地址。
// 不支持的格式（MultiMC/Prism 等）返回 `modpack.unsupported`，绝不猜测。
//
// 设计边界：本模块只负责「解析 + 下载落盘」，不负责安装 MC/加载器；
// 目标实例匹配由 `match_modpack_instance()` 在已安装版本中完成，
// 找不到时由 UI 给出明确引导（先装对应版本），避免静默装错实例。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

#include "core/download/download_scheduler.hpp"
#include "core/game/instance_types.hpp"
#include "util/result.hpp"

namespace sscl::core {

enum class ModpackFormat {
    kUnknown,
    kModrinth,
    kCurseForge,
};

struct ModpackFile {
    std::string path;        // 相对实例目录（"mods/xxx.jar"），已做路径安全校验
    std::string url;         // mrpack 直链；CF 为空，需 resolve_curseforge_urls 填充
    std::string sha1;
    std::string sha512;
    std::int64_t size = -1;
    bool required = true;
    std::string project_id;  // CF projectID
    std::string file_id;     // CF fileID
};

struct ModpackIndex {
    ModpackFormat format = ModpackFormat::kUnknown;
    std::string name;
    std::string version;
    std::string minecraft_version;
    std::string loader;          // fabric / forge / neoforge / 空
    std::string loader_version;
    std::vector<ModpackFile> files;
    std::vector<std::string> warnings;
};

// 读取 zip 并解析（纯本地，不联网）。失败：modpack.read_failed / modpack.invalid / modpack.unsupported。
util::Result<ModpackIndex> parse_modpack(const std::filesystem::path& archive);

// 在已安装版本中找目标实例：先按 mc+loader 精确匹配，再退化为 mc 匹配（无 loader 要求时）。
// 返回空字符串表示未找到。
std::string match_modpack_instance(const ModpackIndex& index,
                                   const std::vector<VersionSummary>& installed);

// CurseForge：按 projectID/fileID 解析下载地址（UI 提供回调，避免 core 依赖 ModCatalog）。
using ModpackUrlResolver =
    std::function<util::Result<std::string>(const std::string& project_id,
                                            const std::string& file_id)>;
util::Result<void> resolve_curseforge_urls(ModpackIndex* index,
                                           const ModpackUrlResolver& resolver,
                                           const std::stop_token& stop);

struct ModpackInstallProgress {
    int completed_files = 0;
    int total_files = 0;
    std::string current_file;
    std::uint64_t done_bytes = 0;
    std::uint64_t total_bytes = 0;
};

using ModpackProgressFn = std::function<void(const ModpackInstallProgress&)>;

struct ModpackInstallOptions {
    bool overwrite = false;  // false：目标已存在则跳过
};

// 下载全部 required 文件到 instance_dir（相对路径已校验）；SHA1 校验由调度器完成。
// 返回成功写出的文件数。任一 required 文件失败即整体失败（modpack.download_failed）。
util::Result<int> install_modpack_files(const ModpackIndex& index,
                                        const std::filesystem::path& instance_dir,
                                        DownloadScheduler& scheduler,
                                        const ModpackInstallOptions& options,
                                        const ModpackProgressFn& progress,
                                        std::stop_token stop);

}  // namespace sscl::core
