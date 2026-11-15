#include <doctest/doctest.h>

#include <miniz/miniz.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "core/game/file_plan.hpp"
#include "core/launch/version_json.hpp"
#include "core/loader/installer_profile.hpp"
#include "core/loader/loader_catalog.hpp"
#include "core/loader/loader_installer.hpp"
#include "core/loader/maven_metadata.hpp"
#include "core/loader/processor_runner.hpp"
#include "platform/zip_utils.hpp"

#pragma comment(lib, "miniz.lib")

namespace {

using sscl::core::ForgePromotions;
using sscl::core::LoaderEntry;
using sscl::core::MavenMetadata;

std::filesystem::path loader_temp_root(const char* name) {
    return std::filesystem::temp_directory_path() / (std::string("sscl-loader-tests-") + name);
}

// 用 miniz 写一个内存 ZIP（安装器 fixture 的最小替身）。
std::string make_zip_bytes(const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive writer{};
    REQUIRE(mz_zip_writer_init_heap(&writer, 0, 0) != 0);
    for (const auto& [name, data] : entries) {
        REQUIRE(mz_zip_writer_add_mem(&writer, name.c_str(), data.data(), data.size(),
                                      MZ_DEFAULT_COMPRESSION) != 0);
    }
    void* buffer = nullptr;
    size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&writer, &buffer, &size) != 0);
    std::string bytes(static_cast<const char*>(buffer), size);
    mz_free(buffer);
    mz_zip_writer_end(&writer);
    return bytes;
}

}  // namespace

TEST_CASE("version json：libraries[].url、本地生成库、processors、data、client_mappings") {
    const std::string text = R"({
        "id": "1.20.1-forge-47.2.0",
        "inheritsFrom": "1.20.1",
        "mainClass": "cpw.mods.bootstraplauncher.BootstrapLauncher",
        "downloads": {
            "client": { "url": "https://example.test/client.jar", "sha1": "aa", "size": 10 },
            "client_mappings": { "url": "https://example.test/mappings.txt", "sha1": "bb", "size": 20 }
        },
        "libraries": [
            { "name": "net.fabricmc:intermediary:1.21.1", "url": "https://maven.fabricmc.net/" },
            { "name": "net.minecraftforge:forge:1.20.1-47.2.0",
              "downloads": { "artifact": { "path": "net/minecraftforge/forge/1.20.1-47.2.0/forge-1.20.1-47.2.0-client.jar", "url": "", "sha1": "", "size": 0 } } },
            { "name": "org.ow2.asm:asm:9.7.1",
              "downloads": { "artifact": { "path": "org/ow2/asm/asm/9.7.1/asm-9.7.1.jar", "url": "https://libraries.minecraft.net/org/ow2/asm/asm/9.7.1/asm-9.7.1.jar", "sha1": "cc", "size": 30 } } }
        ],
        "processors": [
            { "sides": ["client"], "jar": "net.minecraftforge:installertools:1.3.2",
              "classpath": ["net.minecraftforge:installertools:1.3.2"],
              "args": ["--task", "DOWNLOAD_MOJMAPS", "--output", "{MOJMAPS}"],
              "outputs": { "{MOJMAPS}": "{MOJMAPSHA1}" } }
        ],
        "data": {
            "MOJMAPS": { "client": "[net.minecraft:client:1.20.1-20230612.114412:mappings@txt]", "server": "" },
            "BINPATCH": { "client": "/data/client.lzma", "server": "/data/server.lzma" }
        }
    })";

    auto parsed = sscl::core::parse_version_json(text);
    REQUIRE(parsed.is_ok());
    const auto& version = parsed.value();

    REQUIRE(version.libraries.size() == 3);
    // 只有 name+url：保留 Maven 基址，且不是本地生成库
    CHECK(version.libraries[0].url == "https://maven.fabricmc.net/");
    CHECK_FALSE(version.libraries[0].local_only);
    CHECK_FALSE(version.libraries[0].has_artifact);
    // artifact.url 为空 ⇒ 本地生成（processor 产物）
    CHECK(version.libraries[1].has_artifact);
    CHECK(version.libraries[1].local_only);
    // 有完整 url ⇒ 正常下载
    CHECK_FALSE(version.libraries[2].local_only);

    // client_mappings
    REQUIRE(version.downloads.has_client_mappings);
    CHECK(version.downloads.client_mappings.url == "https://example.test/mappings.txt");
    CHECK(version.downloads.client_mappings.sha1 == "bb");

    // processors / data
    REQUIRE(version.has_processors);
    REQUIRE(version.processors.size() == 1);
    CHECK(version.processors[0].sides == std::vector<std::string>{"client"});
    CHECK(version.processors[0].jar == "net.minecraftforge:installertools:1.3.2");
    CHECK(version.processors[0].outputs.at("{MOJMAPS}") == "{MOJMAPSHA1}");
    REQUIRE(version.has_installer_data);
    CHECK(version.installer_data.at("MOJMAPS").client ==
          "[net.minecraft:client:1.20.1-20230612.114412:mappings@txt]");
    CHECK(version.installer_data.at("BINPATCH").client == "/data/client.lzma");
}

