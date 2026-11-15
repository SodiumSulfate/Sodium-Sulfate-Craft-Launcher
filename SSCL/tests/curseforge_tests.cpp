#include <string>

#include <doctest/doctest.h>

#include "core/modplatform/curseforge_client.hpp"

using namespace sscl::core;

namespace {

const char* kCfSearchJson = R"({
  "data": [{
    "id": 238222,
    "name": "Just Enough Items",
    "slug": "jei",
    "summary": "View items and recipes",
    "downloadCount": 1234567,
    "categories": [{"id": 423, "name": "Map and Information"}],
    "logo": {"thumbnailUrl": "https://media.forgecdn.net/logo.png", "url": "https://media.forgecdn.net/logo.png"},
    "links": {"websiteUrl": "https://www.curseforge.com/minecraft/mc-mods/jei"},
    "allowModDistribution": true,
    "dateModified": "2026-10-25T00:00:00Z",
    "authors": [{"name": "mezz"}]
  }],
  "pagination": {"index": 0, "pageSize": 50, "resultCount": 1, "totalCount": 1}
})";

const char* kCfProjectNoDistJson = R"({
  "data": {
    "id": 1,
    "name": "No Distribution",
    "slug": "nodist",
    "summary": "disabled",
    "downloadCount": 2,
    "categories": [],
    "allowModDistribution": false,
    "dateModified": "2026-10-25T00:00:00Z",
    "authors": []
  }
})";

const char* kCfFilesJson = R"({
  "data": [{
    "id": 9318929,
    "modId": 238222,
    "displayName": "30.48.0 for NeoForge",
    "fileName": "jei-1.21.1-neoforge.jar",
    "releaseType": 2,
    "fileDate": "2026-10-25T08:32:36.090Z",
    "fileLength": 4748277,
    "downloadUrl": "https://edge.forgecdn.net/files/9318/929/jei.jar",
    "gameVersions": ["Client", "NeoForge", "Server", "1.21.1"],
    "dependencies": [
      {"modId": 1689768, "relationType": 3},
      {"modId": 1700987, "relationType": 2},
      {"modId": 1700988, "relationType": 5}
    ],
    "hashes": [
      {"value": "f7520a90d2ad01360692abdc49ea1dcf8534462f", "algo": 1},
      {"value": "6a0aeb63964a39887e6c840015b362d9", "algo": 2}
    ],
    "isAvailable": true,
    "fileFingerprint": 3175987010
  }],
  "pagination": {"index": 0, "pageSize": 50, "resultCount": 1, "totalCount": 1}
})";

const char* kCfFileNoDistJson = R"({
  "data": {
    "id": 42,
    "modId": 1,
    "displayName": "blocked",
    "fileName": "blocked.jar",
    "releaseType": 1,
    "fileLength": 10,
    "downloadUrl": null,
    "gameVersions": ["Forge", "1.20.1"],
    "dependencies": [],
    "hashes": [],
    "isAvailable": true
  }
})";

}  // namespace

TEST_CASE("curseforge.url: 搜索参数") {
    ModSearchQuery query;
    query.text = "jei";
    query.game_version = "1.21.1";
    query.loader = ModLoader::kNeoForge;
    query.sort = ModSort::kDownloads;
    query.offset = 40;
    query.limit = 50;
    query.category = "423";
    const std::string url = build_curseforge_search_url(query);
    REQUIRE(url.find("https://api.curseforge.com/v1/mods/search?") == 0);
    REQUIRE(url.find("gameId=432") != std::string::npos);
    REQUIRE(url.find("classId=6") != std::string::npos);
    REQUIRE(url.find("index=40") != std::string::npos);
    REQUIRE(url.find("pageSize=50") != std::string::npos);
    REQUIRE(url.find("sortField=6") != std::string::npos);
    REQUIRE(url.find("sortOrder=desc") != std::string::npos);
    REQUIRE(url.find("searchFilter=jei") != std::string::npos);
    REQUIRE(url.find("gameVersion=1.21.1") != std::string::npos);
    REQUIRE(url.find("modLoaderType=6") != std::string::npos);
    REQUIRE(url.find("categoryId=423") != std::string::npos);
}

TEST_CASE("curseforge.url: 文件分页参数与上限") {
    const std::string url =
        build_curseforge_files_url("238222", "1.21.1", ModLoader::kFabric, 100, 999);
    REQUIRE(url.find("/v1/mods/238222/files?") != std::string::npos);
    REQUIRE(url.find("index=100") != std::string::npos);
    REQUIRE(url.find("pageSize=50") != std::string::npos);
    REQUIRE(url.find("gameVersion=1.21.1") != std::string::npos);
    REQUIRE(url.find("modLoaderType=4") != std::string::npos);
}

TEST_CASE("curseforge.tokens: 混合 gameVersions 过滤") {
    REQUIRE(is_mc_version_token("1.21.1"));
    REQUIRE(is_mc_version_token("1.20"));
    REQUIRE(is_mc_version_token("23w45a"));
    REQUIRE_FALSE(is_mc_version_token("Client"));
    REQUIRE_FALSE(is_mc_version_token("Server"));
    REQUIRE_FALSE(is_mc_version_token("NeoForge"));
    REQUIRE_FALSE(is_mc_version_token("Forge"));
    REQUIRE_FALSE(is_mc_version_token(""));
}

