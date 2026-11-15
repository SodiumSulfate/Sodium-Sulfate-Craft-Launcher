#include "core/loader/loader_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/download/mirror_map.hpp"
#include "core/game/game_service.hpp"  // compare_version_ids（自然序）
#include "platform/file_utils.hpp"
#include "platform/http_client.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

namespace sscl::core {

namespace {

using nlohmann::json;

constexpr const char* kFabricMetaBase = "https://meta.fabricmc.net/v2/";
constexpr const char* kFabricMetaMirror = "https://bmclapi2.bangbang93.com/fabric-meta/v2/";
constexpr const char* kForgeMavenBase =
    "https://maven.minecraftforge.net/net/minecraftforge/forge/";
constexpr const char* kForgeMavenMirror =
    "https://bmclapi2.bangbang93.com/maven/net/minecraftforge/forge/";
constexpr const char* kForgePromotionsUrl =
    "https://files.minecraftforge.net/net/minecraftforge/forge/promotions_slim.json";
constexpr const char* kForgePromotionsMirror =
    "https://bmclapi2.bangbang93.com/forge/promotions_slim.json";
constexpr const char* kForgeBmclListBase = "https://bmclapi2.bangbang93.com/forge/minecraft/";
constexpr const char* kNeoForgeApiBase =
    "https://maven.neoforged.net/api/maven/versions/releases/net/neoforged/";
constexpr const char* kNeoForgeApiMirror =
    "https://bmclapi2.bangbang93.com/neoforge/meta/api/maven/details/releases/net/neoforged/";
constexpr const char* kNeoForgeMavenMirror =
    "https://bmclapi2.bangbang93.com/maven/net/neoforged/";
constexpr const char* kNeoForgeReleasesMirror =
    "https://bmclapi2.bangbang93.com/maven/net/neoforged/";

// D-107：加载器清单属于「下载及列表网络源」，镜像优先档反转官方/镜像顺序。
bool prefer_mirror_source() {
    return global_source_policy().files_prefer_mirror();
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : text) {
        if (c == separator) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    return parts;
}

bool parse_int(const std::string& text, int* out) {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
    }
    try {
        *out = std::stoi(text);
        return true;
    } catch (...) {
        return false;
    }
}

std::string url_encode_component(const std::string& text) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text) {
        const bool safe = std::isalnum(c) != 0 || c == '-' || c == '.' || c == '_' || c == '~';
        if (safe) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

// 简单降序：按自然序（数字段按数值）。相等返回 false。
bool newer_first(const std::string& left, const std::string& right) {
    return compare_version_ids(left, right) > 0;
}

util::Result<std::string> get_text(const std::string& url, int timeout_ms,
                                   std::stop_token stop) {
    if (stop.stop_requested()) {
        return util::Result<std::string>::fail(
            util::Error::make("loader.cancelled", "已取消", {}));
    }
    platform::HttpRequest request;
    request.url = url;
    request.receive_timeout_ms = timeout_ms;
    request.connect_timeout_ms = timeout_ms;
    request.resolve_timeout_ms = timeout_ms;
    request.headers.emplace_back("User-Agent", "SSCL/0.1.0 (+https://github.com/SSCL)");
    util::Result<platform::HttpResponse> response = platform::http_request(request);
    if (response.is_error()) {
        return util::Result<std::string>::fail(util::Error::make(
            "loader.catalog_failed", "加载器清单请求失败", url + " :: " + response.error().detail));
    }
    if (response.value().status_code != 200) {
        return util::Result<std::string>::fail(util::Error::make(
            "loader.catalog_failed", "加载器清单请求返回非 200",
            url + " :: HTTP " + std::to_string(response.value().status_code)));
    }
    return util::Result<std::string>::ok(std::move(response.value().body));
}

// ---------------- 缓存 ----------------

json entry_to_json(const LoaderEntry& entry) {
    json out;
    out["version"] = entry.version;
    out["minecraft_version"] = entry.minecraft_version;
    out["stable"] = entry.stable;
    out["recommended"] = entry.recommended;
    out["download_url"] = entry.download_url;
    out["full_version"] = entry.full_version;
    return out;
}

LoaderEntry entry_from_json(const json& node) {
    LoaderEntry entry;
    entry.version = node.value("version", std::string());
    entry.minecraft_version = node.value("minecraft_version", std::string());
    entry.stable = node.value("stable", true);
    entry.recommended = node.value("recommended", false);
    entry.download_url = node.value("download_url", std::string());
    entry.full_version = node.value("full_version", std::string());
    entry.from_cache = true;
    return entry;
}

std::filesystem::path cache_path(const LoaderCatalogOptions& options, LoaderKind kind,
                                 const std::string& mc_version) {
    if (options.cache_dir.empty()) {
        return {};
    }
    return options.cache_dir / (std::string(loader_kind_key(kind)) + "-" + mc_version + ".json");
}

bool cache_is_fresh(const std::filesystem::path& path, int ttl_seconds) {
    std::error_code ec;
    const auto write_time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return false;
    }
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                         std::filesystem::file_time_type::clock::now() - write_time)
                         .count();
    return age >= 0 && age < ttl_seconds;
}