TEST_CASE("library_relative_path：classifier 与 @ext") {
    CHECK(sscl::core::library_relative_path("org.ow2.asm:asm:9.7.1") ==
          "org/ow2/asm/asm/9.7.1/asm-9.7.1.jar");
    CHECK(sscl::core::library_relative_path("org.lwjgl:lwjgl:3.3.3:natives-windows") ==
          "org/lwjgl/lwjgl/3.3.3/lwjgl-3.3.3-natives-windows.jar");
    CHECK(sscl::core::library_relative_path(
              "net.minecraft:client:1.20.1-20230612.114412:mappings@txt") ==
          "net/minecraft/client/1.20.1-20230612.114412/client-1.20.1-20230612.114412-mappings.txt");
    CHECK(sscl::core::library_relative_path("bad-coordinate").empty());
}

TEST_CASE("file plan：库下载 URL 优先级与本地生成库跳过") {
    sscl::core::Library base_only;
    base_only.name = "net.fabricmc:fabric-loader:0.16.9";
    base_only.url = "https://maven.fabricmc.net/";
    CHECK(sscl::core::library_download_url(base_only) ==
          "https://maven.fabricmc.net/net/fabricmc/fabric-loader/0.16.9/fabric-loader-0.16.9.jar");

    sscl::core::Library base_no_slash;
    base_no_slash.name = "net.fabricmc:fabric-loader:0.16.9";
    base_no_slash.url = "https://maven.fabricmc.net";
    CHECK(sscl::core::library_download_url(base_no_slash) ==
          "https://maven.fabricmc.net/net/fabricmc/fabric-loader/0.16.9/fabric-loader-0.16.9.jar");

    sscl::core::Library local_generated;
    local_generated.name = "net.minecraftforge:forge:1.20.1-47.2.0";
    local_generated.has_artifact = true;
    local_generated.artifact.path =
        "net/minecraftforge/forge/1.20.1-47.2.0/forge-1.20.1-47.2.0-client.jar";
    local_generated.local_only = true;
    CHECK(sscl::core::library_download_url(local_generated).empty());
    CHECK(sscl::core::library_relative_file_path(local_generated) ==
          local_generated.artifact.path);

    sscl::core::Library plain;
    plain.name = "com.google.guava:guava:31.1-jre";
    CHECK(sscl::core::library_download_url(plain) ==
          "https://libraries.minecraft.net/com/google/guava/guava/31.1-jre/guava-31.1-jre.jar");

    // plan_version_files 不为本地生成库产生下载请求
    sscl::core::InstallLayout layout;
    layout.game_dir = loader_temp_root("plan");
    layout.versions_dir = layout.game_dir / "versions";
    layout.libraries_dir = layout.game_dir / "libraries";
    layout.assets_dir = layout.game_dir / "assets";
    sscl::core::VersionJson version;
    version.id = "forged";
    version.jar = "1.20.1";
    version.libraries.push_back(local_generated);
    const auto requests = sscl::core::plan_version_files(version, layout, sscl::core::OsInfo{},
                                                         {});
    for (const auto& request : requests) {
        CHECK(request.destination != layout.libraries_dir / local_generated.artifact.path);
    }
}

