#include <string>

#include <doctest/doctest.h>

#include "core/game/instance_paths.hpp"
#include "core/game/pcl_version.hpp"
#include "core/launch/version_json.hpp"

namespace {

using sscl::core::PclVersionInfo;
using sscl::core::VersionJson;

VersionJson parse_ok(const std::string& text) {
    auto parsed = sscl::core::parse_version_json(text);
    REQUIRE(parsed.is_ok());
    return parsed.value();
}

PclVersionInfo detect(const std::string& text, const std::string& instance = {}) {
    const VersionJson version = parse_ok(text);
    return sscl::core::detect_pcl_version(version, instance.empty() ? version.id : instance);
}

}  // namespace

TEST_CASE("pcl.version: clientVersion / Drop / 正式版") {
    const VersionJson version = parse_ok(
        R"({"id":"pcl-1.20.1","clientVersion":"1.20.1","type":"release",)"
        R"("releaseTime":"2024-06-13T08:00:00+00:00","libraries":[]})");
    const PclVersionInfo info = sscl::core::detect_pcl_version(version, "1.20.1");
    REQUIRE(info.vanilla_name == "1.20.1");
    REQUIRE(info.valid);
    REQUIRE(info.drop == 200);
    REQUIRE(info.is_release);
    REQUIRE_FALSE(info.is_snapshot);
    REQUIRE_FALSE(info.is_fool);
    REQUIRE(sscl::core::mc_version_drop("1.20.1") == 200);
    REQUIRE(sscl::core::mc_version_drop("26.1.5") == 261);
    REQUIRE(sscl::core::mc_version_drop("16w01a") == 209);
    REQUIRE(sscl::core::is_standard_mc_version("1.20.1"));
    REQUIRE(sscl::core::is_standard_mc_version("26.1"));
    REQUIRE_FALSE(sscl::core::is_standard_mc_version("16w01a"));
}

TEST_CASE("pcl.version: 快照/愚人节/远古") {
    const PclVersionInfo snapshot = detect(
        R"({"id":"25w14a","type":"snapshot","releaseTime":"2025-04-01T00:00:00Z",)"
        R"("libraries":[]})");
    REQUIRE(snapshot.vanilla_name == "25w14a");
    REQUIRE(snapshot.is_snapshot);
    REQUIRE_FALSE(snapshot.is_release);

    const PclVersionInfo fool =
        detect(R"({"id":"2.0","type":"release","libraries":[]})");
    REQUIRE(fool.is_fool);
    REQUIRE_FALSE(fool.is_release);
    REQUIRE(fool.drop == 50);  // 2.0 映射到 1.5.1 → Drop 50

    const PclVersionInfo old =
        detect(R"({"id":"old","type":"release","releaseTime":"2011-01-01T00:00:00Z",)"
               R"("libraries":[]})");
    REQUIRE(old.vanilla_name == "Old");
    REQUIRE_FALSE(old.is_release);
    REQUIRE(old.drop == 209);
}

TEST_CASE("pcl.version: HMCL patches 合并") {
    const VersionJson version = parse_ok(
        R"({"id":"hmcl-1.20.1","patches":[)"
        R"({"id":"forge","priority":1,"mainClass":"net.minecraftforge.client.launch.Mod",)"
        R"("libraries":[{"name":"net.minecraftforge:forge:1.20.1-47.2.0"}]},)"
        R"({"id":"game","version":"1.20.1","priority":0}],)"
        R"("type":"release","libraries":[]})");
    REQUIRE(version.id == "hmcl-1.20.1");
    REQUIRE(version.main_class == "net.minecraftforge.client.launch.Mod");
    REQUIRE(version.inherits_from.empty());
    const PclVersionInfo info = sscl::core::detect_pcl_version(version, "hmcl-1.20.1");
    REQUIRE(info.vanilla_name == "1.20.1");
    REQUIRE(info.drop == 200);
}

TEST_CASE("pcl.version: inheritsFrom / FML 参数") {
    const PclVersionInfo inherit =
        detect(R"({"id":"forge-47.2.0","inheritsFrom":"1.20.1","libraries":[]})");
    REQUIRE(inherit.vanilla_name == "1.20.1");

    const PclVersionInfo fml = detect(
        R"({"id":"neoforge-x","arguments":{"game":["--fml.mcVersion","1.21.1"]},)"
        R"("type":"release","libraries":[]})");
    REQUIRE(fml.vanilla_name == "1.21.1");
    REQUIRE(fml.drop == 210);
}

// D-116：「附加」页首屏用实例的原版号做 Mod 平台 versions 筛选；
// 若把加载器 id（neoforge-21.11.48）当成原版号，平台会返回 0 条结果。
TEST_CASE("pcl.version: 加载器子版本不得把加载器 id 当原版号（D-116）") {
    REQUIRE(sscl::core::is_plausible_mc_version("1.21.11"));
    REQUIRE(sscl::core::is_plausible_mc_version("26.1"));
    REQUIRE(sscl::core::is_plausible_mc_version("16w01a"));
    REQUIRE(sscl::core::is_plausible_mc_version("Old"));
    REQUIRE_FALSE(sscl::core::is_plausible_mc_version("neoforge-21.11.48"));
    REQUIRE_FALSE(sscl::core::is_plausible_mc_version("forge-1.21.11-61.2.4"));
    REQUIRE_FALSE(sscl::core::is_plausible_mc_version("fabric-loader-0.20.1-1.21.11"));
    REQUIRE_FALSE(sscl::core::is_plausible_mc_version("Unknown"));
    REQUIRE_FALSE(sscl::core::is_plausible_mc_version(""));

    // SSCL 安装的加载器子版本：sscl.minecraftVersion 优先。
    const VersionJson child = parse_ok(
        R"({"id":"neoforge-21.11.48","inheritsFrom":"1.21.11",)"
        R"("sscl":{"loader":"neoforge","loaderVersion":"21.11.48",)"
        R"("minecraftVersion":"1.21.11"},)"
        R"("arguments":{"game":["--fml.mcVersion","1.21.11"]},"libraries":[]})");
    REQUIRE(sscl::core::detect_pcl_version(child, child.id).vanilla_name == "1.21.11");

    // 合并父版本后 jar 会回填成加载器 id（merge_version 语义），识别结果仍须是原版号。
    const VersionJson parent =
        parse_ok(R"({"id":"1.21.11","type":"release","libraries":[]})");
    const VersionJson merged = sscl::core::merge_version(child, parent);
    REQUIRE(merged.jar == "neoforge-21.11.48");
    const PclVersionInfo info = sscl::core::detect_pcl_version(merged, merged.id);
    REQUIRE(info.vanilla_name == "1.21.11");
    REQUIRE(sscl::core::detect_minecraft_version(merged, merged.id) == "1.21.11");
}
