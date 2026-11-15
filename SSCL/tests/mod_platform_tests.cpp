#include <chrono>
#include <filesystem>
#include <string>

#include <doctest/doctest.h>

#include "core/modplatform/mod_api.hpp"
#include "core/modplatform/mod_cache.hpp"
#include "core/modplatform/mod_types.hpp"
#include "core/modplatform/modrinth_client.hpp"

// 测试用例在全局命名空间；显式引入，避免依赖 ADL。
using namespace sscl::core;

namespace {

namespace fs = std::filesystem;
using sscl::core::DependencyKind;
using sscl::core::ModCache;
using sscl::core::ModFile;
using sscl::core::ModHttpClient;
using sscl::core::ModHttpOptions;
using sscl::core::ModLoader;
using sscl::core::ModPlatform;
using sscl::core::ModSearchQuery;
using sscl::core::ModVersion;
using sscl::core::build_modrinth_search_url;
using sscl::core::build_modrinth_update_body;
using sscl::core::build_modrinth_version_files_body;
using sscl::core::build_modrinth_versions_url;
using sscl::core::parse_modrinth_project;
using sscl::core::parse_modrinth_search;
using sscl::core::parse_modrinth_version;
using sscl::core::parse_modrinth_version_map;
using sscl::core::parse_modrinth_versions;

fs::path make_root(const char* tag) {
    return fs::temp_directory_path() /
           ("sscl_test_modplatform_" + std::string(tag) + "_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

void cleanup(const fs::path& root) {
    std::error_code ec;
    fs::remove_all(root, ec);
}

const char* kSearchJson = R"({
  "hits": [{
    "project_id": "AABBCCDD",
    "slug": "sodium",
    "title": "Sodium",
    "description": "Modern rendering engine",
    "author": "jellysquid3",
    "icon_url": "https://cdn.modrinth.com/data/AABBCCDD/icon.png",
    "downloads": 123456,
    "categories": ["optimization", "fabric"],
    "date_modified": "2026-10-01T00:00:00Z",
    "project_type": "mod"
  }],
  "offset": 0,
  "limit": 20,
  "total_hits": 1
})";

const char* kProjectJson = R"({
  "id": "AABBCCDD",
  "slug": "sodium",
  "title": "Sodium",
  "description": "Modern rendering engine",
  "icon_url": "https://cdn.modrinth.com/data/AABBCCDD/icon.png",
  "downloads": 123456,
  "categories": ["optimization"],
  "updated": "2026-10-01T00:00:00Z"
})";

const char* kVersionsJson = R"([{
  "id": "ver1",
  "project_id": "AABBCCDD",
  "name": "Sodium 0.6.13",
  "version_number": "0.6.13",
  "version_type": "release",
  "date_published": "2026-09-30T00:00:00Z",
  "game_versions": ["1.21.1"],
  "loaders": ["fabric"],
  "featured": true,
  "files": [
    {
      "hashes": {"sha1": "abc123", "sha512": "def456"},
      "url": "https://cdn.modrinth.com/data/AABBCCDD/versions/ver1/sodium.jar",
      "filename": "sodium-0.6.13.jar",
      "primary": true,
      "size": 123456,
      "file_type": null
    },
    {
      "hashes": {"sha1": "src123"},
      "url": "https://cdn.modrinth.com/data/AABBCCDD/versions/ver1/sources.jar",
      "filename": "sources.jar",
      "primary": false,
      "size": 12,
      "file_type": "sources-jar"
    }
  ],
  "dependencies": [
    {"version_id": null, "project_id": "P7dR8mSH", "file_name": null, "dependency_type": "required"},
    {"version_id": null, "project_id": null, "file_name": "external.jar", "dependency_type": "optional"}
  ]
}])";