TEST_CASE("maven metadata：提取 latest/release/versions") {
    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<metadata>
  <groupId>net.minecraftforge</groupId>
  <artifactId>forge</artifactId>
  <versioning>
    <latest>1.21.1-52.0.1</latest>
    <release>1.21.1-52.0.1</release>
    <versions>
      <version>1.21.1-52.0.1</version>
      <version>1.20.1-47.2.0</version>
      <version> 1.20.1-47.1.0 </version>
    </versions>
  </versioning>
</metadata>)";
    auto parsed = sscl::core::parse_maven_metadata(xml);
    REQUIRE(parsed.is_ok());
    CHECK(parsed.value().latest == "1.21.1-52.0.1");
    CHECK(parsed.value().release == "1.21.1-52.0.1");
    REQUIRE(parsed.value().versions.size() == 3);
    CHECK(parsed.value().versions[1] == "1.20.1-47.2.0");
    CHECK(parsed.value().versions[2] == "1.20.1-47.1.0");

    CHECK(sscl::core::parse_maven_metadata("<html></html>").is_error());
    CHECK(sscl::core::parse_maven_metadata("<metadata><versioning></versioning></metadata>")
              .is_error());
}

TEST_CASE("forge 版本号切分：mc-loader / 尾部重复 mc / 分支") {
    auto plain = sscl::core::parse_forge_version_id("1.20.1-47.2.0");
    REQUIRE(plain.is_ok());
    CHECK(plain.value().minecraft == "1.20.1");
    CHECK(plain.value().loader == "47.2.0");
    CHECK(plain.value().branch.empty());

    auto duplicated = sscl::core::parse_forge_version_id("1.7.10-10.13.4.1614-1.7.10");
    REQUIRE(duplicated.is_ok());
    CHECK(duplicated.value().minecraft == "1.7.10");
    CHECK(duplicated.value().loader == "10.13.4.1614");

    auto branched = sscl::core::parse_forge_version_id("1.7.10-10.13.4.1614-prerelease");
    REQUIRE(branched.is_ok());
    CHECK(branched.value().loader == "10.13.4.1614");
    CHECK(branched.value().branch == "prerelease");

    CHECK(sscl::core::parse_forge_version_id("47.2.0").is_error());
    CHECK(sscl::core::parse_forge_version_id("").is_error());
}

TEST_CASE("forge 清单：按 MC 过滤、recommended 置顶、installer URL") {
    MavenMetadata metadata;
    metadata.versions = {"1.20.1-47.1.0", "1.20.1-47.2.0", "1.21.1-52.0.1"};
    ForgePromotions promotions;
    promotions.promos["1.20.1-recommended"] = "47.2.0";
    promotions.promos["1.20.1-latest"] = "47.2.0";

    auto parsed = sscl::core::parse_forge_versions(metadata, promotions, "1.20.1");
    REQUIRE(parsed.is_ok());
    REQUIRE(parsed.value().size() == 2);
    CHECK(parsed.value()[0].version == "47.2.0");
    CHECK(parsed.value()[0].recommended);
    CHECK(parsed.value()[0].stable);
    CHECK(parsed.value()[0].full_version == "1.20.1-47.2.0");
    CHECK(parsed.value()[0].download_url ==
          "https://bmclapi2.bangbang93.com/maven/net/minecraftforge/forge/1.20.1-47.2.0/"
          "forge-1.20.1-47.2.0-installer.jar");
    CHECK_FALSE(parsed.value()[1].recommended);

    auto missing = sscl::core::parse_forge_versions(metadata, promotions, "1.19.2");
    CHECK(missing.is_error());
}