TEST_CASE("curseforge.search: 解析搜索结果") {
    auto page = parse_curseforge_search(kCfSearchJson);
    REQUIRE(page.is_ok());
    REQUIRE(page.value().items.size() == 1);
    REQUIRE(page.value().total == 1);
    REQUIRE(page.value().offset == 0);
    REQUIRE(page.value().limit == 50);
    REQUIRE_FALSE(page.value().has_more);
    const auto& project = page.value().items[0];
    REQUIRE(project.platform == ModPlatform::kCurseForge);
    REQUIRE(project.id == "238222");
    REQUIRE(project.slug == "jei");
    REQUIRE(project.title == "Just Enough Items");
    REQUIRE(project.author == "mezz");
    REQUIRE(project.downloads == 1234567);
    REQUIRE(project.page_url == "https://www.curseforge.com/minecraft/mc-mods/jei");
    REQUIRE(project.distribution_allowed);
    REQUIRE(project.categories.size() == 1);
}

TEST_CASE("curseforge.project: allowModDistribution=false") {
    auto project = parse_curseforge_project(kCfProjectNoDistJson);
    REQUIRE(project.is_ok());
    REQUIRE(project.value().id == "1");
    REQUIRE_FALSE(project.value().distribution_allowed);
    REQUIRE(project.value().page_url == "https://www.curseforge.com/minecraft/mc-mods/nodist");
}

TEST_CASE("curseforge.files: 版本/加载器/哈希/依赖") {
    auto versions = parse_curseforge_files(kCfFilesJson);
    REQUIRE(versions.is_ok());
    REQUIRE(versions.value().size() == 1);
    const ModVersion& version = versions.value()[0];
    REQUIRE(version.platform == ModPlatform::kCurseForge);
    REQUIRE(version.project_id == "238222");
    REQUIRE(version.version_id == "9318929");
    REQUIRE(version.channel == ModChannel::kBeta);
    REQUIRE(version.downloadable);
    REQUIRE(version.game_versions.size() == 1);
    REQUIRE(version.game_versions[0] == "1.21.1");
    REQUIRE(version.loaders.size() == 1);
    REQUIRE(version.loaders[0] == ModLoader::kNeoForge);
    REQUIRE(version.files.size() == 1);
    REQUIRE(version.files[0].filename == "jei-1.21.1-neoforge.jar");
    REQUIRE(version.files[0].sha1 == "f7520a90d2ad01360692abdc49ea1dcf8534462f");
    REQUIRE(version.files[0].size == 4748277);
    REQUIRE(version.dependencies.size() == 3);
    REQUIRE(version.dependencies[0].kind == DependencyKind::kRequired);
    REQUIRE(version.dependencies[0].project_id == "1689768");
    REQUIRE(version.dependencies[1].kind == DependencyKind::kOptional);
    REQUIRE(version.dependencies[2].kind == DependencyKind::kIncompatible);
}

TEST_CASE("curseforge.file: 禁止分发时不可下载") {
    auto version = parse_curseforge_file(kCfFileNoDistJson);
    REQUIRE(version.is_ok());
    REQUIRE(version.value().version_id == "42");
    REQUIRE_FALSE(version.value().downloadable);
    REQUIRE(version.value().game_versions.size() == 1);
    REQUIRE(version.value().game_versions[0] == "1.20.1");
}

TEST_CASE("curseforge.invalid: 缺少 data 时返回 api_invalid") {
    REQUIRE(parse_curseforge_search("{}").is_error());
    REQUIRE(parse_curseforge_project("{}").is_error());
    REQUIRE(parse_curseforge_files("{}").is_error());
    REQUIRE(parse_curseforge_file("{}").is_error());
}

TEST_CASE("curseforge.update: latestFilesIndexes / 指纹 / 批量 body") {
    const char* project_json = R"({
      "data": [{
        "id": 238222, "name": "JEI", "slug": "jei", "summary": "x",
        "downloadCount": 1, "categories": [],
        "allowModDistribution": true,
        "latestFilesIndexes": [
          {"gameVersion": "1.21.1", "modLoader": 6, "fileId": 42},
          {"gameVersion": "1.20.1", "modLoader": 1, "fileId": 41}
        ]
      }]
    })";
    auto projects = parse_curseforge_projects(project_json);
    REQUIRE(projects.is_ok());
    REQUIRE(projects.value().size() == 1);
    REQUIRE(projects.value()[0].latest_files.size() == 2);
    REQUIRE(projects.value()[0].latest_files[0].game_version == "1.21.1");
    REQUIRE(projects.value()[0].latest_files[0].loader == ModLoader::kNeoForge);
    REQUIRE(projects.value()[0].latest_files[0].file_id == "42");

    const char* fingerprint_json = R"({
      "data": {"exactMatches": [{
        "id": 238222,
        "file": {
          "id": 42, "modId": 238222, "fileName": "jei.jar",
          "displayName": "JEI", "releaseType": 1, "fileDate": "2026-10-01T00:00:00Z",
          "fileLength": 10, "downloadUrl": "https://edge.forgecdn.net/a.jar",
          "gameVersions": ["1.21.1", "NeoForge"], "hashes": [],
          "fileFingerprint": 3175987010
        }
      }]}
    })";
    auto matches = parse_curseforge_fingerprints(fingerprint_json);
    REQUIRE(matches.is_ok());
    REQUIRE(matches.value().size() == 1);
    REQUIRE(matches.value()[0].project_id == "238222");
    REQUIRE(matches.value()[0].fingerprint == "3175987010");
    REQUIRE(matches.value()[0].file.version_id == "42");

    const std::string body = build_curseforge_fingerprints_body({1, 3175987010u});
    REQUIRE(body == R"({"fingerprints":[1,3175987010]})");
    const std::string ids = build_curseforge_ids_body("fileIds", {"42", "43"});
    REQUIRE(ids == R"({"fileIds":[42,43]})");
}
