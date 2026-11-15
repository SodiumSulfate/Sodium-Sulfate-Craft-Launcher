#include "core/modplatform/curseforge_client.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

#include <nlohmann/json.hpp>

#include "core/modplatform/mod_json.hpp"

namespace sscl::core {

namespace {

using nlohmann::json;

std::string url_encode(const std::string& text) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const unsigned char c : text) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

int cf_sort_field(ModSort sort) {
    switch (sort) {
        case ModSort::kDownloads:
            return 6;
        case ModSort::kUpdated:
        case ModSort::kNewest:
            return 3;
        case ModSort::kRelevance:
        default:
            return 2;  // Popularity：CF 没有真正的 relevance
    }
}

int cf_loader_type(ModLoader loader) {
    switch (loader) {
        case ModLoader::kForge:
            return 1;
        case ModLoader::kFabric:
            return 4;
        case ModLoader::kQuilt:
            return 5;
        case ModLoader::kNeoForge:
            return 6;
        default:
            return 0;
    }
}

DependencyKind dependency_kind_from_relation(int relation_type) {
    switch (relation_type) {
        case 1:
            return DependencyKind::kEmbedded;
        case 2:
            return DependencyKind::kOptional;
        case 3:
            return DependencyKind::kRequired;
        case 4:
            return DependencyKind::kTool;
        case 5:
            return DependencyKind::kIncompatible;
        case 6:
            return DependencyKind::kInclude;
        default:
            return DependencyKind::kOptional;
    }
}

ModChannel channel_from_release_type(std::int64_t release_type) {
    switch (release_type) {
        case 1:
            return ModChannel::kRelease;
        case 2:
            return ModChannel::kBeta;
        case 3:
            return ModChannel::kAlpha;
        default:
            return ModChannel::kUnknown;
    }
}

ModProject parse_project_node(const json& node) {
    ModProject project;
    project.platform = ModPlatform::kCurseForge;
    project.id = std::to_string(modjson::integer(node, "id", 0));
    project.slug = modjson::str(node, "slug");
    project.title = modjson::str(node, "name");
    project.summary = modjson::str(node, "summary");
    project.downloads = static_cast<std::uint64_t>(modjson::integer(node, "downloadCount", 0));
    project.updated_at = modjson::str(node, "dateReleased");
    if (project.updated_at.empty()) {
        project.updated_at = modjson::str(node, "dateModified");
    }
    project.distribution_allowed = modjson::boolean(node, "allowModDistribution", true);

    if (modjson::has(node, "logo") && node["logo"].is_object()) {
        project.icon_url = modjson::str(node["logo"], "thumbnailUrl");
        if (project.icon_url.empty()) {
            project.icon_url = modjson::str(node["logo"], "url");
        }
    }
    if (modjson::has(node, "links") && node["links"].is_object()) {
        project.page_url = modjson::str(node["links"], "websiteUrl");
    }
    if (project.page_url.empty() && !project.slug.empty()) {
        project.page_url = "https://www.curseforge.com/minecraft/mc-mods/" + project.slug;
    }
    if (modjson::has(node, "categories") && node["categories"].is_array()) {
        for (const json& category : node["categories"]) {
            if (category.is_string()) {
                project.categories.push_back(category.get<std::string>());
            } else if (category.is_object()) {
                std::string name = modjson::str(category, "name");
                if (name.empty()) {
                    name = modjson::str(category, "slug");
                }
                if (!name.empty()) {
                    project.categories.push_back(std::move(name));
                }
            }
        }
    }
    if (modjson::has(node, "authors") && node["authors"].is_array()) {
        for (const json& author : node["authors"]) {
            if (author.is_object()) {
                project.author = modjson::str(author, "name");
                if (!project.author.empty()) {
                    break;
                }
            }
        }
    }
    if (modjson::has(node, "latestFilesIndexes") && node["latestFilesIndexes"].is_array()) {
        for (const json& entry : node["latestFilesIndexes"]) {
            if (!entry.is_object()) {
                continue;
            }
            ModProject::LatestFile latest;
            latest.game_version = modjson::str(entry, "gameVersion");
            latest.loader = mod_loader_from_curseforge_type(
                static_cast<int>(modjson::integer(entry, "modLoader", 0)));
            const std::int64_t file_id = modjson::integer(entry, "fileId", 0);
            if (file_id > 0) {
                latest.file_id = std::to_string(file_id);
            }
            if (!latest.game_version.empty() && !latest.file_id.empty()) {
                project.latest_files.push_back(std::move(latest));
            }
        }
    }
    return project;
}

ModVersion parse_file_node(const json& node) {
    ModVersion version;
    version.platform = ModPlatform::kCurseForge;
    version.project_id = std::to_string(modjson::integer(node, "modId", 0));
    version.version_id = std::to_string(modjson::integer(node, "id", 0));
    version.name = modjson::str(node, "displayName");
    version.version_number = version.name;
    if (version.name.empty()) {
        version.name = modjson::str(node, "fileName");
        version.version_number = version.name;
    }
    version.channel = channel_from_release_type(modjson::integer(node, "releaseType", 0));
    version.published_at = modjson::str(node, "fileDate");

    ModFile file;
    file.filename = modjson::str(node, "fileName");
    file.url = modjson::str(node, "downloadUrl");
    file.size = static_cast<std::uint64_t>(modjson::integer(node, "fileLength", 0));
    file.primary = true;
    version.files.push_back(file);

    const bool available = modjson::boolean(node, "isAvailable", true);
    version.downloadable = available && !file.url.empty();

    if (modjson::has(node, "hashes") && node["hashes"].is_array()) {
        for (const json& hash : node["hashes"]) {
            if (!hash.is_object()) {
                continue;
            }
            const std::int64_t algo = modjson::integer(hash, "algo", 0);
            const std::string value = modjson::str(hash, "value");
            if (algo == 1 && !value.empty()) {
                version.files.front().sha1 = value;
            }
        }
    }

    if (modjson::has(node, "gameVersions") && node["gameVersions"].is_array()) {
        for (const json& item : node["gameVersions"]) {
            if (!item.is_string()) {
                continue;
            }
            const std::string token = item.get<std::string>();
            if (is_mc_version_token(token)) {
                version.game_versions.push_back(token);
            } else {
                const ModLoader loader = mod_loader_from_key(token);
                if (loader != ModLoader::kUnknown && loader != ModLoader::kVanilla) {
                    if (std::find(version.loaders.begin(), version.loaders.end(), loader) ==
                        version.loaders.end()) {
                        version.loaders.push_back(loader);
                    }
                }
            }
        }
    }

    if (modjson::has(node, "dependencies") && node["dependencies"].is_array()) {
        for (const json& dependency : node["dependencies"]) {
            if (!dependency.is_object()) {
                continue;
            }
            ModDependency parsed;
            parsed.kind = dependency_kind_from_relation(
                static_cast<int>(modjson::integer(dependency, "relationType", 0)));
            const std::int64_t mod_id = modjson::integer(dependency, "modId", 0);
            if (mod_id > 0) {
                parsed.project_id = std::to_string(mod_id);
            }
            if (!parsed.project_id.empty()) {
                version.dependencies.push_back(std::move(parsed));
            }
        }
    }
    return version;
}

}  // namespace

