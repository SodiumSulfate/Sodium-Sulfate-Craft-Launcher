#pragma once

// CurseForge API v1 客户端（D-055）。
// - API 必须 `x-api-key`；无 Key 时由 ModHttpClient 直接走 MCIM；
// - 搜索/项目/文件分页（pageSize≤50、offset≤10000）；
// - `allowModDistribution=false` 的文件不提供直链（downloadable=false）；
// - `gameVersions` 是“MC 版本 + 加载器 + Client/Server”混合 token，需过滤。

#include <cstdint>
#include <string>
#include <stop_token>
#include <vector>

#include "core/modplatform/mod_api.hpp"
#include "core/modplatform/mod_types.hpp"
#include "util/result.hpp"

namespace sscl::core {

// CF 指纹匹配项：exactMatches[] 中只含项目 id + file，项目详情需再批量取 /v1/mods。
struct CurseForgeFingerprintMatch {
    std::string project_id;
    std::string fingerprint;
    ModVersion file;
};

class CurseForgeClient {
public:
    explicit CurseForgeClient(ModHttpClient& http) : m_http(http) {}

    util::Result<ModSearchPage> search(const ModSearchQuery& query,
                                       const std::stop_token& stop = {});
    util::Result<ModProject> project(const std::string& id,
                                     const std::stop_token& stop = {});
    util::Result<std::vector<ModVersion>> versions(const std::string& project_id,
                                                   const std::string& game_version,
                                                   ModLoader loader, bool all_versions,
                                                   const std::stop_token& stop = {});
    util::Result<ModVersion> version(const std::string& project_id,
                                     const std::string& file_id,
                                     const std::stop_token& stop = {});

    // PCL 本地更新：POST /v1/fingerprints/432 → exactMatches。
    util::Result<std::vector<CurseForgeFingerprintMatch>> fingerprints(
        const std::vector<std::uint32_t>& fingerprints, const std::stop_token& stop = {});
    // POST /v1/mods {modIds} → 项目（含 latestFilesIndexes）。
    util::Result<std::vector<ModProject>> projects(const std::vector<std::string>& ids,
                                                   const std::stop_token& stop = {});
    // POST /v1/mods/files {fileIds} → 文件。
    util::Result<std::vector<ModVersion>> files(const std::vector<std::string>& ids,
                                                const std::stop_token& stop = {});

private:
    ModHttpClient& m_http;
};

std::string build_curseforge_search_url(const ModSearchQuery& query);
std::string build_curseforge_files_url(const std::string& project_id,
                                       const std::string& game_version, ModLoader loader,
                                       int index, int page_size);
std::string build_curseforge_fingerprints_body(
    const std::vector<std::uint32_t>& fingerprints);
std::string build_curseforge_ids_body(const char* key, const std::vector<std::string>& ids);

// CurseForge gameVersions 里的真实 MC 版本（过滤 Client/Server/加载器名）。
bool is_mc_version_token(const std::string& token);

util::Result<ModSearchPage> parse_curseforge_search(const std::string& json);
util::Result<ModProject> parse_curseforge_project(const std::string& json);
util::Result<std::vector<ModVersion>> parse_curseforge_files(const std::string& json);
util::Result<ModVersion> parse_curseforge_file(const std::string& json);
util::Result<std::vector<ModProject>> parse_curseforge_projects(const std::string& json);
util::Result<std::vector<CurseForgeFingerprintMatch>> parse_curseforge_fingerprints(
    const std::string& json);

}  // namespace sscl::core
