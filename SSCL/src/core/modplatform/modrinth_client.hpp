#pragma once

// Modrinth API v2 客户端（D-055）。
// 端点：/v2/search、/v2/project/{id}、/v2/project/{id}/version、/v2/version/{id}。
// 解析函数与 URL 构造函数公开，便于无网络单测。

#include <map>
#include <string>
#include <stop_token>
#include <vector>

#include "core/modplatform/mod_api.hpp"
#include "core/modplatform/mod_types.hpp"
#include "util/result.hpp"

namespace sscl::core {

class ModrinthClient {
public:
    explicit ModrinthClient(ModHttpClient& http) : m_http(http) {}

    util::Result<ModSearchPage> search(const ModSearchQuery& query,
                                       const std::stop_token& stop = {});
    util::Result<ModProject> project(const std::string& id,
                                     const std::stop_token& stop = {});
    util::Result<std::vector<ModVersion>> versions(const std::string& project_id,
                                                   const std::string& game_version,
                                                   ModLoader loader, bool all_versions,
                                                   const std::stop_token& stop = {});
    util::Result<ModVersion> version(const std::string& project_id,
                                     const std::string& version_id,
                                     const std::stop_token& stop = {});

    // POST /v2/version_files：本地 sha1 → 版本对象。
    util::Result<std::map<std::string, ModVersion>> version_files(
        const std::vector<std::string>& hashes, const std::stop_token& stop = {});

    // POST /v2/version_files/update_individual：精确 MC/加载器下的最新版本。
    util::Result<std::map<std::string, ModVersion>> update_individual(
        const std::vector<ModUpdateQuery>& queries, const std::stop_token& stop = {});

private:
    ModHttpClient& m_http;
};

std::string build_modrinth_search_url(const ModSearchQuery& query);
std::string build_modrinth_versions_url(const std::string& project_id,
                                        const std::string& game_version, ModLoader loader,
                                        bool all_versions);
std::string build_modrinth_version_files_body(const std::vector<std::string>& hashes);
std::string build_modrinth_update_body(const std::vector<ModUpdateQuery>& queries);
util::Result<std::map<std::string, ModVersion>> parse_modrinth_version_map(
    const std::string& json);

util::Result<ModSearchPage> parse_modrinth_search(const std::string& json);
util::Result<ModProject> parse_modrinth_project(const std::string& json);
util::Result<std::vector<ModVersion>> parse_modrinth_versions(const std::string& json);
util::Result<ModVersion> parse_modrinth_version(const std::string& json);

}  // namespace sscl::core