bool is_mc_version_token(const std::string& token) {
    if (token.empty()) {
        return false;
    }
    std::string lower;
    lower.reserve(token.size());
    for (const unsigned char c : token) {
        lower.push_back(static_cast<char>(std::tolower(c)));
    }
    static const char* kNonVersions[] = {"client", "server", "forge", "fabric", "neoforge",
                                         "quilt", "liteloader", "cauldron", "plugin",
                                         "datapack"};
    for (const char* name : kNonVersions) {
        if (lower == name) {
            return false;
        }
    }
    // 快照：23w45a / 24w14a
    if (lower.size() >= 5 && std::isdigit(static_cast<unsigned char>(lower[0])) &&
        std::isdigit(static_cast<unsigned char>(lower[1])) && lower[2] == 'w' &&
        std::isdigit(static_cast<unsigned char>(lower[3])) &&
        std::isdigit(static_cast<unsigned char>(lower[4]))) {
        return lower.size() == 5 ||
               (lower.size() == 6 && std::isalpha(static_cast<unsigned char>(lower[5])));
    }
    // 正式版：1.21 / 1.21.1
    return std::isdigit(static_cast<unsigned char>(lower[0])) &&
           lower.find('.') != std::string::npos;
}

std::string build_curseforge_search_url(const ModSearchQuery& query) {
    const int page_size = std::clamp(query.limit, 1, 50);
    const int index = std::clamp(query.offset, 0, 10000);
    std::string url = "https://api.curseforge.com/v1/mods/search?gameId=432&classId=6";
    url += "&index=" + std::to_string(index);
    url += "&pageSize=" + std::to_string(page_size);
    url += "&sortField=" + std::to_string(cf_sort_field(query.sort)) + "&sortOrder=desc";
    if (!query.text.empty()) {
        url += "&searchFilter=" + url_encode(query.text);
    }
    if (!query.game_version.empty()) {
        url += "&gameVersion=" + url_encode(query.game_version);
    }
    const int loader_type = cf_loader_type(query.loader);
    if (loader_type != 0) {
        url += "&modLoaderType=" + std::to_string(loader_type);
    }
    // PCL Tag = "CF分类ID/MR分类slug"；CF 只接受数字 categoryId。
    const std::string& cf_category =
        query.category_cf.empty() ? query.category : query.category_cf;
    if (!cf_category.empty()) {
        try {
            const int category_id = std::stoi(cf_category);
            if (category_id > 0) {
                url += "&categoryId=" + std::to_string(category_id);
            }
        } catch (...) {
            // 非数字分类（CF 只接受 id）忽略
        }
    }
    return url;
}

