#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "core/mod/mod_download_planner.hpp"
#include "platform/file_utils.hpp"

namespace {

namespace fs = std::filesystem;
using sscl::core::InstanceTarget;
using sscl::core::ModLoader;
using sscl::core::ModVersion;

InstanceTarget target(const fs::path& game, const std::string& id, const std::string& loader,
                      const std::string& mc) {
    InstanceTarget value;
    value.version_id = id;
    value.game_dir = game;
    value.version_dir = game / L"versions" / fs::path(id);
    value.run_dir = value.version_dir;
    value.mods_dir = value.run_dir / L"mods";
    value.loader = loader;
    value.minecraft_version = mc;
    value.moddable = !loader.empty();
    value.isolated = true;
    return value;
}

ModVersion version() {
    ModVersion value;
    value.platform = sscl::core::ModPlatform::kModrinth;
    value.project_id = "p";
    value.version_id = "v";
    value.game_versions = {"1.20.1"};
    value.loaders = {ModLoader::kFabric};
    return value;
}

}  // namespace

TEST_CASE("pcl.download: 默认目录优先选中兼容实例") {
    const fs::path game = fs::temp_directory_path() / L"sscl_pcl_plan";
    const InstanceTarget selected = target(game, "fabric-1.20.1", "fabric", "1.20.1");
    const ModVersion mod = version();
    const auto plan = sscl::core::plan_pcl_mod_download(
        game, &selected, {selected}, "", mod, "sodium-0.5.jar", "钠", 1);
    REQUIRE(plan.selected_instance_compatible);
    REQUIRE(plan.default_folder == selected.run_dir / L"mods");
    REQUIRE(plan.file_name == "[钠] sodium-0.5.jar");
}

TEST_CASE("pcl.download: 不兼容时扫描其他实例，再回退 MC 根") {
    const fs::path game = fs::temp_directory_path() / L"sscl_pcl_plan2";
    const InstanceTarget incompatible = target(game, "forge-1.20.1", "forge", "1.20.1");
    const InstanceTarget compatible = target(game, "fabric-1.20.1", "fabric", "1.20.1");
    const ModVersion mod = version();
    const auto plan = sscl::core::plan_pcl_mod_download(
        game, &incompatible, {incompatible, compatible}, "", mod, "x.jar", "", 1);
    REQUIRE_FALSE(plan.selected_instance_compatible);
    REQUIRE(plan.default_folder == compatible.run_dir / L"mods");

    ModVersion other = mod;
    other.game_versions = {"1.19.2"};
    const auto fallback = sscl::core::plan_pcl_mod_download(
        game, &incompatible, {incompatible, compatible}, "", other, "x.jar", "", 1);
    REQUIRE(fallback.default_folder == game);
}

TEST_CASE("pcl.download: 文件名 PCL 规则与安全兜底") {
    REQUIRE(sscl::core::pcl_download_file_name("a~b.jar", "", 1) == "a-b.jar");
    REQUIRE(sscl::core::pcl_download_file_name("../../evil.jar", "", 1) == "evil.jar");
    REQUIRE(sscl::core::pcl_download_file_name("a.jar", "中文名", 0) == "【中文名】a.jar");
    REQUIRE(sscl::core::pcl_download_file_name("a.jar", "中文名", 2) == "中文名-a.jar");
    REQUIRE(sscl::core::pcl_download_file_name("a<>.jar", "", 1) == "a＜＞.jar");
    REQUIRE(sscl::core::pcl_download_file_name("CON.jar", "", 1) == "_CON.jar");
}
