#include "core/modplatform/modrinth_client.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>

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

const char* sort_key(ModSort sort) {
    switch (sort) {
        case ModSort::kDownloads:
            return "downloads";
        case ModSort::kUpdated:
            return "updated";
        case ModSort::kNewest:
            return "newest";
        case ModSort::kRelevance:
        default:
            return "relevance";
    }
}

DependencyKind dependency_kind_from_key(const std::string& key) {
    if (key == "required") {
        return DependencyKind::kRequired;
    }
    if (key == "optional") {
        return DependencyKind::kOptional;
    }
    if (key == "embedded") {
        return DependencyKind::kEmbedded;
    }
    if (key == "incompatible") {
        return DependencyKind::kIncompatible;
    }
    return DependencyKind::kOptional;
}

ModProject parse_project_node(const json& node) {
    ModProject project;
    project.platform = ModPlatform::kModrinth;
    project.id = modjson::str(node, "project_id");
    if (project.id.empty()) {
        project.id = modjson::str(node, "id");
    }
    project.slug = modjson::str(node, "slug");
    project.title = modjson::str(node, "title");
    project.summary = modjson::str(node, "description");
    project.author = modjson::str(node, "author");
    project.icon_url = modjson::str(node, "icon_url");
    project.downloads = static_cast<std::uint64_t>(
        modjson::integer(node, "downloads", 0));
    project.categories = modjson::str_array(node, "categories");
    project.updated_at = modjson::str(node, "date_modified");
    if (project.updated_at.empty()) {
        project.updated_at = modjson::str(node, "updated");
    }
    if (!project.slug.empty()) {
        project.page_url = "https://modrinth.com/mod/" + project.slug;
    } else if (!project.id.empty()) {
        project.page_url = "https://modrinth.com/mod/" + project.id;
    }
    project.distribution_allowed = true;
    return project;
}

ModFile parse_file_node(const json& node) {
    ModFile file;
    file.filename = modjson::str(node, "filename");
    file.url = modjson::str(node, "url");
    file.size = static_cast<std::uint64_t>(modjson::integer(node, "size", 0));
    file.primary = modjson::boolean(node, "primary", false);
    file.file_type = modjson::str(node, "file_type");
    if (modjson::has(node, "hashes") && node["hashes"].is_object()) {
        const json& hashes = node["hashes"];
        file.sha1 = modjson::str(hashes, "sha1");
        file.sha512 = modjson::str(hashes, "sha512");
    }
    return file;
}

ModVersion parse_version_node(const json& node) {
    ModVersion version;
    version.platform = ModPlatform::kModrinth;
    version.project_id = modjson::str(node, "project_id");
    version.version_id = modjson::str(node, "id");
    version.name = modjson::str(node, "name");
    version.version_number = modjson::str(node, "version_number");
    version.channel = mod_channel_from_key(modjson::str(node, "version_type"));
    version.published_at = modjson::str(node, "date_published");
    version.featured = modjson::boolean(node, "featured", false);
    version.game_versions = modjson::str_array(node, "game_versions");
    for (const std::string& loader : modjson::str_array(node, "loaders")) {
        const ModLoader parsed = mod_loader_from_key(loader);
        if (parsed != ModLoader::kUnknown) {
            version.loaders.push_back(parsed);
        }
    }
    if (node.contains("files") && node["files"].is_array()) {
        for (const json& file : node["files"]) {
            if (file.is_object()) {
                version.files.push_back(parse_file_node(file));
            }
        }
    }
    if (node.contains("dependencies") && node["dependencies"].is_array()) {
        for (const json& dependency : node["dependencies"]) {
            if (!dependency.is_object()) {
                continue;
            }
            ModDependency parsed;
            parsed.kind = dependency_kind_from_key(modjson::str(dependency, "dependency_type"));
            parsed.project_id = modjson::str(dependency, "project_id");
            parsed.version_id = modjson::str(dependency, "version_id");
            parsed.file_name = modjson::str(dependency, "file_name");
            if (!parsed.project_id.empty() || !parsed.version_id.empty() ||
                !parsed.file_name.empty()) {
                version.dependencies.push_back(std::move(parsed));
            }
        }
    }
    version.downloadable = true;
    return version;
}

}  // namespace