std::string build_curseforge_files_url(const std::string& project_id,
                                       const std::string& game_version, ModLoader loader,
                                       int index, int page_size) {
    std::string url = "https://api.curseforge.com/v1/mods/" + url_encode(project_id) + "/files";
    url += "?index=" + std::to_string(std::clamp(index, 0, 10000));
    url += "&pageSize=" + std::to_string(std::clamp(page_size, 1, 50));
    if (!game_version.empty()) {
        url += "&gameVersion=" + url_encode(game_version);
    }
    const int loader_type = cf_loader_type(loader);
    if (loader_type != 0) {
        url += "&modLoaderType=" + std::to_string(loader_type);
    }
    return url;
}

std::string build_curseforge_ids_body(const char* key, const std::vector<std::string>& ids) {
    std::string body = "{\"";
    body += key;
    body += "\":[";
    bool first = true;
    for (const std::string& id : ids) {
        try {
            const long long value = std::stoll(id);
            if (!first) {
                body += ",";
            }
            body += std::to_string(value);
            first = false;
        } catch (...) {
            // 非数字 id 不写入（CF 文件/模组 id 均为数字）。
        }
    }
    body += "]}";
    return body;
}

std::string build_curseforge_fingerprints_body(
    const std::vector<std::uint32_t>& fingerprints) {
    std::string body = "{\"fingerprints\":[";
    for (std::size_t i = 0; i < fingerprints.size(); ++i) {
        if (i != 0) {
            body += ",";
        }
        body += std::to_string(fingerprints[i]);
    }
    body += "]}";
    return body;
}

util::Result<std::vector<ModProject>> parse_curseforge_projects(const std::string& text) {
    util::Result<json> parsed = modjson::parse_object(text, "CurseForge 项目列表");
    if (parsed.is_error()) {
        return util::Result<std::vector<ModProject>>::fail(std::move(parsed.error()));
    }
    const json& root = parsed.value();
    if (!root.contains("data") || !root["data"].is_array()) {
        return util::Result<std::vector<ModProject>>::fail(util::Error::make(
            "mod.api_invalid", "平台返回了无法识别的数据",
            "CurseForge 项目列表：缺少 data"));
    }
    std::vector<ModProject> projects;
    for (const json& node : root["data"]) {
        if (node.is_object()) {
            projects.push_back(parse_project_node(node));
        }
    }
    return util::Result<std::vector<ModProject>>::ok(std::move(projects));
}

util::Result<std::vector<CurseForgeFingerprintMatch>> parse_curseforge_fingerprints(
    const std::string& text) {
    util::Result<json> parsed = modjson::parse_object(text, "CurseForge 指纹");
    if (parsed.is_error()) {
        return util::Result<std::vector<CurseForgeFingerprintMatch>>::fail(
            std::move(parsed.error()));
    }
    const json& root = parsed.value();
    if (!root.contains("data") || !root["data"].is_object() ||
        !root["data"].contains("exactMatches") ||
        !root["data"]["exactMatches"].is_array()) {
        return util::Result<std::vector<CurseForgeFingerprintMatch>>::fail(util::Error::make(
            "mod.api_invalid", "平台返回了无法识别的数据",
            "CurseForge 指纹：缺少 data.exactMatches"));
    }
    std::vector<CurseForgeFingerprintMatch> matches;
    for (const json& entry : root["data"]["exactMatches"]) {
        if (!entry.is_object() || !entry.contains("file") || !entry["file"].is_object()) {
            continue;
        }
        CurseForgeFingerprintMatch match;
        match.project_id = modjson::str(entry, "id");
        match.file = parse_file_node(entry["file"]);
        if (match.project_id.empty()) {
            match.project_id = match.file.project_id;
        }
        const std::int64_t fingerprint =
            modjson::integer(entry["file"], "fileFingerprint", 0);
        if (fingerprint >= 0) {
            match.fingerprint = std::to_string(fingerprint);
        }
        if (!match.project_id.empty()) {
            matches.push_back(std::move(match));
        }
    }
    return util::Result<std::vector<CurseForgeFingerprintMatch>>::ok(std::move(matches));
}