TEST_CASE("fabric 清单：版本/stability/profile URL") {
    const std::string text = R"([
        {"separator":".","build":26,"maven":"net.fabricmc:fabric-loader:0.16.9","version":"0.16.9","stable":true},
        {"separator":".","build":27,"maven":"net.fabricmc:fabric-loader:0.17.0-beta.1","version":"0.17.0-beta.1","stable":false}
    ])";
    auto parsed = sscl::core::parse_fabric_loader_versions(text, "1.21.1");
    REQUIRE(parsed.is_ok());
    REQUIRE(parsed.value().size() == 2);
    CHECK(parsed.value()[0].version == "0.17.0-beta.1");  // 自然序新→旧
    CHECK_FALSE(parsed.value()[0].stable);
    CHECK(parsed.value()[1].version == "0.16.9");
    CHECK(parsed.value()[1].stable);
    CHECK(parsed.value()[1].download_url ==
          "https://meta.fabricmc.net/v2/versions/loader/1.21.1/0.16.9/profile/json");

    CHECK(sscl::core::parse_fabric_loader_versions("{}", "1.21.1").is_error());
}

TEST_CASE("neoforge 版本号 → MC 版本（四形态）") {
    CHECK(sscl::core::neoforge_minecraft_version("1.20.1-47.1.106") == "1.20.1");
    CHECK(sscl::core::neoforge_minecraft_version("20.4.30-beta") == "1.20.4");
    CHECK(sscl::core::neoforge_minecraft_version("20.4.0") == "1.20.4");
    CHECK(sscl::core::neoforge_minecraft_version("0.25w14craftmine.3-beta") == "25w14craftmine");
    CHECK(sscl::core::neoforge_minecraft_version("26.1.0.0-alpha.1+snapshot-1") ==
          "26.1-snapshot-1");
    CHECK(sscl::core::neoforge_minecraft_version("").empty());
    CHECK(sscl::core::neoforge_loader_display("1.20.1-47.1.106") == "47.1.106");
    CHECK(sscl::core::neoforge_loader_display("21.1.0") == "21.1.0");
}

TEST_CASE("neoforge 清单：现行 + legacy 合并、按 MC 过滤、URL 前缀") {
    const std::string current = R"({"isSnapshot":false,"versions":["21.1.0","21.1.0-beta","20.4.30"]})";
    const std::string legacy = R"({"isSnapshot":false,"versions":["1.20.1-47.1.106"]})";

    auto neoforge = sscl::core::parse_neoforge_versions(current, legacy, "1.21.1");
    REQUIRE(neoforge.is_ok());
    REQUIRE(neoforge.value().size() == 2);
    bool found_stable = false;
    bool found_beta = false;
    for (const LoaderEntry& entry : neoforge.value()) {
        if (entry.version == "21.1.0") {
            found_stable = true;
            CHECK(entry.stable);
            CHECK(entry.download_url ==
                  "https://bmclapi2.bangbang93.com/maven/net/neoforged/neoforge/21.1.0/"
                  "neoforge-21.1.0-installer.jar");
        }
        if (entry.version == "21.1.0-beta") {
            found_beta = true;
            CHECK_FALSE(entry.stable);
        }
    }
    CHECK(found_stable);
    CHECK(found_beta);

    auto legacy_only = sscl::core::parse_neoforge_versions("", legacy, "1.20.1");
    REQUIRE(legacy_only.is_ok());
    REQUIRE(legacy_only.value().size() == 1);
    CHECK(legacy_only.value()[0].version == "47.1.106");
    CHECK(legacy_only.value()[0].full_version == "1.20.1-47.1.106");
    CHECK(legacy_only.value()[0].download_url ==
          "https://bmclapi2.bangbang93.com/maven/net/neoforged/forge/1.20.1-47.1.106/"
          "forge-1.20.1-47.1.106-installer.jar");

    auto none = sscl::core::parse_neoforge_versions(current, legacy, "1.19.2");
    CHECK(none.is_error());
}

