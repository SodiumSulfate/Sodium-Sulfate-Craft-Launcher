#pragma once

// Mod 平台统一模型（M4，D-055）：
// - Modrinth / CurseForge 都映射到这里的结构，UI 与安装器不感知平台差异；
// - 只放数据类型与小型映射函数，解析/HTTP 在各自客户端。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sscl::core {

enum class ModPlatform {
    kModrinth = 0,
    kCurseForge = 1,
};

enum class ModLoader {
    kVanilla = 0,  // 明确原版（不可装 Mod）
    kFabric = 1,
    kForge = 2,
    kNeoForge = 3,
    kQuilt = 4,
    kUnknown = 5,  // 未指定（搜索时表示不限）
};

enum class ModSort {
    kRelevance = 0,
    kDownloads = 1,
    kUpdated = 2,
    kNewest = 3,
};

enum class ModChannel {
    kRelease = 0,
    kBeta = 1,
    kAlpha = 2,
    kUnknown = 3,
};

enum class DependencyKind {
    kRequired = 0,
    kOptional = 1,
    kEmbedded = 2,
    kIncompatible = 3,
    kTool = 4,
    kInclude = 5,
    kBroken = 6,
};

// 依赖安装策略（D-058）：询问（默认）/ 自动 / 从不。
enum class DependencyMode {
    kAsk = 0,
    kAuto = 1,
    kNever = 2,
};

const char* mod_platform_key(ModPlatform platform);
const char* mod_loader_key(ModLoader loader);
ModLoader mod_loader_from_key(std::string_view key);
ModLoader mod_loader_from_curseforge_type(int type);
int mod_loader_to_curseforge_type(ModLoader loader);
bool mod_loader_is_moddable(ModLoader loader);
const char* mod_channel_key(ModChannel channel);
ModChannel mod_channel_from_key(std::string_view key);

struct ModProject {
    ModPlatform platform = ModPlatform::kModrinth;
    std::string id;       // Modrinth project_id / CurseForge modId
    std::string slug;
    std::string title;
    std::string summary;
    std::string author;
    std::string icon_url;
    std::string page_url;
    std::uint64_t downloads = 0;
    std::vector<std::string> categories;
    std::string updated_at;  // ISO8601
    bool distribution_allowed = true;  // CF allowModDistribution；Modrinth 恒 true

    // CF latestFilesIndexes：本地更新时按精确 MC/加载器选文件（PCL LocalResourceLoaders）。
    struct LatestFile {
        std::string game_version;
        ModLoader loader = ModLoader::kUnknown;
        std::string file_id;
    };
    std::vector<LatestFile> latest_files;
};

struct ModFile {
    std::string filename;
    std::string url;
    std::string sha1;
    std::string sha512;
    std::uint64_t size = 0;
    bool primary = false;
    std::string file_type;  // Modrinth 附件类型；CF 空
};

struct ModDependency {
    DependencyKind kind = DependencyKind::kRequired;
    std::string project_id;
    std::string version_id;
    std::string file_name;
};

struct ModVersion {
    ModPlatform platform = ModPlatform::kModrinth;
    std::string project_id;
    std::string version_id;
    std::string name;
    std::string version_number;
    ModChannel channel = ModChannel::kUnknown;
    std::string published_at;
    std::vector<std::string> game_versions;
    std::vector<ModLoader> loaders;
    std::vector<ModFile> files;
    std::vector<ModDependency> dependencies;
    bool featured = false;
    bool downloadable = true;  // CF：禁止分发时 false
};

struct ModSearchQuery {
    ModPlatform platform = ModPlatform::kModrinth;
    std::string text;
    std::string game_version;  // 空 = 不限
    ModLoader loader = ModLoader::kUnknown;
    std::string category;       // Modrinth slug（PCL Tag.AfterLast("/")）
    std::string category_cf;    // CurseForge categoryId（PCL Tag.BeforeFirst("/")）
    ModSort sort = ModSort::kRelevance;
    int offset = 0;
    int limit = 20;
};

struct ModSearchPage {
    std::vector<ModProject> items;
    int offset = 0;
    int limit = 0;
    int total = -1;  // -1 = 未知
    bool has_more = false;
    std::string warning;  // 例如“使用离线缓存”
};

// 本地 Mod 更新查询（PCL LocalResourceLoaders：精确 MC 版本 + 本地加载器集合）。
struct ModUpdateQuery {
    std::string hash;  // Modrinth sha1 / CurseForge 指纹十进制
    std::string game_version;
    std::vector<ModLoader> loaders;
};

// 选中可安装的主文件（Modrinth primary 且非 sources/dev/javadoc/signature；
// 否则第一个非附件；再否则第一个）。
const ModFile* select_primary_file(const ModVersion& version);

}  // namespace sscl::core