std::string build_modrinth_search_url(const ModSearchQuery& query) {
    json facets = json::array();
    json project_type = json::array();
    project_type.push_back("project_type:mod");
    facets.push_back(std::move(project_type));
    if (!query.game_version.empty()) {
        json versions = json::array();
        versions.push_back("versions:" + query.game_version);
        facets.push_back(std::move(versions));
    }
    if (query.loader != ModLoader::kUnknown && query.loader != ModLoader::kVanilla) {
        json loaders = json::array();
        loaders.push_back(std::string("categories:") + mod_loader_key(query.loader));
        facets.push_back(std::move(loaders));
    }
    if (!query.category.empty()) {
        json categories = json::array();
        categories.push_back("categories:" + query.category);
        facets.push_back(std::move(categories));
    }

    std::string url = "https://api.modrinth.com/v2/search?limit=" +
                      std::to_string(std::clamp(query.limit, 1, 100)) +
                      "&offset=" + std::to_string((std::max)(query.offset, 0)) +
                      "&index=" + sort_key(query.sort);
    if (!query.text.empty()) {
        url += "&query=" + url_encode(query.text);
    }
    url += "&facets=" + url_encode(facets.dump());
    return url;
}

std::string build_modrinth_versions_url(const std::string& project_id,
                                        const std::string& game_version, ModLoader loader,
                                        bool all_versions) {
    std::string url = "https://api.modrinth.com/v2/project/" + url_encode(project_id) +
                      "/version?include_changelog=false";
    if (all_versions) {
        return url;
    }
    if (!game_version.empty()) {
        json versions = json::array();
        versions.push_back(game_version);
        url += "&game_versions=" + url_encode(versions.dump());
    }
    if (loader != ModLoader::kUnknown && loader != ModLoader::kVanilla) {
        json loaders = json::array();
        loaders.push_back(mod_loader_key(loader));
        url += "&loaders=" + url_encode(loaders.dump());
    }
    return url;
}

util::Result<ModSearchPage> parse_modrinth_search(const std::string& text) {
    util::Result<json> parsed = modjson::parse_object(text, "Modrinth 搜索");
    if (parsed.is_error()) {
        return util::Result<ModSearchPage>::fail(std::move(parsed.error()));
    }
    const json& root = parsed.value();
    if (!root.contains("hits") || !root["hits"].is_array()) {
        return util::Result<ModSearchPage>::fail(util::Error::make(
            "mod.api_invalid", "平台返回了无法识别的数据", "Modrinth 搜索：缺少 hits"));
    }
    ModSearchPage page;
    page.offset = static_cast<int>(modjson::integer(root, "offset", 0));
    page.limit = static_cast<int>(modjson::integer(root, "limit", 0));
    page.total = static_cast<int>(modjson::integer(root, "total_hits", -1));
    for (const json& hit : root["hits"]) {
        if (hit.is_object()) {
            page.items.push_back(parse_project_node(hit));
        }
    }
    page.has_more = page.total < 0 ? static_cast<int>(page.items.size()) >= page.limit
                                   : page.offset + static_cast<int>(page.items.size()) < page.total;
    return util::Result<ModSearchPage>::ok(std::move(page));
}

util::Result<ModProject> parse_modrinth_project(const std::string& text) {
    util::Result<json> parsed = modjson::parse_object(text, "Modrinth 项目");
    if (parsed.is_error()) {
        return util::Result<ModProject>::fail(std::move(parsed.error()));
    }
    return util::Result<ModProject>::ok(parse_project_node(parsed.value()));
}

util::Result<std::vector<ModVersion>> parse_modrinth_versions(const std::string& text) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_array()) {
        return util::Result<std::vector<ModVersion>>::fail(util::Error::make(
            "mod.api_invalid", "平台返回了无法识别的数据", "Modrinth 版本列表：不是数组"));
    }
    std::vector<ModVersion> versions;
    versions.reserve(root.size());
    for (const json& node : root) {
        if (node.is_object()) {
            versions.push_back(parse_version_node(node));
        }
    }
    return util::Result<std::vector<ModVersion>>::ok(std::move(versions));
}