util::Result<ModSearchPage> parse_curseforge_search(const std::string& text) {
    util::Result<json> parsed = modjson::parse_object(text, "CurseForge 搜索");
    if (parsed.is_error()) {
        return util::Result<ModSearchPage>::fail(std::move(parsed.error()));
    }
    const json& root = parsed.value();
    if (!root.contains("data") || !root["data"].is_array()) {
        return util::Result<ModSearchPage>::fail(util::Error::make(
            "mod.api_invalid", "平台返回了无法识别的数据", "CurseForge 搜索：缺少 data"));
    }
    ModSearchPage page;
    for (const json& node : root["data"]) {
        if (node.is_object()) {
            page.items.push_back(parse_project_node(node));
        }
    }
    if (root.contains("pagination") && root["pagination"].is_object()) {
        const json& pagination = root["pagination"];
        page.offset = static_cast<int>(modjson::integer(pagination, "index", 0));
        page.limit = static_cast<int>(modjson::integer(pagination, "pageSize", 0));
        page.total = static_cast<int>(modjson::integer(pagination, "totalCount", -1));
    }
    page.has_more = page.total < 0
                        ? static_cast<int>(page.items.size()) >= page.limit
                        : page.offset + static_cast<int>(page.items.size()) < page.total;
    return util::Result<ModSearchPage>::ok(std::move(page));
}

util::Result<ModProject> parse_curseforge_project(const std::string& text) {
    util::Result<json> parsed = modjson::parse_object(text, "CurseForge 项目");
    if (parsed.is_error()) {
        return util::Result<ModProject>::fail(std::move(parsed.error()));
    }
    const json& root = parsed.value();
    if (!root.contains("data") || !root["data"].is_object()) {
        return util::Result<ModProject>::fail(util::Error::make(
            "mod.api_invalid", "平台返回了无法识别的数据", "CurseForge 项目：缺少 data"));
    }
    return util::Result<ModProject>::ok(parse_project_node(root["data"]));
}

util::Result<std::vector<ModVersion>> parse_curseforge_files(const std::string& text) {
    util::Result<json> parsed = modjson::parse_object(text, "CurseForge 文件");
    if (parsed.is_error()) {
        return util::Result<std::vector<ModVersion>>::fail(std::move(parsed.error()));
    }
    const json& root = parsed.value();
    if (!root.contains("data") || !root["data"].is_array()) {
        return util::Result<std::vector<ModVersion>>::fail(util::Error::make(
            "mod.api_invalid", "平台返回了无法识别的数据", "CurseForge 文件：缺少 data"));
    }
    std::vector<ModVersion> versions;
    versions.reserve(root["data"].size());
    for (const json& node : root["data"]) {
        if (node.is_object()) {
            versions.push_back(parse_file_node(node));
        }
    }
    return util::Result<std::vector<ModVersion>>::ok(std::move(versions));
}

util::Result<ModVersion> parse_curseforge_file(const std::string& text) {
    util::Result<json> parsed = modjson::parse_object(text, "CurseForge 单文件");
    if (parsed.is_error()) {
        return util::Result<ModVersion>::fail(std::move(parsed.error()));
    }
    const json& root = parsed.value();
    if (!root.contains("data") || !root["data"].is_object()) {
        return util::Result<ModVersion>::fail(util::Error::make(
            "mod.api_invalid", "平台返回了无法识别的数据", "CurseForge 单文件：缺少 data"));
    }
    return util::Result<ModVersion>::ok(parse_file_node(root["data"]));
}

util::Result<ModSearchPage> CurseForgeClient::search(const ModSearchQuery& query,
                                                     const std::stop_token& stop) {
    const std::string url = build_curseforge_search_url(query);
    bool stale = false;
    util::Result<std::string> body =
        m_http.get_json(url, std::chrono::seconds(600), stop, &stale);
    if (body.is_error()) {
        return util::Result<ModSearchPage>::fail(std::move(body.error()));
    }
    util::Result<ModSearchPage> page = parse_curseforge_search(body.value());
    if (page.is_ok() && stale) {
        page.value().warning = "网络不可用，显示的是离线缓存";
    }
    return page;
}