TEST_CASE("zip utils：条目/读取/子树解包与路径安全") {
    const std::string bytes = make_zip_bytes({
        {"install_profile.json", "{\"spec\":1}"},
        {"version.json", "{\"id\":\"x\"}"},
        {"maven/net/minecraftforge/forge/1.20.1-47.2.0/forge.jar", "JAR"},
        {"../evil.txt", "bad"},
    });

    auto names = sscl::platform::zip_entry_names_from_memory(bytes);
    REQUIRE(names.is_ok());
    CHECK(names.value().size() == 4);

    auto profile = sscl::platform::zip_read_entry_from_memory(bytes, "install_profile.json");
    REQUIRE(profile.is_ok());
    CHECK(profile.value() == "{\"spec\":1}");
    CHECK(sscl::platform::zip_read_entry_from_memory(bytes, "missing.json").is_error());

    SUBCASE("子树解包只写前缀内文件") {
        const std::filesystem::path root = loader_temp_root("zip-ok");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        auto written = sscl::platform::zip_extract_subtree_from_memory(bytes, "maven/", root);
        REQUIRE(written.is_ok());
        CHECK(written.value() == 1);
        CHECK(std::filesystem::is_regular_file(
            root / "net/minecraftforge/forge/1.20.1-47.2.0/forge.jar", ec));
        CHECK_FALSE(std::filesystem::exists(root / "install_profile.json", ec));
        std::filesystem::remove_all(root, ec);
    }

    SUBCASE("不安全路径被拒绝且不落盘") {
        const std::filesystem::path root = loader_temp_root("zip-evil");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        auto written = sscl::platform::zip_extract_subtree_from_memory(bytes, "", root);
        REQUIRE(written.is_error());
        CHECK(written.error().code == "zip.unsafe_path");
        CHECK_FALSE(std::filesystem::exists(root.parent_path() / "evil.txt", ec));
        std::filesystem::remove_all(root, ec);
    }
}

