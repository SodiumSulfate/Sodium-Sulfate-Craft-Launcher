#include <doctest/doctest.h>

#include <string>

#include "core/launch/version_json.hpp"

namespace core = sscl::core;

namespace {

const core::OsInfo kWin64{"windows", "10.0.19045", "x86_64"};

core::Library make_library(const std::string& name) {
    core::Library library;
    library.name = name;
    return library;
}

}  // namespace

TEST_CASE("version.parse: 最小版本 JSON 与缺失 id") {
    auto ok = core::parse_version_json(R"({"id":"1.20.1","mainClass":"net.minecraft.client.main.Main"})");
    REQUIRE(ok.is_ok());
    CHECK(ok.value().id == "1.20.1");
    CHECK(ok.value().main_class == "net.minecraft.client.main.Main");

    auto bad = core::parse_version_json(R"({"mainClass":"x"})");
    CHECK(bad.is_error());
    CHECK(bad.error().code == "version.missing_id");

    auto broken = core::parse_version_json("{not json");
    CHECK(broken.is_error());
}

TEST_CASE("version.merge: libraries 子前父后、arguments 父前子后") {
    core::VersionJson parent;
    parent.id = "1.20.1";
    parent.main_class = "parent.Main";
    parent.libraries.push_back(make_library("p:parent:1"));
    parent.has_arguments = true;
    parent.jvm_arguments.entries.push_back({{}, {"-Xparent"}});
    parent.game_arguments.entries.push_back({{}, {"--parent"}});

    core::VersionJson child;
    child.id = "fabric-loader";
    child.inherits_from = "1.20.1";
    child.main_class = "child.Main";
    child.libraries.push_back(make_library("c:child:1"));
    child.has_arguments = true;
    child.jvm_arguments.entries.push_back({{}, {"-Xchild"}});
    child.game_arguments.entries.push_back({{}, {"--child"}});

    const core::VersionJson merged = core::merge_version(child, parent);

    CHECK(merged.id == "fabric-loader");
    CHECK(merged.main_class == "child.Main");
    REQUIRE(merged.libraries.size() == 2);
    CHECK(merged.libraries[0].name == "c:child:1");   // 子前
    CHECK(merged.libraries[1].name == "p:parent:1");  // 父后

    REQUIRE(merged.jvm_arguments.entries.size() == 2);
    CHECK(merged.jvm_arguments.entries[0].values[0] == "-Xparent");  // 父前
    CHECK(merged.jvm_arguments.entries[1].values[0] == "-Xchild");   // 子后
    REQUIRE(merged.game_arguments.entries.size() == 2);
    CHECK(merged.game_arguments.entries[0].values[0] == "--parent");
    CHECK(merged.game_arguments.entries[1].values[0] == "--child");
}

TEST_CASE("version.merge: jar 缺省 = id、minimumLauncherVersion 取 max") {
    core::VersionJson parent;
    parent.id = "p";
    parent.minimum_launcher_version = 21;
    core::VersionJson child;
    child.id = "c";
    child.minimum_launcher_version = 16;

    const core::VersionJson merged = core::merge_version(child, parent);
    CHECK(merged.jar == "c");  // 都无 jar 时用合并后 id
    CHECK(merged.minimum_launcher_version == 21);
}

TEST_CASE("version.rules: 默认 DISALLOW、最后生效规则覆盖") {
    core::Rule allow;
    allow.action = "allow";
    core::Rule disallow;
    disallow.action = "disallow";

    CHECK(core::rules_allow({}, kWin64, {}));                        // 无规则 = 允许
    CHECK_FALSE(core::rules_allow({disallow}, kWin64, {}));          // 仅 disallow
    CHECK(core::rules_allow({allow}, kWin64, {}));                   // 仅 allow
    CHECK_FALSE(core::rules_allow({allow, disallow}, kWin64, {}));   // 后者覆盖
    CHECK(core::rules_allow({disallow, allow}, kWin64, {}));
}

TEST_CASE("version.rules: os 匹配（含 win-x86 复合与正则）") {
    core::Rule allow_win;
    allow_win.action = "allow";
    allow_win.has_os = true;
    allow_win.os.name = "windows";

    CHECK(core::rules_allow({allow_win}, kWin64, {}));
    CHECK_FALSE(core::rules_allow({allow_win}, {"osx", "23.0", "x86_64"}, {}));

    core::Rule allow_win10 = allow_win;
    // matches() 是全匹配（HMCL OSRestriction.java:88）：模式须写全，如 ^10\..*
    allow_win10.os.version = "^10\\..*";
    CHECK(core::rules_allow({allow_win10}, {"windows", "10.0.19045", "x86_64"}, {}));
    CHECK_FALSE(core::rules_allow({allow_win10}, {"windows", "6.1.7601", "x86_64"}, {}));

    // 全匹配语义锁定：^10\.（不带 .*）不匹配 "10.0.19045"
    core::Rule prefix_only = allow_win;
    prefix_only.os.version = "^10\\.";
    CHECK_FALSE(core::rules_allow({prefix_only}, {"windows", "10.0.19045", "x86_64"}, {}));

    core::Rule allow_x86 = allow_win;
    allow_x86.os.name = "win-x86";  // 复合形式
    CHECK(core::rules_allow({allow_x86}, {"windows", "10.0", "x86"}, {}));
    CHECK_FALSE(core::rules_allow({allow_x86}, {"windows", "10.0", "x86_64"}, {}));

    core::Rule bad_regex = allow_win;
    bad_regex.os.version = "^(";
    CHECK_FALSE(core::rules_allow({bad_regex}, kWin64, {}));  // 非法正则：规则不生效
}

TEST_CASE("version.rules: features 逐项完全相等才生效") {
    core::Rule rule;
    rule.action = "allow";
    rule.features["has_custom_resolution"] = true;

    CHECK(core::rules_allow({rule}, kWin64, {{"has_custom_resolution", true}}));
    CHECK_FALSE(core::rules_allow({rule}, kWin64, {{"has_custom_resolution", false}}));
    CHECK_FALSE(core::rules_allow({rule}, kWin64, {}));
}

TEST_CASE("version.library: natives 描述符与 Maven 路径") {
    core::Library library = make_library("org.lwjgl:lwjgl:3.3.1");
    library.natives["windows"] = "natives-windows-${arch}";

    CHECK(core::select_native_classifier(library, "windows", true) == "natives-windows-64");
    CHECK(core::select_native_classifier(library, "windows", false) == "natives-windows-32");
    CHECK(core::select_native_classifier(library, "osx", true).empty());

    CHECK(core::library_relative_path("com.mojang:patchy:1.1") ==
          "com/mojang/patchy/1.1/patchy-1.1.jar");
    CHECK(core::library_relative_path("org.lwjgl:lwjgl:3.3.1:natives-windows") ==
          "org/lwjgl/lwjgl/3.3.1/lwjgl-3.3.1-natives-windows.jar");
}