util::Result<std::vector<LoaderEntry>> read_cache(const std::filesystem::path& path) {
    if (path.empty()) {
        return util::Result<std::vector<LoaderEntry>>::fail(
            util::Error::make("loader.cache_missing", "无缓存", {}));
    }
    util::Result<std::string> text = platform::read_file(path);
    if (text.is_error()) {
        return util::Result<std::vector<LoaderEntry>>::fail(text.error());
    }
    json root = json::parse(text.value(), nullptr, false);
    if (root.is_discarded() || !root.is_object() || !root.contains("entries") ||
        !root["entries"].is_array()) {
        return util::Result<std::vector<LoaderEntry>>::fail(
            util::Error::make("loader.cache_invalid", "缓存格式不正确",
                              util::path_to_utf8(path)));
    }
    std::vector<LoaderEntry> entries;
    for (const json& node : root["entries"]) {
        if (node.is_object()) {
            entries.push_back(entry_from_json(node));
        }
    }
    return util::Result<std::vector<LoaderEntry>>::ok(std::move(entries));
}

void write_cache(const std::filesystem::path& path, const std::vector<LoaderEntry>& entries) {
    if (path.empty()) {
        return;
    }
    json root;
    root["fetched_at"] = static_cast<std::int64_t>(std::time(nullptr));
    root["entries"] = json::array();
    for (const LoaderEntry& entry : entries) {
        root["entries"].push_back(entry_to_json(entry));
    }
    util::Result<void> created = platform::create_directories(path.parent_path());
    if (created.is_error()) {
        util::log_warn("[loader] 缓存目录创建失败：" + created.error().detail);
        return;
    }
    util::Result<void> written = platform::write_file_atomic(path, root.dump(2));
    if (written.is_error()) {
        util::log_warn("[loader] 缓存写入失败：" + written.error().detail);
    }
}

}  // namespace

util::Result<std::vector<LoaderEntry>> parse_fabric_loader_versions(
    const std::string& loader_json, const std::string& mc_version) {
    json root = json::parse(loader_json, nullptr, false);
    if (root.is_discarded() || !root.is_array()) {
        return util::Result<std::vector<LoaderEntry>>::fail(util::Error::make(
            "loader.catalog_invalid", "Fabric 版本清单格式不正确", {}));
    }
    std::vector<LoaderEntry> entries;
    for (const json& node : root) {
        if (!node.is_object()) {
            continue;
        }
        const std::string version = node.value("version", std::string());
        if (version.empty()) {
            continue;
        }
        LoaderEntry entry;
        entry.version = version;
        entry.minecraft_version = mc_version;
        entry.stable = node.value("stable", false);
        entry.download_url = std::string(kFabricMetaBase) + "versions/loader/" +
                             url_encode_component(mc_version) + "/" +
                             url_encode_component(version) + "/profile/json";
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(), [](const LoaderEntry& left, const LoaderEntry& right) {
        return newer_first(left.version, right.version);
    });
    return util::Result<std::vector<LoaderEntry>>::ok(std::move(entries));
}