TEST_CASE("安装器 install_profile 三分支解析") {
    using sscl::core::InstallerFlavor;

    const std::string modern = R"({
        "spec": 1, "profile": "forge", "version": "1.20.1-47.2.0", "minecraft": "1.20.1",
        "json": "/version.json",
        "data": {
            "MOJMAPS": { "client": "[net.minecraft:client:1.20.1:mappings@txt]",
                          "server": "[net.minecraft:server:1.20.1:mappings@txt]" },
            "SIDE_LITERAL": { "client": "'literal'", "server": "'literal'" }
        },
        "processors": [ { "sides": ["client"], "jar": "net.minecraftforge:installertools:1.3.2",
            "classpath": ["net.minecraftforge:installertools:1.3.2"],
            "args": ["--task", "DOWNLOAD_MOJMAPS", "--output", "{MOJMAPS}"],
            "outputs": { "{MOJMAPS}": "{MOJMAPSHA1}" } } ],
        "libraries": [ { "name": "net.minecraftforge:forge:1.20.1-47.2.0:universal",
            "downloads": { "artifact": { "path": "net/minecraftforge/forge/x.jar",
                                         "url": "", "sha1": "", "size": 0 } } } ]
    })";
    auto parsed = sscl::core::parse_installer_profile(modern);
    REQUIRE(parsed.is_ok());
    CHECK(parsed.value().flavor == InstallerFlavor::kModern);
    CHECK(parsed.value().spec == 1);
    CHECK(parsed.value().minecraft == "1.20.1");
    CHECK(parsed.value().version == "1.20.1-47.2.0");
    CHECK(parsed.value().json_entry == "version.json");  // 去掉前导 '/'
    CHECK(parsed.value().data.at("MOJMAPS").client ==
          "[net.minecraft:client:1.20.1:mappings@txt]");
    CHECK(parsed.value().data.at("SIDE_LITERAL").server == "'literal'");
    REQUIRE(parsed.value().processors.size() == 1);
    CHECK(parsed.value().processors[0].jar == "net.minecraftforge:installertools:1.3.2");
    CHECK(parsed.value().processors[0].args.size() == 4);
    CHECK(parsed.value().processors[0].args[0] == "--task");
    CHECK(parsed.value().processors[0].args[1] == "DOWNLOAD_MOJMAPS");
    CHECK(parsed.value().processors[0].outputs.at("{MOJMAPS}") == "{MOJMAPSHA1}");
    REQUIRE(parsed.value().libraries.size() == 1);
    CHECK(parsed.value().libraries[0].local_only);  // url 为空 ⇒ 处理器产物，禁止下载

    const std::string legacy = R"({
        "install": { "path": "net.minecraftforge:forge:1.12.2-14.23.5.2860",
                     "filePath": "forge-1.12.2-14.23.5.2860-universal.jar",
                     "minecraft": "1.12.2" },
        "versionInfo": { "id": "1.12.2-forge1.12.2-14.23.5.2860",
                         "mainClass": "net.minecraft.launchwrapper.Launch",
                         "libraries": [] }
    })";
    auto legacy_parsed = sscl::core::parse_installer_profile(legacy);
    REQUIRE(legacy_parsed.is_ok());
    CHECK(legacy_parsed.value().flavor == InstallerFlavor::kLegacy);
    CHECK(legacy_parsed.value().minecraft == "1.12.2");
    CHECK(legacy_parsed.value().legacy_install_path == "net.minecraftforge:forge:1.12.2-14.23.5.2860");
    CHECK(legacy_parsed.value().legacy_file_path == "forge-1.12.2-14.23.5.2860-universal.jar");
    CHECK(legacy_parsed.value().embedded_version_json.find("net.minecraft.launchwrapper.Launch") !=
          std::string::npos);

    auto maven_only = sscl::core::parse_installer_profile(
        R"({"json":"/version.json","minecraft":"1.7.10","version":"1.7.10-10.13.4.1614"})");
    REQUIRE(maven_only.is_ok());
    CHECK(maven_only.value().flavor == InstallerFlavor::kMavenOnly);
    CHECK(maven_only.value().json_entry == "version.json");

    auto unsupported = sscl::core::parse_installer_profile(R"({"foo":1})");
    REQUIRE(unsupported.is_error());
    CHECK(unsupported.error().code == "loader.installer_unsupported");

    auto invalid = sscl::core::parse_installer_profile("not-json");
    REQUIRE(invalid.is_error());
    CHECK(invalid.error().code == "loader.installer_invalid");
}