util::Result<ModVersion> parse_modrinth_version(const std::string& text) {
    util::Result<json> parsed = modjson::parse_object(text, "Modrinth 版本");
    if (parsed.is_error()) {
        return util::Result<ModVersion>::fail(std::move(parsed.error()));
    }
    return util::Result<ModVersion>::ok(parse_version_node(parsed.value()));
}

std::string build_modrinth_version_files_body(const std::vector<std::string>& hashes) {
    json body;
    body["hashes"] = hashes;
    body["algorithm"] = "sha1";
    return body.dump();
}

std::string build_modrinth_update_body(const std::vector<ModUpdateQuery>& queries) {
    json hashes = json::array();
    for (const ModUpdateQuery& query : queries) {
        if (query.hash.empty()) {
            continue;
        }
        json item;
        item["hash"] = query.hash;
        json loaders = json::array();
        for (ModLoader loader : query.loaders) {
            if (loader != ModLoader::kUnknown && loader != ModLoader::kVanilla) {
                loaders.push_back(mod_loader_key(loader));
            }
        }
        item["loaders"] = std::move(loaders);
        json versions = json::array();
        if (!query.game_version.empty()) {
            versions.push_back(query.game_version);
        }
        item["game_versions"] = std::move(versions);
        hashes.push_back(std::move(item));
    }
    json body;
    body["algorithm"] = "sha1";
    body["hashes"] = std::move(hashes);
    return body.dump();
}

util::Result<std::map<std::string, ModVersion>> parse_modrinth_version_map(
    const std::string& text) {
    util::Result<json> root = modjson::parse_object(text, "Modrinth 版本映射");
    if (root.is_error()) {
        return util::Result<std::map<std::string, ModVersion>>::fail(std::move(root.error()));
    }
    std::map<std::string, ModVersion> result;
    for (auto it = root.value().begin(); it != root.value().end(); ++it) {
        if (!it.value().is_object()) {
            continue;
        }
        util::Result<ModVersion> version = parse_modrinth_version(it.value().dump());
        if (version.is_ok()) {
            result[it.key()] = std::move(version.value());
        }
    }
    return util::Result<std::map<std::string, ModVersion>>::ok(std::move(result));
}

util::Result<ModSearchPage> ModrinthClient::search(const ModSearchQuery& query,
                                                   const std::stop_token& stop) {
    const std::string url = build_modrinth_search_url(query);
    bool stale = false;
    util::Result<std::string> body =
        m_http.get_json(url, std::chrono::seconds(600), stop, &stale);
    if (body.is_error()) {
        return util::Result<ModSearchPage>::fail(std::move(body.error()));
    }
    util::Result<ModSearchPage> page = parse_modrinth_search(body.value());
    if (page.is_ok() && stale) {
        page.value().warning = "网络不可用，显示的是离线缓存";
    }
    return page;
}

util::Result<ModProject> ModrinthClient::project(const std::string& id,
                                                 const std::stop_token& stop) {
    const std::string url = "https://api.modrinth.com/v2/project/" + url_encode(id);
    bool stale = false;
    util::Result<std::string> body =
        m_http.get_json(url, std::chrono::seconds(86400), stop, &stale);
    if (body.is_error()) {
        return util::Result<ModProject>::fail(std::move(body.error()));
    }
    return parse_modrinth_project(body.value());
}

util::Result<std::vector<ModVersion>> ModrinthClient::versions(
    const std::string& project_id, const std::string& game_version, ModLoader loader,
    bool all_versions, const std::stop_token& stop) {
    const std::string url =
        build_modrinth_versions_url(project_id, game_version, loader, all_versions);
    bool stale = false;
    util::Result<std::string> body =
        m_http.get_json(url, std::chrono::seconds(1800), stop, &stale);
    if (body.is_error()) {
        return util::Result<std::vector<ModVersion>>::fail(std::move(body.error()));
    }
    return parse_modrinth_versions(body.value());
}