const char* kVersionJson = R"({
  "id": "ver1",
  "project_id": "AABBCCDD",
  "name": "Sodium 0.6.13",
  "version_number": "0.6.13",
  "version_type": "beta",
  "date_published": "2026-09-30T00:00:00Z",
  "game_versions": ["1.21.1"],
  "loaders": ["fabric", "quilt"],
  "featured": false,
  "files": [{
    "hashes": {"sha1": "abc123"},
    "url": "https://cdn.modrinth.com/data/AABBCCDD/versions/ver1/sodium.jar",
    "filename": "sodium-0.6.13.jar",
    "primary": true,
    "size": 123456
  }],
  "dependencies": []
})";

}  // namespace

TEST_CASE("mod.types: 加载器/频道映射与主文件选择") {
    REQUIRE(std::string(sscl::core::mod_loader_key(ModLoader::kFabric)) == "fabric");
    REQUIRE(sscl::core::mod_loader_from_key("NeoForge") == ModLoader::kNeoForge);
    REQUIRE(sscl::core::mod_loader_from_key("") == ModLoader::kUnknown);
    REQUIRE(sscl::core::mod_loader_is_moddable(ModLoader::kQuilt));
    REQUIRE_FALSE(sscl::core::mod_loader_is_moddable(ModLoader::kVanilla));
    REQUIRE(sscl::core::mod_channel_from_key("release") == sscl::core::ModChannel::kRelease);

    ModVersion version;
    ModFile primary;
    primary.filename = "mod.jar";
    primary.primary = true;
    ModFile sources;
    sources.filename = "sources.jar";
    sources.file_type = "sources-jar";
    version.files = {sources, primary};
    const ModFile* selected = sscl::core::select_primary_file(version);
    REQUIRE(selected != nullptr);
    REQUIRE(selected->filename == "mod.jar");

    version.files = {sources};
    selected = sscl::core::select_primary_file(version);
    REQUIRE(selected != nullptr);
    REQUIRE(selected->filename == "sources.jar");  // 全是附件时退回第一个
}

TEST_CASE("mod.cache: 写入/读取/损坏回退/禁用") {
    const fs::path root = make_root("cache");
    ModCache cache(root);
    REQUIRE(cache.enabled());

    REQUIRE(cache.write(ModPlatform::kModrinth, "search:test", R"({"a":1})").is_ok());
    auto loaded = cache.read(ModPlatform::kModrinth, "search:test");
    REQUIRE(loaded.is_ok());
    REQUIRE(loaded.value().found);
    REQUIRE(loaded.value().body == R"({"a":1})");
    REQUIRE(loaded.value().saved_at > 0);

    auto missing = cache.read(ModPlatform::kModrinth, "missing");
    REQUIRE(missing.is_ok());
    REQUIRE_FALSE(missing.value().found);

    // 不同平台同 key 不混淆。
    auto other = cache.read(ModPlatform::kCurseForge, "search:test");
    REQUIRE(other.is_ok());
    REQUIRE_FALSE(other.value().found);

    ModCache disabled;
    REQUIRE_FALSE(disabled.enabled());
    REQUIRE(disabled.write(ModPlatform::kModrinth, "x", "y").is_ok());
    auto disabled_read = disabled.read(ModPlatform::kModrinth, "x");
    REQUIRE(disabled_read.is_ok());
    REQUIRE_FALSE(disabled_read.value().found);

    cleanup(root);
}

