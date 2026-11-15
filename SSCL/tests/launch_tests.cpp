#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <string>

#include "core/launch/launch_command.hpp"
#include "platform/file_utils.hpp"
#include "platform/path_utils.hpp"

namespace core = sscl::core;

namespace {

core::LaunchContext make_context() {
    core::LaunchContext context;
    context.account.player_name = "Steve";
    context.account.uuid_compact = "0123456789abcdef0123456789abcdef";
    context.account.access_token = "token-xyz";
    context.paths.java_exe = L"C:\\java\\bin\\javaw.exe";
    context.paths.game_dir = L"C:\\game";
    context.paths.natives_dir = L"C:\\game\\natives";
    context.paths.assets_dir = L"C:\\game\\assets";
    context.paths.libraries_dir = L"C:\\game\\libraries";
    context.paths.version_jar = L"C:\\game\\versions\\1.20.1\\1.20.1.jar";
    context.settings.memory_mb = 2048;
    context.version_name = "1.20.1";
    context.version_type = "release";
    context.os = {"windows", "10.0.19045", "x86_64"};
    return context;
}

bool contains(const std::vector<std::string>& list, const std::string& value) {
    for (const std::string& item : list) {
        if (item == value) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("launch.placeholders: 替换与未知保留") {
    const std::map<std::string, std::string> values{{"${auth_player_name}", "Steve"},
                                                    {"${game_directory}", "C:\\game"}};
    CHECK(core::replace_placeholders("--username ${auth_player_name}", values) ==
          "--username Steve");
    // 未知占位符原样保留（StringArgument.java:45-53 行为）
    CHECK(core::replace_placeholders("${unknown_key}", values) == "${unknown_key}");
    CHECK(core::replace_placeholders("plain", values) == "plain");
}

TEST_CASE("launch.plan: 旧格式组装顺序与占位符") {
    core::VersionJson version;
    version.id = "old-mc";
    version.main_class = "net.minecraft.client.main.Main";
    version.minecraft_arguments =
        "--username ${auth_player_name} --version ${version_name} --gameDir ${game_directory}";

    core::LaunchContext context = make_context();
    core::LaunchPlan plan;
    auto built = core::build_launch_plan(version, context);
    REQUIRE(built.is_ok());
    plan = std::move(built.value());

    REQUIRE(plan.arguments.size() >= 8);
    CHECK(plan.arguments[0] == "-Xmx2048m");
    CHECK(plan.arguments[1] == "-Xms2048m");
    CHECK(plan.arguments[2].rfind("-Dfile.encoding=", 0) == 0);  // 系统 ACP 名

    // 旧格式 → 默认 JVM 集
    CHECK(contains(plan.arguments, "-Djava.library.path=C:\\game\\natives"));
    CHECK(contains(plan.arguments, "-Dminecraft.launcher.brand=SSCL"));
    CHECK(contains(plan.arguments, "-cp"));

    // mainClass 在 JVM 参数之后、游戏参数之前
    const auto main_it = std::find(plan.arguments.begin(), plan.arguments.end(),
                                   "net.minecraft.client.main.Main");
    REQUIRE(main_it != plan.arguments.end());
    CHECK(contains(plan.arguments, "--username"));
    CHECK(contains(plan.arguments, "Steve"));

    // 旧格式 + has_custom_resolution → --width/--height
    CHECK_FALSE(contains(plan.arguments, "--width"));
    context.settings.width = 1280;
    context.settings.height = 720;
    auto sized = core::build_launch_plan(version, context);
    REQUIRE(sized.is_ok());
    CHECK(contains(sized.value().arguments, "--width"));
    CHECK(contains(sized.value().arguments, "1280"));

    CHECK_FALSE(plan.used_argfile);
    CHECK(plan.command_line.find("javaw.exe") != std::string::npos);
}

TEST_CASE("launch.plan: 新格式 jvm/game 参数与 Win10 特判") {
    core::VersionJson version;
    version.id = "new-mc";
    version.main_class = "net.minecraft.client.main.Main";
    version.has_arguments = true;
    version.jvm_arguments.entries.push_back({{}, {"-Djava.library.path=${natives_directory}"}});
    core::RuledValues osx_only;
    osx_only.rules.push_back(core::Rule{"allow", true, {"osx", "", ""}, {}});
    osx_only.values.push_back("-XstartOnFirstThread");
    version.jvm_arguments.entries.push_back(osx_only);
    version.game_arguments.entries.push_back({{}, {"--username", "${auth_player_name}"}});

    core::LaunchContext context = make_context();
    auto built = core::build_launch_plan(version, context);
    REQUIRE(built.is_ok());
    const core::LaunchPlan& plan = built.value();

    // 有版本 jvm 参数 → 不注入默认 JVM 集
    CHECK_FALSE(contains(plan.arguments, "-Dminecraft.launcher.brand=SSCL"));
    CHECK(contains(plan.arguments, "-Djava.library.path=C:\\game\\natives"));
    // osx 规则在 windows 上不生效
    CHECK_FALSE(contains(plan.arguments, "-XstartOnFirstThread"));
    CHECK(contains(plan.arguments, "--username"));
    CHECK(contains(plan.arguments, "Steve"));
}

TEST_CASE("launch.argfile: 引号规则与写出") {
    const std::filesystem::path dir = sscl::platform::data_dir() / L"unittest_tmp";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / L"argfile-test.txt";

    auto written = core::write_argfile(file, {"plain", "has space", "quote\"inside", "back\\slash"});
    REQUIRE(written.is_ok());

    auto read_back = sscl::platform::read_file(file);
    REQUIRE(read_back.is_ok());
    const std::string& text = read_back.value();
    CHECK(text.find("plain\n") != std::string::npos);
    CHECK(text.find("\"has space\"\n") != std::string::npos);
    CHECK(text.find("\"quote\\\"inside\"\n") != std::string::npos);
    CHECK(text.find("\"back\\\\slash\"\n") != std::string::npos);
}

TEST_CASE("launch.paths: 非 ASCII 路径按 UTF-8 展开") {
    core::VersionJson version;
    version.id = "old-utf8";
    version.main_class = "net.minecraft.client.main.Main";
    version.minecraft_arguments = "--gameDir ${game_directory}";

    core::LaunchContext context = make_context();
    context.paths.game_dir = L"C:\\测试\\游戏";
    context.paths.natives_dir = L"C:\\测试\\游戏\\natives";

    auto built = core::build_launch_plan(version, context);
    REQUIRE(built.is_ok());
    CHECK(contains(built.value().arguments, "--gameDir"));
    CHECK(contains(built.value().arguments, "C:\\测试\\游戏"));
    CHECK(contains(built.value().arguments,
                   "-Djava.library.path=C:\\测试\\游戏\\natives"));
}