util::Result<ForgeVersionId> parse_forge_version_id(const std::string& full_version) {
    const std::string text = trim(full_version);
    const std::size_t dash = text.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= text.size()) {
        return util::Result<ForgeVersionId>::fail(util::Error::make(
            "loader.version_invalid", "Forge 版本号格式不正确", full_version));
    }
    ForgeVersionId parsed;
    parsed.minecraft = text.substr(0, dash);
    std::string rest = text.substr(dash + 1);
    // 容忍尾部重复的 "-<mc>"（如 1.7.10-10.13.4.1614-1.7.10）
    const std::string trailing = "-" + parsed.minecraft;
    if (ends_with(rest, trailing) && rest.size() > trailing.size()) {
        rest.resize(rest.size() - trailing.size());
    }
    if (rest.empty()) {
        return util::Result<ForgeVersionId>::fail(util::Error::make(
            "loader.version_invalid", "Forge 版本号缺少加载器版本", full_version));
    }
    // 分支：剩余部分含 '-' 时，最后一段视为 branch（如 1.7.10-10.13.4.1614-prerelease）
    const std::size_t branch_dash = rest.rfind('-');
    if (branch_dash != std::string::npos && branch_dash + 1 < rest.size()) {
        parsed.loader = rest.substr(0, branch_dash);
        parsed.branch = rest.substr(branch_dash + 1);
    } else {
        parsed.loader = rest;
    }
    if (parsed.loader.empty()) {
        return util::Result<ForgeVersionId>::fail(util::Error::make(
            "loader.version_invalid", "Forge 版本号缺少加载器版本", full_version));
    }
    return util::Result<ForgeVersionId>::ok(std::move(parsed));
}

util::Result<ForgePromotions> parse_forge_promotions(const std::string& json_text) {
    ForgePromotions promotions;
    json root = json::parse(json_text, nullptr, false);
    if (root.is_discarded() || !root.is_object() || !root.contains("promos") ||
        !root["promos"].is_object()) {
        // promotions 仅用于"推荐/最新"标记，缺失不算致命：返回空表
        return util::Result<ForgePromotions>::ok(std::move(promotions));
    }
    for (auto it = root["promos"].begin(); it != root["promos"].end(); ++it) {
        if (it.value().is_string()) {
            promotions.promos[it.key()] = it.value().get<std::string>();
        }
    }
    return util::Result<ForgePromotions>::ok(std::move(promotions));
}

util::Result<std::vector<LoaderEntry>> parse_forge_versions(const MavenMetadata& metadata,
                                                           const ForgePromotions& promotions,
                                                           const std::string& mc_version) {
    std::vector<LoaderEntry> entries;
    std::set<std::string> seen;
    for (const std::string& full : metadata.versions) {
        util::Result<ForgeVersionId> parsed = parse_forge_version_id(full);
        if (parsed.is_error()) {
            continue;
        }
        const ForgeVersionId& id = parsed.value();
        if (!mc_version.empty() && id.minecraft != mc_version) {
            continue;
        }
        if (!seen.insert(id.loader + "|" + id.branch).second) {
            continue;
        }
        LoaderEntry entry;
        entry.version = id.loader;
        entry.minecraft_version = id.minecraft;
        entry.full_version = full;
        entry.recommended =
            promotions.promos.find(id.minecraft + "-recommended") != promotions.promos.end() &&
            promotions.promos.at(id.minecraft + "-recommended") == id.loader;
        entry.stable = entry.recommended;
        // 安装器：BMCLAPI maven 镜像优先（国内可达性优于官方；PCL 同款取舍）
        entry.download_url = std::string(kForgeMavenMirror) + full + "/forge-" + full +
                             "-installer.jar";
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) {
        return util::Result<std::vector<LoaderEntry>>::fail(util::Error::make(
            "loader.no_compatible_version", "该游戏版本没有可用的 Forge 版本",
            "mc=" + mc_version));
    }
    std::sort(entries.begin(), entries.end(), [](const LoaderEntry& left, const LoaderEntry& right) {
        return newer_first(left.version, right.version);
    });
    // recommended 置顶（Axolotl forge_manifest 同款处理）
    const auto recommended = std::find_if(entries.begin(), entries.end(),
                                          [](const LoaderEntry& entry) { return entry.recommended; });
    if (recommended != entries.end() && recommended != entries.begin()) {
        std::rotate(entries.begin(), recommended, recommended + 1);
    }
    return util::Result<std::vector<LoaderEntry>>::ok(std::move(entries));
}