TEST_CASE("处理器字面量与安装器 data 解析") {
    using sscl::core::ProcessorEnvironment;

    const std::filesystem::path root = loader_temp_root("processor-literals");
    ProcessorEnvironment env;
    env.libraries_dir = root / "libraries";
    env.game_dir = root;
    env.variables["MOJMAPS"] = "[net.minecraft:client:1.20.1:mappings@txt]";
    env.variables["ROOT"] = root.string();
    env.variables["SIDED"] = "'literal'";

    auto mapping = sscl::core::resolve_processor_text("{MOJMAPS}", env);
    REQUIRE(mapping.is_ok());
    CHECK(std::filesystem::path(mapping.value()) ==
          env.libraries_dir /
              sscl::core::library_relative_path("net.minecraft:client:1.20.1:mappings@txt"));

    auto literal = sscl::core::resolve_processor_text("{SIDED}", env);
    REQUIRE(literal.is_ok());
    CHECK(literal.value() == "literal");

    auto joined = sscl::core::resolve_processor_text("{ROOT}/libraries", env);
    REQUIRE(joined.is_ok());
    CHECK(joined.value() == root.string() + "/libraries");

    auto plain = sscl::core::resolve_processor_text("--task", env);
    REQUIRE(plain.is_ok());
    CHECK(plain.value() == "--task");

    CHECK(sscl::core::resolve_processor_text("{MISSING}", env).is_error());
    CHECK(sscl::core::resolve_processor_text("{ROOT}/{MISSING}", env).is_error());

    // data 取值：'literal' / [coords] / 普通串
    auto data_literal = sscl::core::resolve_installer_data_value("'x y'", {}, env.libraries_dir,
                                                                 root / "tmp");
    REQUIRE(data_literal.is_ok());
    CHECK(data_literal.value() == "x y");
    auto data_coords = sscl::core::resolve_installer_data_value(
        "[net.minecraft:client:1.20.1:mappings@txt]", {}, env.libraries_dir, root / "tmp");
    REQUIRE(data_coords.is_ok());
    CHECK(std::filesystem::path(data_coords.value()) ==
          env.libraries_dir /
              sscl::core::library_relative_path("net.minecraft:client:1.20.1:mappings@txt"));
    auto data_plain =
        sscl::core::resolve_installer_data_value("plain", {}, env.libraries_dir, root / "tmp");
    REQUIRE(data_plain.is_ok());
    CHECK(data_plain.value() == "plain");

    // 处理器 jar 缺失 → classpath 错误
    sscl::core::InstallerProcessor processor;
    processor.jar = "net.minecraftforge:installertools:1.3.2";
    auto command = sscl::core::build_processor_command(processor, env);
    REQUIRE(command.is_error());
    CHECK(command.error().code == "loader.processor_classpath_missing");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("加载器版本 id 命名与 Fabric profile 补丁") {
    using sscl::core::LoaderEntry;
    using sscl::core::LoaderKind;

    LoaderEntry fabric;
    fabric.version = "0.16.9";
    fabric.download_url = "https://meta.fabricmc.net/v2/versions/loader/1.21.1/0.16.9/profile/json";
    CHECK(sscl::core::make_loader_version_id(LoaderKind::kFabric, "1.21.1", fabric) ==
          "fabric-loader-0.16.9-1.21.1");

    LoaderEntry forge;
    forge.version = "47.2.0";
    forge.full_version = "1.20.1-47.2.0";
    CHECK(sscl::core::make_loader_version_id(LoaderKind::kForge, "1.20.1", forge) ==
          "forge-1.20.1-47.2.0");

    LoaderEntry neoforge;
    neoforge.version = "21.1.0";
    neoforge.full_version = "21.1.0";
    CHECK(sscl::core::make_loader_version_id(LoaderKind::kNeoForge, "1.21.1", neoforge) ==
          "neoforge-21.1.0");

    LoaderEntry neoforge_legacy;
    neoforge_legacy.version = "47.1.106";
    neoforge_legacy.full_version = "1.20.1-47.1.106";
    CHECK(sscl::core::make_loader_version_id(LoaderKind::kNeoForge, "1.20.1", neoforge_legacy) ==
          "neoforge-1.20.1-47.1.106");

    const std::string profile = R"({
        "id": "fabric-loader-0.16.9-1.21.1",
        "mainClass": "net.fabricmc.loader.impl.launch.knot.KnotClient",
        "libraries": [ { "name": "net.fabricmc:fabric-loader:0.16.9", "url": "https://maven.fabricmc.net/" } ]
    })";
    auto patched = sscl::core::patch_loader_profile(
        profile, LoaderKind::kFabric, "fabric-loader-0.16.9-1.21.1", "1.21.1", fabric);
    REQUIRE(patched.is_ok());
    const nlohmann::json root = nlohmann::json::parse(patched.value());
    CHECK(root.at("id").get<std::string>() == "fabric-loader-0.16.9-1.21.1");
    CHECK(root.at("inheritsFrom").get<std::string>() == "1.21.1");
    CHECK(root.at("sscl").at("loader").get<std::string>() == "fabric");
    CHECK(root.at("sscl").at("loaderVersion").get<std::string>() == "0.16.9");
    CHECK(root.at("sscl").at("minecraftVersion").get<std::string>() == "1.21.1");
    CHECK(root.at("sscl").at("installerUrl").get<std::string>() == fabric.download_url);

    // 已有 inheritsFrom 时保留（不覆盖）
    const std::string with_parent =
        R"({"mainClass":"a.B","inheritsFrom":"1.20.1","libraries":[]})";
    auto kept = sscl::core::patch_loader_profile(with_parent, LoaderKind::kFabric,
                                                 "fabric-loader-0.16.9-1.21.1", "1.21.1", fabric);
    REQUIRE(kept.is_ok());
    CHECK(nlohmann::json::parse(kept.value()).at("inheritsFrom").get<std::string>() == "1.20.1");

    // 缺少 mainClass / libraries ⇒ 拒绝
    CHECK(sscl::core::patch_loader_profile(R"({"id":"x","libraries":[]})", LoaderKind::kFabric,
                                           "x", "1.21.1", fabric)
              .is_error());
    CHECK(sscl::core::patch_loader_profile(R"({"mainClass":"a.B"})", LoaderKind::kFabric, "x",
                                           "1.21.1", fabric)
              .is_error());
    CHECK(sscl::core::patch_loader_profile("not-json", LoaderKind::kFabric, "x", "1.21.1", fabric)
              .is_error());
}

