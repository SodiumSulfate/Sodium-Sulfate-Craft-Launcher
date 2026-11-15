#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "core/game/game_service.hpp"
#include "core/game/instance_launch.hpp"
#include "core/game/instance_paths.hpp"
#include "core/game/instance_registry.hpp"
#include "core/game/launcher_config.hpp"
#include "core/launch/version_json.hpp"
#include "platform/file_utils.hpp"

namespace {

namespace fs = std::filesystem;
using sscl::core::InstallLayout;
using sscl::core::InstanceTarget;
using sscl::core::IsolationMode;
using sscl::core::VersionJson;

fs::path make_root(const char* tag) {
    return fs::temp_directory_path() /
           ("sscl_test_instance_" + std::string(tag) + "_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

InstallLayout make_layout(const fs::path& root) {
    InstallLayout layout;
    layout.game_dir = root / L"minecraft";
    layout.versions_dir = layout.game_dir / L"versions";
    layout.libraries_dir = layout.game_dir / L"libraries";
    layout.assets_dir = layout.game_dir / L"assets";
    return layout;
}

VersionJson parse_ok(const std::string& text) {
    auto parsed = sscl::core::parse_version_json(text);
    REQUIRE(parsed.is_ok());
    return parsed.value();
}

void cleanup(const fs::path& root) {
    std::error_code ec;
    fs::remove_all(root, ec);
}

}  // namespace

TEST_CASE("instance.sscl: 版本 JSON 元数据解析与合并") {
    const VersionJson parent =
        parse_ok(R"({"id":"1.21.1","type":"release","libraries":[]})");
    const VersionJson child = parse_ok(
        R"({"id":"fabric-loader-0.16.9-1.21.1","inheritsFrom":"1.21.1",)"
        R"("mainClass":"net.fabricmc.loader.impl.launch.knot.KnotClient",)"
        R"("libraries":[{"name":"net.fabricmc:fabric-loader:0.16.9"}],)"
        R"("sscl":{"loader":"fabric","loaderVersion":"0.16.9","minecraftVersion":"1.21.1"}})");
    REQUIRE(child.sscl.has);
    REQUIRE(child.sscl.loader == "fabric");
    REQUIRE(child.sscl.loader_version == "0.16.9");
    REQUIRE(child.sscl.minecraft_version == "1.21.1");

    const VersionJson merged = sscl::core::merge_version(child, parent);
    REQUIRE(merged.sscl.has);
    REQUIRE(merged.sscl.loader == "fabric");
    REQUIRE(merged.sscl.minecraft_version == "1.21.1");
    REQUIRE(merged.type == "release");
}

TEST_CASE("instance.paths: 隔离策略矩阵与运行目录") {
    const fs::path root = make_root("matrix");
    const InstallLayout layout = make_layout(root);
    std::error_code ec;
    fs::create_directories(layout.versions_dir, ec);

    const VersionJson release =
        parse_ok(R"({"id":"1.21.1","type":"release","libraries":[]})");
    const VersionJson snapshot =
        parse_ok(R"({"id":"25w14a","type":"snapshot","libraries":[]})");
    const VersionJson fabric = parse_ok(
        R"({"id":"fabric-loader-0.16.9-1.21.1","type":"release",)"
        R"("libraries":[{"name":"net.fabricmc:fabric-loader:0.16.9"}],)"
        R"("sscl":{"loader":"fabric","loaderVersion":"0.16.9","minecraftVersion":"1.21.1"}})");

    const auto all = sscl::core::resolve_instance_target(layout, IsolationMode::kAll, release, false);
    REQUIRE(all.is_ok());
    REQUIRE(all.value().isolated);
    const bool all_run_dir_ok = all.value().run_dir == layout.versions_dir / L"1.21.1";
    const bool all_mods_dir_ok =
        all.value().mods_dir == layout.versions_dir / L"1.21.1" / L"mods";
    REQUIRE(all_run_dir_ok);
    REQUIRE(all_mods_dir_ok);
    REQUIRE_FALSE(all.value().moddable);

    const auto off = sscl::core::resolve_instance_target(layout, IsolationMode::kOff, release, false);
    REQUIRE(off.is_ok());
    REQUIRE_FALSE(off.value().isolated);
    const bool off_run_dir_ok = off.value().run_dir == layout.game_dir;
    const bool off_mods_dir_ok = off.value().mods_dir == layout.game_dir / L"mods";
    REQUIRE(off_run_dir_ok);
    REQUIRE(off_mods_dir_ok);

    const auto modded = sscl::core::resolve_instance_target(layout, IsolationMode::kModded, fabric, false);
    REQUIRE(modded.is_ok());
    REQUIRE(modded.value().isolated);
    REQUIRE(modded.value().moddable);
    REQUIRE(modded.value().loader == "fabric");

    const auto modded_vanilla =
        sscl::core::resolve_instance_target(layout, IsolationMode::kModded, release, false);
    REQUIRE(modded_vanilla.is_ok());
    REQUIRE_FALSE(modded_vanilla.value().isolated);

    const auto non_release =
        sscl::core::resolve_instance_target(layout, IsolationMode::kNonRelease, snapshot, false);
    REQUIRE(non_release.is_ok());
    REQUIRE(non_release.value().isolated);

    cleanup(root);
}

TEST_CASE("instance.paths: mods/saves 自动隔离与覆盖文件优先") {
    const fs::path root = make_root("auto");
    const InstallLayout layout = make_layout(root);
    const VersionJson version =
        parse_ok(R"({"id":"1.20.1","type":"release","libraries":[]})");

    std::error_code ec;
    fs::create_directories(layout.versions_dir / L"1.20.1" / L"mods", ec);
    sscl::platform::write_file(layout.versions_dir / L"1.20.1" / L"mods" / L"a.jar", "x");

    // 版本目录已有 mods：即使全局关闭隔离也自动隔离（PCL 语义）。
    const auto auto_mods =
        sscl::core::resolve_instance_target(layout, IsolationMode::kOff, version, false);
    REQUIRE(auto_mods.is_ok());
    REQUIRE(auto_mods.value().isolated);

    // 显式覆盖优先于自动规则。
    REQUIRE(sscl::core::save_instance_settings(layout.versions_dir / L"1.20.1",
                                               {false, true})
                .is_ok());
    const auto override_off =
        sscl::core::resolve_instance_target(layout, IsolationMode::kAll, version, false);
    REQUIRE(override_off.is_ok());
    REQUIRE_FALSE(override_off.value().isolated);

    // saves 有子目录同样触发自动隔离。
    const VersionJson other =
        parse_ok(R"({"id":"1.19.2","type":"release","libraries":[]})");
    fs::create_directories(layout.versions_dir / L"1.19.2" / L"saves" / L"world", ec);
    const auto auto_saves =
        sscl::core::resolve_instance_target(layout, IsolationMode::kOff, other, false);
    REQUIRE(auto_saves.is_ok());
    REQUIRE(auto_saves.value().isolated);

    cleanup(root);
}

TEST_CASE("instance.paths: 加载器与 MC 版本识别") {
    const VersionJson vanilla =
        parse_ok(R"({"id":"1.21.1","type":"release","libraries":[]})");
    REQUIRE(sscl::core::detect_loader_key(vanilla).empty());
    REQUIRE(sscl::core::detect_minecraft_version(vanilla, "1.21.1") == "1.21.1");

    const VersionJson forge = parse_ok(
        R"({"id":"forge-1.20.1-47.2.0","type":"release",)"
        R"("libraries":[{"name":"net.minecraftforge:forge:1.20.1-47.2.0"}]})");
    REQUIRE(sscl::core::detect_loader_key(forge) == "forge");
    REQUIRE(sscl::core::detect_minecraft_version(forge, forge.id) == "1.20.1");

    const VersionJson neoforge = parse_ok(
        R"({"id":"neoforge-21.1.0","type":"release",)"
        R"("libraries":[{"name":"net.neoforged:neoforge:21.1.0"}]})");
    REQUIRE(sscl::core::detect_loader_key(neoforge) == "neoforge");

    const VersionJson quilt = parse_ok(
        R"({"id":"quilt-loader-0.26.0-1.21.1","type":"release",)"
        R"("libraries":[{"name":"org.quiltmc:quilt-loader:0.26.0"}]})");
    REQUIRE(sscl::core::detect_loader_key(quilt) == "quilt");

    const VersionJson fabric_id = parse_ok(
        R"({"id":"fabric-loader-0.16.9-1.21.1","type":"release","libraries":[]})");
    REQUIRE(sscl::core::detect_minecraft_version(fabric_id, fabric_id.id) == "1.21.1");
}

TEST_CASE("instance.config: 隔离/Mod 配置读写往返与默认值") {
    const fs::path path =
        fs::temp_directory_path() /
        ("sscl_test_mod_config_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");

    auto missing = sscl::core::load_launcher_config(path);
    REQUIRE(missing.is_ok());
    REQUIRE(missing.value().version_isolation == 4);
    REQUIRE_FALSE(missing.value().instances_migrated);
    REQUIRE(missing.value().mod_dependency_mode == 0);
    REQUIRE(missing.value().mod_platform == "modrinth");

    sscl::core::LauncherConfig config;
    config.version_isolation = 2;
    config.instances_migrated = true;
    config.mod_dependency_mode = 1;
    config.mod_platform = "curseforge";
    config.mod_last_target_version = "fabric-loader-0.16.9-1.21.1";
    config.selected_mc_folder = "E:/Cpp_Project/SSCL/SSCL/_testenv/minecraft";
    REQUIRE(sscl::core::save_launcher_config(path, config).is_ok());

    auto loaded = sscl::core::load_launcher_config(path);
    REQUIRE(loaded.is_ok());
    REQUIRE(loaded.value().version_isolation == 2);
    REQUIRE(loaded.value().instances_migrated);
    REQUIRE(loaded.value().mod_dependency_mode == 1);
    REQUIRE(loaded.value().mod_platform == "curseforge");
    REQUIRE(loaded.value().mod_last_target_version == "fabric-loader-0.16.9-1.21.1");
    REQUIRE(loaded.value().selected_mc_folder ==
            "E:/Cpp_Project/SSCL/SSCL/_testenv/minecraft");

    std::error_code ec;
    fs::remove(path, ec);
}

TEST_CASE("instance.isolation: 已有版本按 PCL 语义首次判定") {
    const fs::path root = make_root("migrate");
    const InstallLayout layout = make_layout(root);
    std::error_code ec;
    fs::create_directories(layout.versions_dir / L"old" / L"mods", ec);
    REQUIRE(sscl::platform::write_file(layout.versions_dir / L"old" / L"old.json",
                                       R"({"id":"old","type":"release"})")
                .is_ok());
    REQUIRE(sscl::platform::write_file(layout.versions_dir / L"old" / L"mods" / L"a.jar", "x")
                .is_ok());

    const VersionJson version =
        parse_ok(R"({"id":"old","type":"release","libraries":[]})");
    const auto target =
        sscl::core::resolve_instance_target(layout, IsolationMode::kAll, version, false);
    REQUIRE(target.is_ok());
    REQUIRE(target.value().isolated);  // mods 有文件 → 自动隔离
    cleanup(root);
}

TEST_CASE("instance.management: 版本摘要与逐版本隔离切换") {
    const fs::path root = make_root("summary");
    const InstallLayout layout = make_layout(root);
    const fs::path version_dir = layout.versions_dir / L"1.20.1";
    std::error_code ec;
    fs::create_directories(version_dir, ec);
    REQUIRE(sscl::platform::write_file(
                version_dir / L"1.20.1.json",
                R"({"id":"1.20.1","type":"release","libraries":[]})")
                .is_ok());
    REQUIRE(sscl::platform::write_file(version_dir / L"1.20.1.jar", "jar").is_ok());

    sscl::core::OsInfo os;
    os.name = "windows";
    os.arch = "x86_64";
    sscl::core::GameService service(layout, os, sscl::core::SchedulerConfig{});
    service.set_isolation_mode(IsolationMode::kAll);

    const auto summaries = service.version_summaries();
    REQUIRE(summaries.size() == 1);
    REQUIRE(summaries[0].minecraft_version == "1.20.1");
    REQUIRE(summaries[0].isolated);

    auto off = service.set_instance_isolation("1.20.1", false);
    REQUIRE(off.is_ok());
    REQUIRE_FALSE(off.value().isolated);
    auto settings = sscl::core::load_instance_settings(version_dir);
    REQUIRE(settings.is_ok());
    REQUIRE(settings.value().decided);
    REQUIRE_FALSE(settings.value().isolated);

    auto on = service.set_instance_isolation("1.20.1", true);
    REQUIRE(on.is_ok());
    REQUIRE(on.value().isolated);
    cleanup(root);
}

TEST_CASE("instance.registry: 版本名校验与分类卡片") {
    std::string reason;
    CHECK_FALSE(sscl::core::instance_name_is_valid("", &reason));
    CHECK_FALSE(sscl::core::instance_name_is_valid("a/b", &reason));
    CHECK_FALSE(sscl::core::instance_name_is_valid("CON", &reason));
    CHECK_FALSE(sscl::core::instance_name_is_valid("bad.", &reason));
    CHECK(sscl::core::instance_name_is_valid("1.21.1-Forge", &reason));

    auto make = [](const std::string& id, sscl::core::InstanceState state,
                   const std::string& time, int drop) {
        sscl::core::VersionSummary version;
        version.version.id = id;
        version.state = state;
        version.release_time = time;
        version.drop = drop;
        return version;
    };
    std::vector<sscl::core::VersionSummary> versions;
    versions.push_back(make("1.20.1", sscl::core::InstanceState::kOriginal, "2024-06-13", 210));
    versions.push_back(make("1.20.0", sscl::core::InstanceState::kOriginal, "2024-05-01", 210));
    versions.push_back(make("1.19.4", sscl::core::InstanceState::kOriginal, "2023-03-14", 194));
    versions.push_back(make("25w14a", sscl::core::InstanceState::kSnapshot, "2025-04-01", 251));
    versions.push_back(make("24w14a", sscl::core::InstanceState::kSnapshot, "2024-04-01", 240));
    versions.push_back(make("forge-47.2.0", sscl::core::InstanceState::kForge, "2024-08-01", 210));
    versions.push_back(make("fabric-0.16.9", sscl::core::InstanceState::kFabric, "2024-08-02", 210));
    versions.push_back(make("b1.7.3", sscl::core::InstanceState::kOld, "2011-07-08", 173));
    versions.push_back(make("2.0", sscl::core::InstanceState::kFool, "2013-04-01", 150));
    versions.push_back(make("broken", sscl::core::InstanceState::kError, "", 209));
    versions.back().error = "bad json";
    versions.push_back(make("hidden", sscl::core::InstanceState::kOriginal, "2024-01-01", 200));
    versions.back().category = sscl::core::InstanceCategory::kHidden;
    versions.push_back(make("star", sscl::core::InstanceState::kOriginal, "2024-01-02", 201));
    versions.back().starred = true;

    const auto cards = sscl::core::classify_instance_summaries(versions, false);
    REQUIRE(cards.size() == 6);
    CHECK(cards[0].category == sscl::core::InstanceCategory::kStar);
    CHECK(cards[0].title == "收藏夹");
    REQUIRE(cards[0].versions.size() == 1);
    CHECK(cards[0].versions[0].version.id == "star");
    CHECK(cards[1].category == sscl::core::InstanceCategory::kApi);
    CHECK(cards[1].title == "可安装 Mod");
    CHECK(cards[2].category == sscl::core::InstanceCategory::kOriginalLike);
    REQUIRE(cards[2].versions.size() == 3);  // 每个 Drop 最新 + 最新快照
    bool has_120 = false;
    bool has_119 = false;
    for (const auto& item : cards[2].versions) {
        has_120 = has_120 || item.version.id == "1.20.1";
        has_119 = has_119 || item.version.id == "1.19.4";
    }
    CHECK(has_120);
    CHECK(has_119);
    CHECK(cards[3].category == sscl::core::InstanceCategory::kRubbish);
    CHECK(cards[4].category == sscl::core::InstanceCategory::kFool);
    CHECK(cards[5].category == sscl::core::InstanceCategory::kError);

    const auto hidden = sscl::core::classify_instance_summaries(versions, true);
    REQUIRE(hidden.size() == 1);
    CHECK(hidden[0].title == "隐藏的版本");
    REQUIRE(hidden[0].versions.size() == 1);
    CHECK(hidden[0].versions[0].version.id == "hidden");
}

TEST_CASE("instance.settings: 每版本设置 v2 往返与旧文件兼容") {
    const fs::path root = make_root("settings_v2");
    const fs::path version_dir = root / L"1.20.1";
    std::error_code ec;
    fs::create_directories(version_dir, ec);

    sscl::core::InstanceSettings settings;
    settings.isolated = true;
    settings.decided = true;
    settings.starred = true;
    settings.category = 2;
    settings.custom_info = "整合包";
    settings.logo = "PCL/Logo.png";
    settings.window_title = "SSCL";
    settings.version_info = "hello";
    settings.java_mode = 3;
    settings.java_range = "[17, 21)";
    settings.java_path = "D:/JDK/bin/java.exe";
    settings.ram_type = 1;
    settings.ram_custom_gb = 8;
    settings.ram_optimize = 1;
    settings.server_login = 4;
    settings.server_nide = "nide";
    settings.server_auth_url = "https://example.com/api";
    settings.server_auth_register = "https://example.com/register";
    settings.server_auth_name = "测试服";
    settings.server_enter = "127.0.0.1:25565";
    settings.jvm_args = "-XX:+UseG1GC";
    settings.game_args = "--demo";
    settings.gc_mode = 3;
    settings.pre_launch = "echo hi";
    settings.pre_launch_wait = true;
    settings.disable_mod_update = true;
    settings.disable_file_check = true;
    settings.disable_jlw = true;
    settings.disable_lua = true;
    REQUIRE(sscl::core::save_instance_settings(version_dir, settings).is_ok());

    auto loaded = sscl::core::load_instance_settings(version_dir);
    REQUIRE(loaded.is_ok());
    CHECK(loaded.value().starred);
    CHECK(loaded.value().category == 2);
    CHECK(loaded.value().custom_info == "整合包");
    CHECK(loaded.value().logo == "PCL/Logo.png");
    CHECK(loaded.value().java_mode == 3);
    CHECK(loaded.value().java_range == "[17, 21)");
    CHECK(loaded.value().java_path == "D:/JDK/bin/java.exe");
    CHECK(loaded.value().ram_type == 1);
    CHECK(loaded.value().ram_custom_gb == 8);
    CHECK(loaded.value().ram_optimize == 1);
    CHECK(loaded.value().server_login == 4);
    CHECK(loaded.value().server_nide == "nide");
    CHECK(loaded.value().server_auth_name == "测试服");
    CHECK(loaded.value().server_enter == "127.0.0.1:25565");
    CHECK(loaded.value().jvm_args == "-XX:+UseG1GC");
    CHECK(loaded.value().game_args == "--demo");
    CHECK(loaded.value().gc_mode == 3);
    CHECK(loaded.value().pre_launch == "echo hi");
    CHECK(loaded.value().pre_launch_wait);
    CHECK(loaded.value().disable_mod_update);
    CHECK(loaded.value().disable_file_check);
    CHECK(loaded.value().disable_jlw);
    CHECK(loaded.value().disable_lua);

    REQUIRE(sscl::platform::write_file(version_dir / L"sscl-instance.json",
                                       R"({"isolated":true})")
                .is_ok());
    auto legacy = sscl::core::load_instance_settings(version_dir);
    REQUIRE(legacy.is_ok());
    CHECK(legacy.value().isolated);
    CHECK(legacy.value().decided);
    cleanup(root);
}

TEST_CASE("instance.registry: 文件夹扫描去重/命名/选中") {
    const fs::path root = make_root("folders");
    const fs::path base = root / L"minecraft";
    const fs::path sibling = root / L"Other";
    const fs::path official = root / L"official" / L".minecraft";
    const fs::path custom = root / L"custom";
    std::error_code ec;
    fs::create_directories(base / L"versions", ec);
    fs::create_directories(sibling / L"versions", ec);
    fs::create_directories(official / L"versions", ec);
    fs::create_directories(custom, ec);

    std::vector<sscl::core::McFolderConfig> saved;
    saved.push_back(sscl::core::McFolderConfig{"我的主目录", base.string(), 0});
    saved.push_back(sscl::core::McFolderConfig{"自定义", custom.string(), 2});
    const auto folders = sscl::core::scan_mc_folders(
        base, official, saved, official.string());
    REQUIRE(folders.size() >= 4);
    bool has_base = false;
    bool has_official = false;
    bool has_custom = false;
    for (const auto& folder : folders) {
        if (folder.name == "我的主目录") {
            has_base = true;
            CHECK(folder.type == sscl::core::McFolderType::kRenamedVanilla);
        }
        if (folder.name == "官方启动器文件夹") {
            has_official = true;
            CHECK(folder.selected);
        }
        if (folder.name == "自定义") {
            has_custom = true;
            CHECK(folder.type == sscl::core::McFolderType::kCustom);
        }
    }
    CHECK(has_base);
    CHECK(has_official);
    CHECK(has_custom);
    cleanup(root);
}

TEST_CASE("instance.launch: 每版本内存/JVM/GC/Java 范围合并") {
    sscl::core::InstanceSettings settings;
    settings.ram_type = 0;
    auto merged = sscl::core::merge_instance_launch(settings, 4096);
    CHECK(merged.memory_mb == 4096);

    settings.ram_type = 1;
    settings.ram_custom_gb = 8;
    merged = sscl::core::merge_instance_launch(settings, 4096);
    CHECK(merged.memory_mb == 8192);

    settings.ram_custom_gb = 99;
    merged = sscl::core::merge_instance_launch(settings, 4096);
    CHECK(merged.memory_mb == 64 * 1024);

    settings.ram_type = 2;
    settings.java_mode = 1;
    settings.java_range = "[17.0.1, 25.0)";
    settings.jvm_args = "-XX:+UseG1GC -Xmx2G";
    settings.game_args = "--demo";
    settings.gc_mode = 4;
    merged = sscl::core::merge_instance_launch(settings, 4096);
    CHECK(merged.memory_mb == 4096);
    CHECK(merged.java_mode == 1);
    CHECK(merged.java_range == "[17.0.1, 25.0)");
    CHECK(merged.jvm_args == "-XX:+UseG1GC -Xmx2G");
    CHECK(merged.game_args == "--demo");
    CHECK(merged.gc_mode == 4);

    int min_major = 0;
    int max_major = 0;
    bool max_inclusive = true;
    CHECK(sscl::core::parse_java_range("[17.0.1, 25.0)", &min_major, &max_major,
                                       &max_inclusive));
    CHECK(min_major == 17);
    CHECK(max_major == 25);
    CHECK_FALSE(max_inclusive);
    CHECK(sscl::core::parse_java_range("[1.8, 8]", &min_major, &max_major, &max_inclusive));
    CHECK(min_major == 8);
    CHECK(max_major == 8);
    CHECK(max_inclusive);
    CHECK_FALSE(sscl::core::parse_java_range("[17, 25", &min_major, &max_major,
                                             &max_inclusive));
    CHECK_FALSE(sscl::core::parse_java_range("[25, 17]", &min_major, &max_major,
                                              &max_inclusive));
}

TEST_CASE("instance.management: 重命名/依赖阻止/永久删除") {    const fs::path root = make_root("rename");
    const InstallLayout layout = make_layout(root);
    const fs::path old_dir = layout.versions_dir / L"old";
    std::error_code ec;
    fs::create_directories(old_dir, ec);
    REQUIRE(sscl::platform::write_file(
                old_dir / L"old.json",
                R"({"id":"old","type":"release","mainClass":"a.B","libraries":[]})")
                .is_ok());
    REQUIRE(sscl::platform::write_file(old_dir / L"old.jar", "jar").is_ok());

    sscl::core::OsInfo os;
    os.name = "windows";
    os.arch = "x86_64";
    sscl::core::GameService service(layout, os, sscl::core::SchedulerConfig{});

    auto renamed = service.rename_instance("old", "new");
    REQUIRE(renamed.is_ok());
    CHECK_FALSE(fs::exists(old_dir));
    const fs::path new_dir = layout.versions_dir / L"new";
    CHECK(fs::is_regular_file(new_dir / L"new.jar"));
    auto text = sscl::platform::read_file(new_dir / L"new.json");
    REQUIRE(text.is_ok());
    CHECK(text.value().find("\"id\": \"new\"") != std::string::npos);

    fs::create_directories(layout.versions_dir / L"child", ec);
    REQUIRE(sscl::platform::write_file(
                layout.versions_dir / L"child" / L"child.json",
                R"({"id":"child","type":"release","mainClass":"a.C","inheritsFrom":"new"})")
                .is_ok());    auto blocked = service.rename_instance("new", "new2");
    REQUIRE(blocked.is_error());
    CHECK(blocked.error().code == "instance.rename_dependency");

    auto deleted = service.delete_instance("child", true);
    REQUIRE(deleted.is_ok());
    CHECK_FALSE(fs::exists(layout.versions_dir / L"child"));
    cleanup(root);
}