TEST_CASE("mod.api: MCIM 映射/候选顺序/错误码") {
    REQUIRE(ModHttpClient::mcim_url("https://api.modrinth.com/v2/search?x=1") ==
            "https://mod.mcimirror.top/modrinth/v2/search?x=1");
    REQUIRE(ModHttpClient::mcim_url("https://edge.forgecdn.net/files/1/2/a.jar") ==
            "https://mod.mcimirror.top/files/1/2/a.jar");
    REQUIRE(ModHttpClient::mcim_url("https://mediafilez.forgecdn.net/files/1/2/a.jar") ==
            "https://mediafilez.forgecdn.net/files/1/2/a.jar");

    ModHttpOptions options;
    options.platform = ModPlatform::kModrinth;
    options.use_mcim = true;
    auto modrinth = ModHttpClient::candidate_urls(options, "https://api.modrinth.com/v2/search");
    REQUIRE(modrinth.size() == 2);
    REQUIRE(modrinth[0].find("api.modrinth.com") != std::string::npos);
    REQUIRE(modrinth[1].find("mod.mcimirror.top/modrinth") != std::string::npos);

    options.platform = ModPlatform::kCurseForge;
    options.curseforge_api_key.clear();
    auto cf_no_key =
        ModHttpClient::candidate_urls(options, "https://api.curseforge.com/v1/mods/1");
    REQUIRE(cf_no_key.size() == 1);
    REQUIRE(cf_no_key[0].find("mod.mcimirror.top/curseforge") != std::string::npos);

    options.curseforge_api_key = "key";
    auto cf_with_key =
        ModHttpClient::candidate_urls(options, "https://api.curseforge.com/v1/mods/1");
    REQUIRE(cf_with_key.size() == 2);
    REQUIRE(cf_with_key[0].find("api.curseforge.com") != std::string::npos);

    REQUIRE(ModHttpClient::map_status_error(ModPlatform::kCurseForge, 403).code ==
            "mod.invalid_key");
    REQUIRE(ModHttpClient::map_status_error(ModPlatform::kModrinth, 403).code ==
            "mod.forbidden");
    REQUIRE(ModHttpClient::map_status_error(ModPlatform::kModrinth, 404).code ==
            "mod.not_found");
    REQUIRE(ModHttpClient::map_status_error(ModPlatform::kModrinth, 429).code ==
            "mod.rate_limited");
    REQUIRE(ModHttpClient::map_status_error(ModPlatform::kModrinth, 503).code ==
            "mod.api_invalid");
}

TEST_CASE("modrinth.url: 搜索与版本 URL 参数") {
    ModSearchQuery query;
    query.text = "sodium";
    query.game_version = "1.21.1";
    query.loader = ModLoader::kFabric;
    query.sort = sscl::core::ModSort::kDownloads;
    query.limit = 20;
    query.offset = 40;
    const std::string url = build_modrinth_search_url(query);
    REQUIRE(url.find("https://api.modrinth.com/v2/search?") == 0);
    REQUIRE(url.find("limit=20") != std::string::npos);
    REQUIRE(url.find("offset=40") != std::string::npos);
    REQUIRE(url.find("index=downloads") != std::string::npos);
    REQUIRE(url.find("query=sodium") != std::string::npos);
    REQUIRE(url.find("facets=") != std::string::npos);
    REQUIRE(url.find("project_type%3Amod") != std::string::npos);
    REQUIRE(url.find("versions%3A1.21.1") != std::string::npos);
    REQUIRE(url.find("categories%3Afabric") != std::string::npos);

    const std::string versions =
        build_modrinth_versions_url("AABBCCDD", "1.21.1", ModLoader::kFabric, false);
    REQUIRE(versions.find("/project/AABBCCDD/version?") != std::string::npos);
    REQUIRE(versions.find("include_changelog=false") != std::string::npos);
    REQUIRE(versions.find("game_versions=%5B%221.21.1%22%5D") != std::string::npos);
    REQUIRE(versions.find("loaders=%5B%22fabric%22%5D") != std::string::npos);

    const std::string all =
        build_modrinth_versions_url("AABBCCDD", "1.21.1", ModLoader::kFabric, true);
    REQUIRE(all.find("game_versions=") == std::string::npos);
    REQUIRE(all.find("loaders=") == std::string::npos);
}