util::Result<std::vector<LoaderEntry>> parse_forge_bmclapi_versions(
    const std::string& json_text, const std::string& mc_version,
    const ForgePromotions& promotions) {
    json root = json::parse(json_text, nullptr, false);
    if (root.is_discarded() || !root.is_array()) {
        return util::Result<std::vector<LoaderEntry>>::fail(util::Error::make(
            "loader.catalog_invalid", "BMCLAPI Forge 清单格式不正确", {}));
    }
    std::vector<LoaderEntry> entries;
    for (const json& node : root) {
        if (!node.is_object()) {
            continue;
        }
        const std::string version = node.value("version", std::string());
        if (version.empty()) {
            continue;
        }
        const std::string node_mc = node.value("mcversion", mc_version);
        if (!mc_version.empty() && node_mc != mc_version) {
            continue;
        }
        LoaderEntry entry;
        entry.version = version;
        entry.minecraft_version = node_mc;
        const std::string branch = node.value("branch", std::string());
        entry.full_version = node_mc + "-" + version + (branch.empty() ? "" : "-" + branch);
        const auto recommended_it = promotions.promos.find(node_mc + "-recommended");
        entry.recommended = recommended_it != promotions.promos.end() &&
                            recommended_it->second == version;
        entry.stable = entry.recommended;
        entry.download_url = "https://bmclapi2.bangbang93.com/forge/download?mcversion=" +
                             url_encode_component(node_mc) + "&version=" +
                             url_encode_component(version) +
                             "&branch=" + url_encode_component(branch) +
                             "&category=installer&format=jar";
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) {
        return util::Result<std::vector<LoaderEntry>>::fail(util::Error::make(
            "loader.no_compatible_version", "该游戏版本没有可用的 Forge 版本",
            "mc=" + mc_version));
    }
    std::sort(entries.begin(), entries.end(), [](const LoaderEntry& left, const LoaderEntry& right) {
        return newer_first(left.version, right.version);
    });
    return util::Result<std::vector<LoaderEntry>>::ok(std::move(entries));
}

std::string neoforge_minecraft_version(const std::string& neoforge_version) {
    const std::string text = trim(neoforge_version);
    if (text.empty()) {
        return {};
    }
    // 1) legacy：1.20.1-47.x
    if (starts_with(text, "1.")) {
        const std::size_t dash = text.find('-');
        if (dash == std::string::npos) {
            return {};
        }
        return text.substr(0, dash);
    }
    // 2) 快照：0.<snapshot>.<build>[-...]
    if (starts_with(text, "0.")) {
        const std::size_t first = text.find('.');
        const std::size_t second = text.find('.', first + 1);
        if (first == std::string::npos || second == std::string::npos) {
            return {};
        }
        return text.substr(first + 1, second - first - 1);
    }
    // 3/4) 数字形态：20.4.30 / 26.1.0.0-alpha.1+snapshot-1
    std::string base = text;
    std::string build;
    const std::size_t plus = text.find('+');
    if (plus != std::string::npos) {
        base = text.substr(0, plus);
        build = text.substr(plus + 1);
    }
    const std::size_t dash = base.find('-');
    if (dash != std::string::npos) {
        base.resize(dash);
    }
    const std::vector<std::string> segments = split(base, '.');
    if (segments.size() < 2) {
        return {};
    }
    int major = 0;
    int minor = 0;
    if (!parse_int(segments[0], &major) || !parse_int(segments[1], &minor)) {
        return {};
    }
    std::string mc;
    if (major >= 26) {
        if (segments.size() < 3) {
            return {};
        }
        int patch = 0;
        if (!parse_int(segments[2], &patch)) {
            return {};
        }
        mc = patch == 0 ? segments[0] + "." + segments[1]
                        : segments[0] + "." + segments[1] + "." + segments[2];
    } else {
        mc = minor == 0 ? "1." + segments[0] : "1." + segments[0] + "." + segments[1];
    }
    if (!build.empty()) {
        mc += "-" + build;
    }
    return mc;
}

