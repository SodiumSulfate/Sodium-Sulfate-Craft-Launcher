#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "core/game/file_plan.hpp"
#include "core/game/game_service.hpp"
#include "core/game/launcher_config.hpp"
#include "core/launch/version_json.hpp"

namespace core = sscl::core;

namespace {

const core::OsInfo kWin64{"windows", "10.0.19045", "x86_64"};

core::InstallLayout make_layout() {
    core::InstallLayout layout;
    layout.game_dir = L"C:\\game";
    layout.versions_dir = L"C:\\game\\versions";
    layout.libraries_dir = L"C:\\game\\libraries";
    layout.assets_dir = L"C:\\game\\assets";
    layout.natives_dir = L"C:\\game\\versions\\mc\\mc-natives";
    layout.version_id = "mc";
    return layout;
}

}  // namespace

TEST_CASE("game.assets: 资产索引解析") {
    auto index = core::parse_asset_index(R"({
        "objects": {
            "minecraft/sounds/step/stone1.ogg": {"hash": "00112233445566778899aabbccddeeff00112233", "size": 123},
            "lang/en_us.json": {"hash": "ffeeddccbbaa99887766554433221100ffeeddcc", "size": 9}
        }
    })");
    REQUIRE(index.is_ok());
    REQUIRE(index.value().objects.size() == 2);
    // nlohmann object 遍历按 key 字典序（非 JSON 文本顺序）→ 断言与顺序无关
    bool found_sound = false;
    bool found_lang = false;
    for (const auto& object : index.value().objects) {
        if (object.virtual_path == "minecraft/sounds/step/stone1.ogg" &&
            object.hash == "00112233445566778899aabbccddeeff00112233" && object.size == 123) {
            found_sound = true;
        }
        if (object.virtual_path == "lang/en_us.json" &&
            object.hash == "ffeeddccbbaa99887766554433221100ffeeddcc") {
            found_lang = true;
        }
    }
    CHECK(found_sound);
    CHECK(found_lang);

    CHECK(core::parse_asset_index("{bad").is_error());
}

TEST_CASE("game.plan: 版本文件计划（缺省 URL 与 natives）") {
    core::VersionJson version;
    version.id = "mc";
    version.main_class = "main.Main";

    // 普通库（无 downloads → 默认 libraries.minecraft.net）
    core::Library plain;
    plain.name = "com.mojang:patchy:1.1";
    version.libraries.push_back(plain);

    // natives 库
    core::Library lwjgl;
    lwjgl.name = "org.lwjgl:lwjgl:3.3.1";
    lwjgl.natives["windows"] = "natives-windows";
    version.libraries.push_back(lwjgl);

    // 资产索引
    version.has_asset_index = true;
    version.asset_index.id = "5";
    version.asset_index.url = "https://launchermeta.mojang.com/v1/packages/x/5.json";

    // log4j
    version.has_logging = true;
    version.logging.file.url = "https://piston-data.mojang.com/log4j2.xml";
    version.logging.file.sha1 = "abc";

    const auto requests = core::plan_version_files(version, make_layout(), kWin64, {});

    // client jar（无 downloads → BMCLAPI versions 缺省形式，HMCL Constants.java:31）
    REQUIRE(requests.size() >= 5);
    CHECK(requests[0].destination == std::filesystem::path(L"C:\\game\\versions\\mc\\mc.jar"));
    CHECK(requests[0].url == "https://bmclapi2.bangbang93.com/versions/mc/mc.jar");

    // 普通库 → 默认库 URL
    bool found_patchy = false;
    bool found_native = false;
    bool found_index = false;
    bool found_log4j = false;
    for (const auto& request : requests) {
        if (request.url.find("patchy-1.1.jar") != std::string::npos) {
            found_patchy = true;
            CHECK(request.url == "https://libraries.minecraft.net/com/mojang/patchy/1.1/patchy-1.1.jar");
        }
        if (request.display_name == "org.lwjgl:lwjgl:3.3.1:natives-windows") {
            found_native = true;
        }
        if (request.display_name == "asset index 5") {
            found_index = true;
        }
        if (request.display_name == "log4j2.xml") {
            found_log4j = true;
            CHECK(request.expected_sha1 == "abc");
        }
    }
    CHECK(found_patchy);
    CHECK(found_native);
    CHECK(found_index);
    CHECK(found_log4j);
}

TEST_CASE("game.plan: rules 过滤与资产对象路径") {
    core::VersionJson version;
    version.id = "mc";

    core::Library mac_only;
    mac_only.name = "a:b:1";
    core::Rule disallow_win;
    disallow_win.action = "disallow";
    disallow_win.has_os = true;
    disallow_win.os.name = "windows";
    mac_only.rules.push_back(disallow_win);
    version.libraries.push_back(mac_only);

    const auto requests = core::plan_version_files(version, make_layout(), kWin64, {});
    for (const auto& request : requests) {
        CHECK(request.display_name != "a:b:1");  // windows 下被 disallow
    }

    core::AssetObject object;
    object.hash = "00112233445566778899aabbccddeeff00112233";
    object.virtual_path = "x";
    core::AssetIndex index;
    index.objects.push_back(object);
    const auto assets = core::plan_asset_files(index, make_layout());
    REQUIRE(assets.size() == 1);
    CHECK(assets[0].destination ==
          std::filesystem::path(L"C:\\game\\assets\\objects\\00\\00112233445566778899aabbccddeeff00112233"));
    CHECK(assets[0].url ==
          "https://resources.download.minecraft.net/00/00112233445566778899aabbccddeeff00112233");
}