util::Result<ModVersion> ModrinthClient::version(const std::string& project_id,
                                                 const std::string& version_id,
                                                 const std::stop_token& stop) {
    const std::string url = "https://api.modrinth.com/v2/version/" + url_encode(version_id);
    bool stale = false;
    util::Result<std::string> body =
        m_http.get_json(url, std::chrono::seconds(86400), stop, &stale);
    if (body.is_error()) {
        return util::Result<ModVersion>::fail(std::move(body.error()));
    }
    util::Result<ModVersion> parsed = parse_modrinth_version(body.value());
    if (parsed.is_ok() && parsed.value().project_id.empty()) {
        parsed.value().project_id = project_id;
    }
    return parsed;
}

util::Result<std::map<std::string, ModVersion>> ModrinthClient::version_files(
    const std::vector<std::string>& hashes, const std::stop_token& stop) {
    if (hashes.empty()) {
        return util::Result<std::map<std::string, ModVersion>>::ok({});
    }
    json body;
    body["hashes"] = hashes;
    body["algorithm"] = "sha1";
    util::Result<std::string> text = m_http.post_json(
        "https://api.modrinth.com/v2/version_files", body.dump(), stop);
    if (text.is_error()) {
        return util::Result<std::map<std::string, ModVersion>>::fail(std::move(text.error()));
    }
    util::Result<json> root = modjson::parse_object(text.value(), "Modrinth version_files");
    if (root.is_error()) {
        return util::Result<std::map<std::string, ModVersion>>::fail(std::move(root.error()));
    }
    std::map<std::string, ModVersion> result;
    for (auto it = root.value().begin(); it != root.value().end(); ++it) {
        if (!it.value().is_object()) {
            continue;
        }
        util::Result<ModVersion> version = parse_modrinth_version(it.value().dump());
        if (version.is_ok()) {
            result[it.key()] = std::move(version.value());
        }
    }
    return util::Result<std::map<std::string, ModVersion>>::ok(std::move(result));
}

util::Result<std::map<std::string, ModVersion>> ModrinthClient::update_individual(
    const std::vector<ModUpdateQuery>& queries, const std::stop_token& stop) {
    if (queries.empty()) {
        return util::Result<std::map<std::string, ModVersion>>::ok({});
    }
    json hashes = json::array();
    for (const ModUpdateQuery& query : queries) {
        if (query.hash.empty()) {
            continue;
        }
        json item;
        item["hash"] = query.hash;
        json loaders = json::array();
        for (ModLoader loader : query.loaders) {
            if (loader != ModLoader::kUnknown && loader != ModLoader::kVanilla) {
                loaders.push_back(mod_loader_key(loader));
            }
        }
        item["loaders"] = std::move(loaders);
        json versions = json::array();
        if (!query.game_version.empty()) {
            versions.push_back(query.game_version);
        }
        item["game_versions"] = std::move(versions);
        hashes.push_back(std::move(item));
    }
    json body;
    body["algorithm"] = "sha1";
    body["hashes"] = std::move(hashes);
    util::Result<std::string> text = m_http.post_json(
        "https://api.modrinth.com/v2/version_files/update_individual", body.dump(), stop);
    if (text.is_error()) {
        return util::Result<std::map<std::string, ModVersion>>::fail(std::move(text.error()));
    }
    util::Result<json> root = modjson::parse_object(text.value(), "Modrinth update_individual");
    if (root.is_error()) {
        return util::Result<std::map<std::string, ModVersion>>::fail(std::move(root.error()));
    }
    std::map<std::string, ModVersion> result;
    for (auto it = root.value().begin(); it != root.value().end(); ++it) {
        if (!it.value().is_object()) {
            continue;
        }
        util::Result<ModVersion> version = parse_modrinth_version(it.value().dump());
        if (version.is_ok()) {
            result[it.key()] = std::move(version.value());
        }
    }
    return util::Result<std::map<std::string, ModVersion>>::ok(std::move(result));
}

}  // namespace sscl::core