std::string neoforge_loader_display(const std::string& neoforge_version) {
    const std::string text = trim(neoforge_version);
    if (starts_with(text, "1.")) {
        const std::size_t dash = text.find('-');
        if (dash != std::string::npos && dash + 1 < text.size()) {
            return text.substr(dash + 1);
        }
    }
    return text;
}

namespace {

// NeoForge 单条版本 → LoaderEntry（派生 MC；legacy 用 forge 命名空间与下载路径）。
bool append_neoforge_entry(const std::string& full, bool legacy, const std::string& mc_version,
                           std::vector<LoaderEntry>* entries) {
    const std::string derived_mc = neoforge_minecraft_version(full);
    if (derived_mc.empty()) {
        return false;
    }
    if (!mc_version.empty() && derived_mc != mc_version) {
        return false;
    }
    LoaderEntry entry;
    entry.version = neoforge_loader_display(full);
    entry.minecraft_version = derived_mc;
    entry.full_version = full;
    const std::string lower = [&full]() {
        std::string value = full;
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }();
    entry.stable =
        lower.find("beta") == std::string::npos && lower.find("alpha") == std::string::npos;
    // 镜像优先（国内网络更稳），官方为兜底；两者产物相同
    if (legacy) {
        entry.download_url = std::string(kNeoForgeReleasesMirror) + "forge/" + full + "/forge-" +
                             full + "-installer.jar";
    } else {
        entry.download_url = std::string(kNeoForgeReleasesMirror) + "neoforge/" + full +
                             "/neoforge-" + full + "-installer.jar";
    }
    entries->push_back(std::move(entry));
    return true;
}

util::Result<std::vector<LoaderEntry>> finalize_neoforge_entries(
    std::vector<LoaderEntry> entries, const std::string& mc_version) {
    if (entries.empty()) {
        return util::Result<std::vector<LoaderEntry>>::fail(util::Error::make(
            "loader.no_compatible_version", "该游戏版本没有可用的 NeoForge 版本",
            "mc=" + mc_version));
    }
    std::sort(entries.begin(), entries.end(),
              [](const LoaderEntry& left, const LoaderEntry& right) {
                  return newer_first(left.version, right.version);
              });
    return util::Result<std::vector<LoaderEntry>>::ok(std::move(entries));
}

}  // namespace

util::Result<std::vector<LoaderEntry>> parse_neoforge_version_list(
    const std::vector<std::string>& versions, bool legacy, const std::string& mc_version) {
    std::vector<LoaderEntry> entries;
    for (const std::string& full : versions) {
        append_neoforge_entry(full, legacy, mc_version, &entries);
    }
    return finalize_neoforge_entries(std::move(entries), mc_version);
}

util::Result<std::vector<LoaderEntry>> parse_neoforge_detail_versions(
    const std::string& detail_json, bool legacy, const std::string& mc_version) {
    if (trim(detail_json).empty()) {
        return util::Result<std::vector<LoaderEntry>>::fail(util::Error::make(
            "loader.catalog_invalid", "NeoForge 明细清单为空", {}));
    }
    json root = json::parse(detail_json, nullptr, false);
    if (root.is_discarded() || !root.is_object() || !root.contains("files") ||
        !root["files"].is_array()) {
        return util::Result<std::vector<LoaderEntry>>::fail(util::Error::make(
            "loader.catalog_invalid", "NeoForge 明细清单格式不正确", {}));
    }
    std::vector<LoaderEntry> entries;
    for (const json& node : root["files"]) {
        if (!node.is_object() || !node.contains("name") || !node["name"].is_string()) {
            continue;
        }
        const std::string name = node["name"].get<std::string>();
        if (name.find("maven-metadata") != std::string::npos) {
            continue;
        }
        const std::string type = node.value("type", std::string());
        if (type != "DIRECTORY") {
            continue;  // 目录才是版本号；FILE 是 metadata/校验文件
        }
        append_neoforge_entry(name, legacy, mc_version, &entries);
    }
    return finalize_neoforge_entries(std::move(entries), mc_version);
}