TEST_CASE("镜像清单：NeoForge 明细/maven 列表 + Forge BMCL 列表") {
    // BMCLAPI NeoForge 明细：{"files":[{"name","type"}]}（PCL ModDownload.vb:926）；只看 DIRECTORY
    const std::string details = R"({"name":"neoforge","files":[
        {"name":"21.1.0","type":"DIRECTORY"},
        {"name":"21.1.0-beta","type":"DIRECTORY"},
        {"name":"1.20.1-47.1.106","type":"DIRECTORY"},
        {"name":"maven-metadata.xml","type":"FILE"}
    ]})";
    auto current = sscl::core::parse_neoforge_detail_versions(details, false, "1.21.1");
    REQUIRE(current.is_ok());
    REQUIRE(current.value().size() == 2);
    for (const auto& entry : current.value()) {
        CHECK(entry.minecraft_version == "1.21.1");
        CHECK(entry.download_url.find("bmclapi") != std::string::npos);
        if (entry.version == "21.1.0-beta") {
            CHECK_FALSE(entry.stable);
        }
        if (entry.version == "21.1.0") {
            CHECK(entry.stable);
            CHECK(entry.download_url.find("neoforge/21.1.0/neoforge-21.1.0-installer.jar") !=
                  std::string::npos);
        }
    }

    auto legacy = sscl::core::parse_neoforge_detail_versions(details, true, "1.20.1");
    REQUIRE(legacy.is_ok());
    REQUIRE(legacy.value().size() == 1);
    CHECK(legacy.value()[0].version == "47.1.106");
    CHECK(legacy.value()[0].full_version == "1.20.1-47.1.106");
    CHECK(legacy.value()[0].download_url.find("forge/1.20.1-47.1.106/forge-1.20.1-47.1.106-installer.jar") !=
          std::string::npos);

    // maven-metadata 镜像兜底：任意版本号集合按 MC 过滤 + 四形态推导
    auto from_maven = sscl::core::parse_neoforge_version_list(
        {"21.1.0", "21.1.1", "1.20.1-47.1.106", "26.1.0.0-alpha.1+snapshot-1"}, false, "1.21.1");
    REQUIRE(from_maven.is_ok());
    CHECK(from_maven.value().size() == 2);

    // BMCL Forge 列表：数组项含 mcversion/version/files（无 branch 也可）
    const std::string forge_list = R"([{
        "mcversion":"1.20.1","version":"47.4.0",
        "files":[{"format":"jar","category":"installer","hash":"x"}]
    }])";
    auto forge = sscl::core::parse_forge_bmclapi_versions(forge_list, "1.20.1",
                                                          sscl::core::ForgePromotions{});
    REQUIRE(forge.is_ok());
    REQUIRE(forge.value().size() == 1);
    CHECK(forge.value()[0].version == "47.4.0");
    CHECK(forge.value()[0].full_version == "1.20.1-47.4.0");
    CHECK(forge.value()[0].download_url.find("bmclapi2.bangbang93.com/forge/download") !=
          std::string::npos);
}