util::Result<ModProject> CurseForgeClient::project(const std::string& id,
                                                   const std::stop_token& stop) {
    const std::string url = "https://api.curseforge.com/v1/mods/" + url_encode(id);
    bool stale = false;
    util::Result<std::string> body =
        m_http.get_json(url, std::chrono::seconds(86400), stop, &stale);
    if (body.is_error()) {
        return util::Result<ModProject>::fail(std::move(body.error()));
    }
    return parse_curseforge_project(body.value());
}

util::Result<std::vector<ModVersion>> CurseForgeClient::versions(
    const std::string& project_id, const std::string& game_version, ModLoader loader,
    bool all_versions, const std::stop_token& stop) {
    // 先取项目以应用 allowModDistribution（缓存 24h，通常只有一次请求）。
    util::Result<ModProject> project_info = project(project_id, stop);
    const bool distribution_allowed =
        project_info.is_ok() ? project_info.value().distribution_allowed : true;

    std::vector<ModVersion> result;
    int index = 0;
    constexpr int kPageSize = 50;
    constexpr int kMaxFiles = 1000;
    while (index < kMaxFiles) {
        const std::string url = build_curseforge_files_url(
            project_id, all_versions ? std::string() : game_version,
            all_versions ? ModLoader::kUnknown : loader, index, kPageSize);
        bool stale = false;
        util::Result<std::string> body =
            m_http.get_json(url, std::chrono::seconds(1800), stop, &stale);
        if (body.is_error()) {
            if (!result.empty()) {
                break;  // 已有部分结果时容忍翻页失败
            }
            return util::Result<std::vector<ModVersion>>::fail(std::move(body.error()));
        }
        util::Result<std::vector<ModVersion>> page = parse_curseforge_files(body.value());
        if (page.is_error()) {
            return page;
        }
        const std::size_t before = result.size();
        for (ModVersion& version : page.value()) {
            version.downloadable = version.downloadable && distribution_allowed;
            result.push_back(std::move(version));
        }
        if (result.size() == before || page.value().size() < kPageSize) {
            break;
        }
        index += static_cast<int>(page.value().size());
    }
    return util::Result<std::vector<ModVersion>>::ok(std::move(result));
}

util::Result<ModVersion> CurseForgeClient::version(const std::string& project_id,
                                                   const std::string& file_id,
                                                   const std::stop_token& stop) {
    const std::string url = "https://api.curseforge.com/v1/mods/" +
                            url_encode(project_id) + "/files/" + url_encode(file_id);
    bool stale = false;
    util::Result<std::string> body =
        m_http.get_json(url, std::chrono::seconds(86400), stop, &stale);
    if (body.is_error()) {
        return util::Result<ModVersion>::fail(std::move(body.error()));
    }
    return parse_curseforge_file(body.value());
}

util::Result<std::vector<CurseForgeFingerprintMatch>> CurseForgeClient::fingerprints(
    const std::vector<std::uint32_t>& fingerprints, const std::stop_token& stop) {
    if (fingerprints.empty()) {
        return util::Result<std::vector<CurseForgeFingerprintMatch>>::ok({});
    }
    util::Result<std::string> body = m_http.post_json(
        "https://api.curseforge.com/v1/fingerprints/432",
        build_curseforge_fingerprints_body(fingerprints), stop);
    if (body.is_error()) {
        return util::Result<std::vector<CurseForgeFingerprintMatch>>::fail(
            std::move(body.error()));
    }
    return parse_curseforge_fingerprints(body.value());
}

util::Result<std::vector<ModProject>> CurseForgeClient::projects(
    const std::vector<std::string>& ids, const std::stop_token& stop) {
    if (ids.empty()) {
        return util::Result<std::vector<ModProject>>::ok({});
    }
    util::Result<std::string> body = m_http.post_json(
        "https://api.curseforge.com/v1/mods", build_curseforge_ids_body("modIds", ids),
        stop);
    if (body.is_error()) {
        return util::Result<std::vector<ModProject>>::fail(std::move(body.error()));
    }
    return parse_curseforge_projects(body.value());
}

util::Result<std::vector<ModVersion>> CurseForgeClient::files(
    const std::vector<std::string>& ids, const std::stop_token& stop) {
    if (ids.empty()) {
        return util::Result<std::vector<ModVersion>>::ok({});
    }
    util::Result<std::string> body = m_http.post_json(
        "https://api.curseforge.com/v1/mods/files", build_curseforge_ids_body("fileIds", ids),
        stop);
    if (body.is_error()) {
        return util::Result<std::vector<ModVersion>>::fail(std::move(body.error()));
    }
    return parse_curseforge_files(body.value());
}

}  // namespace sscl::core