util::Result<std::vector<LoaderEntry>> parse_neoforge_versions(const std::string& current_json,
                                                              const std::string& legacy_json,
                                                              const std::string& mc_version) {
    std::vector<LoaderEntry> entries;

    const auto append_versions = [&entries, &mc_version](const std::string& text,
                                                         bool legacy) -> util::Result<void> {
        if (trim(text).empty()) {
            return util::Result<void>::ok();
        }
        json root = json::parse(text, nullptr, false);
        if (root.is_discarded() || !root.is_object() || !root.contains("versions") ||
            !root["versions"].is_array()) {
            return util::Result<void>::fail(util::Error::make(
                "loader.catalog_invalid", "NeoForge 版本清单格式不正确", {}));
        }
        for (const json& node : root["versions"]) {
            if (!node.is_string()) {
                continue;
            }
            append_neoforge_entry(node.get<std::string>(), legacy, mc_version, &entries);
        }
        return util::Result<void>::ok();
    };

    util::Result<void> current_result = append_versions(current_json, false);
    if (current_result.is_error()) {
        return util::Result<std::vector<LoaderEntry>>::fail(current_result.error());
    }
    util::Result<void> legacy_result = append_versions(legacy_json, true);
    if (legacy_result.is_error()) {
        return util::Result<std::vector<LoaderEntry>>::fail(legacy_result.error());
    }
    return finalize_neoforge_entries(std::move(entries), mc_version);
}

