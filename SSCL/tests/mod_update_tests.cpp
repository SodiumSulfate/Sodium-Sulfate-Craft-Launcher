#include <string>

#include <doctest/doctest.h>

#include "core/mod/mod_update.hpp"

using sscl::core::ModLoader;
using sscl::core::ModProject;
using sscl::core::ModVersion;

TEST_CASE("mod.update: CF latestFilesIndexes 精确匹配") {
    ModProject project;
    project.id = "238222";
    project.latest_files = {
        {"1.21.1", ModLoader::kNeoForge, "42"},
        {"1.20.1", ModLoader::kForge, "41"},
        {"1.21.1", ModLoader::kUnknown, "40"},
    };
    REQUIRE(sscl::core::choose_curseforge_latest_file(project, "1.21.1",
                                                      ModLoader::kNeoForge) == "42");
    REQUIRE(sscl::core::choose_curseforge_latest_file(project, "1.20.1",
                                                      ModLoader::kForge) == "41");
    REQUIRE(sscl::core::choose_curseforge_latest_file(project, "1.19.2",
                                                      ModLoader::kForge).empty());
    REQUIRE(sscl::core::choose_curseforge_latest_file(project, "1.21.1",
                                                      ModLoader::kForge).empty());
}

TEST_CASE("mod.update: PCL 更新文件名") {
    REQUIRE(sscl::core::pcl_update_file_name("sodium-0.5.1.jar", "sodium-0.6.0.jar") ==
            "sodium-0.6.0.jar");
    REQUIRE(sscl::core::pcl_update_file_name("jei-1.20.1-forge-15.0.jar",
                                             "jei-1.20.1-forge-16.0.jar") ==
            "jei-1.20.1-forge-16.0.jar");
    REQUIRE(sscl::core::pcl_update_file_name("a.jar", "b.jar") == "b.jar");
}

TEST_CASE("mod.update: 更新判定") {    ModVersion current;
    current.version_id = "old";
    current.published_at = "2026-10-01T00:00:00Z";
    current.downloadable = true;

    ModVersion latest;
    latest.version_id = "new";
    latest.published_at = "2026-10-02T00:00:00Z";
    latest.downloadable = true;
    latest.files.push_back({"x.jar", "https://example/x.jar", "newsha", {}, 1, true, {}});
    REQUIRE(sscl::core::should_update_mod(current, latest, "oldsha"));

    // 同 hash 不更新。
    latest.files[0].sha1 = "oldsha";
    REQUIRE_FALSE(sscl::core::should_update_mod(current, latest, "oldsha"));

    // 时间相同/更旧不更新。
    latest.published_at = current.published_at;
    latest.files[0].sha1 = "newsha";
    REQUIRE_FALSE(sscl::core::should_update_mod(current, latest, "oldsha"));
}
