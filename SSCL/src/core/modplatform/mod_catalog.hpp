#pragma once

// Mod 平台门面（D-055）：UI 只依赖本类，不直接感知 Modrinth/CurseForge。
// 持有两个平台的 ModHttpClient 与客户端；缓存目录由 <data>/cache/modplatform 提供。

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <stop_token>
#include <vector>

#include "core/modplatform/curseforge_client.hpp"
#include "core/modplatform/mod_api.hpp"
#include "core/modplatform/mod_types.hpp"
#include "core/modplatform/modrinth_client.hpp"
#include "util/result.hpp"

namespace sscl::core {

inline constexpr const char* kModUserAgent = "SSCL/0.1.0";

struct ModCatalogOptions {
    ModHttpOptions modrinth;
    ModHttpOptions curseforge;
};

// 默认：UA + 缓存目录 + CF Key（用户/构建/环境由调用方汇总）+ MCIM 开关。
ModCatalogOptions make_mod_catalog_options(const std::filesystem::path& data_dir,
                                           const std::string& curseforge_key,
                                           bool use_mcim);

class ModCatalog {
public:
    explicit ModCatalog(ModCatalogOptions options);

    bool available(ModPlatform platform, std::string* reason = nullptr) const;

    util::Result<ModSearchPage> search(const ModSearchQuery& query,
                                       const std::stop_token& stop = {});

    // PCL 2.99 ResourceSearcher 语义：双平台并行搜索 → CF 在前 → 去重 → 评分排序。
    // 无搜索词按下载量×平台权重（CF×1 / Modrinth×5）；有搜索词用 log10 下载量 + 文本相似度。
    // 任一平台失败仍返回另一平台结果（warning 提示"结果可能不全"）；两者都失败才报错。
    util::Result<ModSearchPage> search_merged(const ModSearchQuery& query,
                                               const std::stop_token& stop = {});
    util::Result<ModProject> project(ModPlatform platform, const std::string& id,
                                     const std::stop_token& stop = {});
    util::Result<std::vector<ModVersion>> versions(ModPlatform platform,
                                                   const std::string& project_id,
                                                   const std::string& game_version,
                                                   ModLoader loader, bool all_versions,
                                                   const std::stop_token& stop = {});
    util::Result<ModVersion> version(ModPlatform platform, const std::string& project_id,
                                     const std::string& version_id,
                                     const std::stop_token& stop = {});

    // 本地 Mod 更新（本期先提供 Modrinth；CF 指纹批处理后续接入）。
    util::Result<std::map<std::string, ModVersion>> version_files(
        ModPlatform platform, const std::vector<std::string>& hashes,
        const std::stop_token& stop = {});
    util::Result<std::map<std::string, ModVersion>> update_individual(
        ModPlatform platform, const std::vector<ModUpdateQuery>& queries,
        const std::stop_token& stop = {});

    // CurseForge 本地更新（fingerprints → projects/latestFilesIndexes → files）。
    util::Result<std::vector<CurseForgeFingerprintMatch>> curseforge_fingerprints(
        const std::vector<std::uint32_t>& fingerprints, const std::stop_token& stop = {});
    util::Result<std::vector<ModProject>> curseforge_projects(
        const std::vector<std::string>& ids, const std::stop_token& stop = {});
    util::Result<std::vector<ModVersion>> curseforge_files(
        const std::vector<std::string>& ids, const std::stop_token& stop = {});

private:
    ModHttpClient m_modrinth_http;
    ModHttpClient m_curseforge_http;
    ModrinthClient m_modrinth;
    CurseForgeClient m_curseforge;
};

}  // namespace sscl::core