TEST_CASE("game.version_order: 自然比较（数字段按数值）") {
    CHECK(core::compare_version_ids("1.20.4", "1.20.10") < 0);
    CHECK(core::compare_version_ids("1.20.10", "1.20.4") > 0);
    CHECK(core::compare_version_ids("1.7.10", "1.12.2") < 0);
    CHECK(core::compare_version_ids("1.20.4", "1.20.4") == 0);
    CHECK(core::compare_version_ids("24w14a", "23w51a") > 0);
    CHECK(core::compare_version_ids("1.20", "1.20.1") < 0);
    // 前导零按数值比较
    CHECK(core::compare_version_ids("1.020.1", "1.20.1") == 0);
}

TEST_CASE("game.installed: 扫描 versions 目录并降序排序") {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("sscl_test_game_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::create_directories(root / L"versions" / L"1.7.10", ec);
    std::filesystem::create_directories(root / L"versions" / L"1.20.10", ec);
    std::filesystem::create_directories(root / L"versions" / L"1.20.4", ec);
    std::filesystem::create_directories(root / L"versions" / L"no-json", ec);
    // D-028：未完成下载（有 json 无 jar 且无 inheritsFrom）不得被认定为可启动
    std::filesystem::create_directories(root / L"versions" / L"incomplete", ec);
    // 加载器版本（inheritsFrom，无自有 jar）应保留
    std::filesystem::create_directories(root / L"versions" / L"fabric-1.20.4", ec);

    auto write_text = [](const std::filesystem::path& path, const std::string& text) {
        std::ofstream out(path, std::ios::binary);
        out << text;
    };
    write_text(root / L"versions" / L"1.7.10" / L"1.7.10.json",
               R"({"id":"1.7.10","type":"release"})");
    write_text(root / L"versions" / L"1.20.10" / L"1.20.10.json",
               R"({"id":"1.20.10","type":"release"})");
    write_text(root / L"versions" / L"1.20.4" / L"1.20.4.json",
               R"({"id":"1.20.4","type":"snapshot"})");
    write_text(root / L"versions" / L"no-json" / L"other.txt", "x");
    write_text(root / L"versions" / L"incomplete" / L"incomplete.json",
               R"({"id":"incomplete","type":"release"})");
    write_text(root / L"versions" / L"fabric-1.20.4" / L"fabric-1.20.4.json",
               R"({"id":"fabric-1.20.4","type":"release","inheritsFrom":"1.20.4"})");
    // 客户端 jar（可启动的必要条件）
    write_text(root / L"versions" / L"1.7.10" / L"1.7.10.jar", "jar");
    write_text(root / L"versions" / L"1.20.10" / L"1.20.10.jar", "jar");
    write_text(root / L"versions" / L"1.20.4" / L"1.20.4.jar", "jar");

    core::InstallLayout layout;
    layout.game_dir = root;
    layout.versions_dir = root / L"versions";
    layout.libraries_dir = root / L"libraries";
    layout.assets_dir = root / L"assets";
    core::GameService service(layout, kWin64);

    const auto installed = service.installed_versions();
    // PCL 对齐：被 fabric-1.20.4 继承、且无用户数据的 1.20.4 是安装内部基础，不列出。
    REQUIRE(installed.size() == 3);
    CHECK(installed[0].id == "1.20.10");  // 降序：新版本在前
    CHECK(installed[1].id == "1.7.10");
    CHECK(installed[2].id == "fabric-1.20.4");  // inheritsFrom 无 jar 保留（字母 id 段排序在后）

    // incomplete（无 jar 无 inheritsFrom）与 loader parent 都被过滤
    for (const auto& item : installed) {
        CHECK(item.id != "incomplete");
        CHECK(item.id != "1.20.4");
    }

    // 父版本一旦有用户数据（此处放一个 Mod），仍作为独立实例显示。
    const std::filesystem::path parent_mods = root / L"versions" / L"1.20.4" / L"mods";
    std::filesystem::create_directories(parent_mods, ec);
    write_text(parent_mods / L"demo.jar", "mod");
    const auto with_user_data = service.installed_versions();
    REQUIRE(with_user_data.size() == 4);
    bool parent_visible = false;
    for (const auto& item : with_user_data) {
        if (item.id == "1.20.4") {
            parent_visible = true;
            CHECK(item.type == "snapshot");
        }
    }
    CHECK(parent_visible);

    std::filesystem::remove_all(root, ec);
}

TEST_CASE("game.config: launcher_config 读写往返与损坏回退") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("sscl_test_config_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");

    // 缺失 → 默认
    auto missing = core::load_launcher_config(path);
    REQUIRE(missing.is_ok());
    CHECK(missing.value().player_name == "Player");
    CHECK(missing.value().selected_version_id.empty());

    // 往返
    core::LauncherConfig config;
    config.player_name = "Steve";
    config.selected_version_id = "1.20.4";
    config.window.valid = true;
    config.window.x = 120;
    config.window.y = 80;
    config.window.width = 1024;
    config.window.height = 640;
    config.window.maximized = true;
    REQUIRE(core::save_launcher_config(path, config).is_ok());
    auto loaded = core::load_launcher_config(path);
    REQUIRE(loaded.is_ok());
    CHECK(loaded.value().player_name == "Steve");
    CHECK(loaded.value().selected_version_id == "1.20.4");
    CHECK(loaded.value().window.valid);
    CHECK(loaded.value().window.x == 120);
    CHECK(loaded.value().window.y == 80);
    CHECK(loaded.value().window.width == 1024);
    CHECK(loaded.value().window.height == 640);
    CHECK(loaded.value().window.maximized);

    // 损坏 → 默认
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "{bad";
    }
    auto corrupt = core::load_launcher_config(path);
    REQUIRE(corrupt.is_ok());
    CHECK(corrupt.value().player_name == "Player");

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