util::Result<std::vector<LoaderEntry>> fetch_loader_versions(
    LoaderKind kind, const std::string& mc_version, const LoaderCatalogOptions& options,
    std::stop_token stop) {
    if (kind == LoaderKind::kNone || mc_version.empty()) {
        return util::Result<std::vector<LoaderEntry>>::fail(util::Error::make(
            "loader.catalog_failed", "加载器类型或游戏版本为空", {}));
    }

    const std::filesystem::path cache = cache_path(options, kind, mc_version);
    if (!cache.empty() && cache_is_fresh(cache, options.cache_ttl_seconds)) {
        util::Result<std::vector<LoaderEntry>> cached = read_cache(cache);
        if (cached.is_ok() && !cached.value().empty()) {
            return cached;
        }
    }

    util::Result<std::vector<LoaderEntry>> fetched =
        util::Result<std::vector<LoaderEntry>>::fail(
            util::Error::make("loader.catalog_failed", "加载器清单获取失败", {}));

    switch (kind) {
        case LoaderKind::kFabric: {
            const std::vector<const char*> fabric_bases =
                prefer_mirror_source()
                    ? std::vector<const char*>{kFabricMetaMirror, kFabricMetaBase}
                    : std::vector<const char*>{kFabricMetaBase, kFabricMetaMirror};
            // Result<T> 只有 ok()/fail() 工厂，没有默认构造函数（编译回传 #85 修复）。
            // 这里给一个初始失败值，循环内会被每个候选的 get_text 结果覆盖。
            util::Result<std::string> text = util::Result<std::string>::fail(
                util::Error::make("loader.catalog_failed", "加载器清单获取失败", {}));
            for (const char* base : fabric_bases) {
                text = get_text(std::string(base) + "versions/loader",
                                options.request_timeout_ms, stop);
                if (text.is_ok()) {
                    break;
                }
            }
            if (text.is_ok()) {
                // R-LDR-104：先用 /versions/game 校验该 MC 是否被 Fabric 支持
                util::Result<std::string> games = get_text(
                    std::string(kFabricMetaBase) + "versions/game",
                    options.request_timeout_ms, stop);
                bool supported = games.is_error();  // 校验请求失败时不阻断（列表仍可用）
                if (games.is_ok()) {
                    json game_root = json::parse(games.value(), nullptr, false);
                    if (game_root.is_discarded() || !game_root.is_array()) {
                        supported = true;
                    } else {
                        for (const json& node : game_root) {
                            if (node.is_object() && node.value("version", std::string()) ==
                                                         mc_version) {
                                supported = true;
                                break;
                            }
                        }
                    }
                }
                if (!supported) {
                    fetched = util::Result<std::vector<LoaderEntry>>::fail(util::Error::make(
                        "loader.no_compatible_version", "该游戏版本没有可用的 Fabric 版本",
                        "mc=" + mc_version));
                } else {
                    fetched = parse_fabric_loader_versions(text.value(), mc_version);
                }
            } else {
                fetched = util::Result<std::vector<LoaderEntry>>::fail(text.error());
            }
            break;
        }
        case LoaderKind::kForge: {
            // promotions：BMCLAPI 镜像 → 官方；失败只影响"推荐"标记，不阻断列表
            ForgePromotions promotions;
            const std::vector<const char*> promotion_urls =
                prefer_mirror_source()
                    ? std::vector<const char*>{kForgePromotionsMirror, kForgePromotionsUrl}
                    : std::vector<const char*>{kForgePromotionsUrl, kForgePromotionsMirror};
            for (const char* promotion_url : promotion_urls) {
                util::Result<std::string> text =
                    get_text(promotion_url, options.request_timeout_ms, stop);
                if (text.is_error()) {
                    util::log_warn("[loader] forge promotions 获取失败：" +
                                   std::string(promotion_url) + " :: " + text.error().detail);
                    continue;
                }
                util::Result<ForgePromotions> parsed = parse_forge_promotions(text.value());
                if (parsed.is_ok()) {
                    promotions = std::move(parsed.value());
                    break;
                }
            }

            // 元数据：BMCLAPI maven 镜像 → 官方 maven；**解析失败也回退**（挑战页/HTML 情形）
            const std::vector<const char*> maven_bases =
                prefer_mirror_source()
                    ? std::vector<const char*>{kForgeMavenMirror, kForgeMavenBase}
                    : std::vector<const char*>{kForgeMavenBase, kForgeMavenMirror};
            for (const char* base : maven_bases) {
                util::Result<std::string> metadata =
                    get_text(std::string(base) + "maven-metadata.xml",
                             options.request_timeout_ms, stop);
                if (metadata.is_error()) {
                    util::log_warn("[loader] forge metadata 获取失败：" + std::string(base) +
                                   " :: " + metadata.error().detail);
                    continue;
                }
                util::Result<MavenMetadata> parsed = parse_maven_metadata(metadata.value());
                if (parsed.is_error()) {
                    util::log_warn("[loader] forge metadata 解析失败：" + std::string(base));
                    continue;
                }
                util::Result<std::vector<LoaderEntry>> attempt =
                    parse_forge_versions(parsed.value(), promotions, mc_version);
                if (attempt.is_ok()) {
                    fetched = std::move(attempt);
                    break;
                }
                util::log_warn("[loader] forge metadata 无可用版本：" + std::string(base) +
                               " :: " + attempt.error().detail);
            }

            // 最后兜底：BMCLAPI Forge 版本列表（PCL `ModDownload.vb:744` 同款数据源）
            if (!fetched.is_ok()) {
                util::Result<std::string> list = get_text(
                    std::string(kForgeBmclListBase) + url_encode_component(mc_version),
                    options.request_timeout_ms, stop);
                if (list.is_ok()) {
                    fetched = parse_forge_bmclapi_versions(list.value(), mc_version, promotions);
                } else {
                    util::log_warn("[loader] forge BMCL 列表获取失败：" + list.error().detail);
                }
            }
            break;
        }
        case LoaderKind::kNeoForge: {
            std::vector<LoaderEntry> entries;
            bool got_any = false;
            const auto absorb = [&entries, &got_any](
                                    util::Result<std::vector<LoaderEntry>>&& result) {
                if (result.is_error()) {
                    return;
                }
                got_any = true;
                for (LoaderEntry& entry : result.value()) {
                    entries.push_back(std::move(entry));
                }
            };

            // D-107：镜像优先档先取 BMCL 明细；其余档位官方 API 优先。
            const auto fetch_mirror_details = [&]() {
                util::Result<std::string> current_mirror =
                    get_text(std::string(kNeoForgeApiMirror) + "neoforge",
                             options.request_timeout_ms, stop);
                util::Result<std::string> legacy_mirror =
                    get_text(std::string(kNeoForgeApiMirror) + "forge", options.request_timeout_ms,
                             stop);
                if (current_mirror.is_ok()) {
                    absorb(parse_neoforge_detail_versions(current_mirror.value(), false, mc_version));
                }
                if (legacy_mirror.is_ok()) {
                    absorb(parse_neoforge_detail_versions(legacy_mirror.value(), true, mc_version));
                }
            };
            const auto fetch_official_api = [&]() {
                util::Result<std::string> current =
                    get_text(std::string(kNeoForgeApiBase) + "neoforge",
                             options.request_timeout_ms, stop);
                util::Result<std::string> legacy =
                    get_text(std::string(kNeoForgeApiBase) + "forge", options.request_timeout_ms,
                             stop);
                if (current.is_ok() || legacy.is_ok()) {
                    absorb(parse_neoforge_versions(current.is_ok() ? current.value() : "",
                                                   legacy.is_ok() ? legacy.value() : "",
                                                   mc_version));
                } else {
                    util::log_warn("[loader] neoforge 官方 API 获取失败：" +
                                   current.error().detail);
                }
            };
            if (prefer_mirror_source()) {
                fetch_mirror_details();
                if (!got_any) {
                    fetch_official_api();
                }
            } else {
                fetch_official_api();
                if (!got_any) {
                    fetch_mirror_details();
                }
            }

            // ③ BMCLAPI maven-metadata 镜像（最后兜底）
            if (!got_any) {
                const std::pair<const char*, bool> artifacts[] = {
                    {"neoforge/maven-metadata.xml", false},
                    {"forge/maven-metadata.xml", true},
                };
                for (const auto& [path, is_legacy] : artifacts) {
                    util::Result<std::string> metadata =
                        get_text(std::string(kNeoForgeMavenMirror) + path,
                                 options.request_timeout_ms, stop);
                    if (metadata.is_error()) {
                        continue;
                    }
                    util::Result<MavenMetadata> parsed = parse_maven_metadata(metadata.value());
                    if (parsed.is_error()) {
                        continue;
                    }
                    absorb(parse_neoforge_version_list(parsed.value().versions, is_legacy,
                                                       mc_version));
                }
            }

            if (!entries.empty()) {
                std::sort(entries.begin(), entries.end(),
                          [](const LoaderEntry& left, const LoaderEntry& right) {
                              if (left.version == right.version) {
                                  return left.full_version < right.full_version;
                              }
                              return newer_first(left.version, right.version);
                          });
                entries.erase(std::unique(entries.begin(), entries.end(),
                                          [](const LoaderEntry& left, const LoaderEntry& right) {
                                              return left.version == right.version;
                                          }),
                              entries.end());
                fetched = util::Result<std::vector<LoaderEntry>>::ok(std::move(entries));
            } else {
                fetched = util::Result<std::vector<LoaderEntry>>::fail(util::Error::make(
                    "loader.no_compatible_version", "该游戏版本没有可用的 NeoForge 版本",
                    "mc=" + mc_version));
            }
            break;
        }
        case LoaderKind::kNone:
        default:
            break;
    }

    if (fetched.is_ok()) {
        write_cache(cache, fetched.value());
        util::log_info("[loader] " + std::string(loader_kind_key(kind)) + " " + mc_version +
                       " 清单：" + std::to_string(fetched.value().size()) + " 项");
        return fetched;
    }

    // 网络失败 → 过期缓存兜底
    if (!cache.empty()) {
        util::Result<std::vector<LoaderEntry>> cached = read_cache(cache);
        if (cached.is_ok() && !cached.value().empty()) {
            util::log_warn("[loader] 清单获取失败，使用过期缓存：" +
                           util::path_to_utf8(cache));
            std::vector<LoaderEntry> entries = std::move(cached.value());
            for (LoaderEntry& entry : entries) {
                entry.from_cache = true;
            }
            return util::Result<std::vector<LoaderEntry>>::ok(std::move(entries));
        }
    }
    return fetched;
}

}  // namespace sscl::core