TEST_CASE("modrinth.search: 解析搜索结果") {
    auto page = parse_modrinth_search(kSearchJson);
    REQUIRE(page.is_ok());
    REQUIRE(page.value().items.size() == 1);
    REQUIRE(page.value().total == 1);
    REQUIRE(page.value().offset == 0);
    REQUIRE(page.value().limit == 20);
    REQUIRE_FALSE(page.value().has_more);
    const auto& project = page.value().items[0];
    REQUIRE(project.id == "AABBCCDD");
    REQUIRE(project.slug == "sodium");
    REQUIRE(project.title == "Sodium");
    REQUIRE(project.author == "jellysquid3");
    REQUIRE(project.downloads == 123456);
    REQUIRE(project.page_url == "https://modrinth.com/mod/sodium");
    REQUIRE(project.categories.size() == 2);
    REQUIRE(project.distribution_allowed);
}

TEST_CASE("modrinth.project: 解析项目详情") {
    auto project = parse_modrinth_project(kProjectJson);
    REQUIRE(project.is_ok());
    REQUIRE(project.value().id == "AABBCCDD");
    REQUIRE(project.value().title == "Sodium");
    REQUIRE(project.value().page_url == "https://modrinth.com/mod/sodium");
    REQUIRE(project.value().downloads == 123456);
}

TEST_CASE("modrinth.versions: 解析版本与依赖") {
    auto versions = parse_modrinth_versions(kVersionsJson);
    REQUIRE(versions.is_ok());
    REQUIRE(versions.value().size() == 1);
    const ModVersion& version = versions.value()[0];
    REQUIRE(version.platform == ModPlatform::kModrinth);
    REQUIRE(version.project_id == "AABBCCDD");
    REQUIRE(version.version_id == "ver1");
    REQUIRE(version.channel == sscl::core::ModChannel::kRelease);
    REQUIRE(version.featured);
    REQUIRE(version.game_versions.size() == 1);
    REQUIRE(version.game_versions[0] == "1.21.1");
    REQUIRE(version.loaders.size() == 1);
    REQUIRE(version.loaders[0] == ModLoader::kFabric);
    REQUIRE(version.files.size() == 2);
    REQUIRE(version.files[0].sha1 == "abc123");
    REQUIRE(version.files[0].sha512 == "def456");
    REQUIRE(version.files[1].file_type == "sources-jar");
    REQUIRE(version.dependencies.size() == 2);
    REQUIRE(version.dependencies[0].kind == DependencyKind::kRequired);
    REQUIRE(version.dependencies[0].project_id == "P7dR8mSH");
    REQUIRE(version.dependencies[1].kind == DependencyKind::kOptional);
    REQUIRE(version.dependencies[1].file_name == "external.jar");
}

TEST_CASE("modrinth.version: 解析单版本与 project_id 回退") {
    auto version = parse_modrinth_version(kVersionJson);
    REQUIRE(version.is_ok());
    REQUIRE(version.value().version_id == "ver1");
    REQUIRE(version.value().channel == sscl::core::ModChannel::kBeta);
    REQUIRE(version.value().loaders.size() == 2);
    REQUIRE(version.value().files.size() == 1);
}

TEST_CASE("modrinth: 更新请求体与版本映射") {
    const std::string files_body = build_modrinth_version_files_body({"abc123"});
    REQUIRE(files_body.find("\"hashes\"") != std::string::npos);
    REQUIRE(files_body.find("\"abc123\"") != std::string::npos);
    REQUIRE(files_body.find("\"sha1\"") != std::string::npos);

    const std::string update_body = build_modrinth_update_body(
        {ModUpdateQuery{"abc123", "1.21.1", {ModLoader::kFabric}}});
    REQUIRE(update_body.find("\"game_versions\"") != std::string::npos);
    REQUIRE(update_body.find("\"fabric\"") != std::string::npos);

    const auto parsed = parse_modrinth_version_map(
        R"({"abc123":{"id":"ver1","project_id":"AABBCCDD","version_number":"1.0",)"
        R"("files":[]}})");
    REQUIRE(parsed.is_ok());
    REQUIRE(parsed.value().size() == 1);
    REQUIRE(parsed.value().at("abc123").version_id == "ver1");
    REQUIRE(parsed.value().at("abc123").project_id == "AABBCCDD");
}
