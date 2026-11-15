#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "core/download/download_scheduler.hpp"
#include "core/game/instance_paths.hpp"
#include "core/mod/mod_installer.hpp"
#include "core/mod/mod_scanner.hpp"
#include "platform/file_utils.hpp"
#include "util/hash.hpp"

namespace {

namespace fs = std::filesystem;

fs::path make_root(const char* tag) {
    return fs::temp_directory_path() /
           ("sscl_test_modinstall_" + std::string(tag) + "_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

void cleanup(const fs::path& root) {
    std::error_code ec;
    fs::remove_all(root, ec);
}

sscl::core::ModResolved make_resolved(const std::string& project_id,
                                      const std::string& file_name,
                                      const std::string& sha1 = "abc123",
                                      bool downloadable = true) {
    sscl::core::ModResolved resolved;
    resolved.project.platform = sscl::core::ModPlatform::kModrinth;
    resolved.project.id = project_id;
    resolved.project.title = project_id;
    resolved.project.page_url = "https://modrinth.com/mod/" + project_id;
    resolved.version.platform = sscl::core::ModPlatform::kModrinth;
    resolved.version.project_id = project_id;
    resolved.version.version_id = project_id + "-v1";
    resolved.version.downloadable = downloadable;
    sscl::core::ModFile file;
    file.filename = file_name;
    file.url = "https://cdn.modrinth.com/data/" + project_id + "/" + file_name;
    file.sha1 = sha1;
    file.size = 3;
    file.primary = true;
    resolved.version.files.push_back(file);
    resolved.role = sscl::core::ModPlanRole::kUser;
    return resolved;
}

}  // namespace

TEST_CASE("mod.plan: 新装/同名同哈希/同名冲突") {
    const fs::path root = make_root("plan");
    const fs::path mods = root / L"mods";
    std::error_code ec;
    fs::create_directories(mods, ec);

    sscl::core::InstanceTarget target;
    target.mods_dir = mods;
    target.run_dir = root;

    sscl::core::ModHashCache cache(root / L"cache" / L"hashes.json");
    sscl::core::ModInstallOptions options;

    auto fresh = sscl::core::build_install_plan(
        target, {make_resolved("sodium", "sodium.jar")}, options, cache);
    REQUIRE(fresh.items.size() == 1);
    REQUIRE(fresh.install_count == 1);
    REQUIRE(fresh.items[0].action == sscl::core::ModPlanAction::kInstall);
    REQUIRE(fresh.items[0].file_name == "sodium.jar");

    REQUIRE(sscl::platform::write_file(mods / L"sodium.jar", "abc").is_ok());
    const std::string same_hash =
        sscl::util::hash_hex(sscl::util::HashAlgorithm::kSha1, "abc");
    auto same = sscl::core::build_install_plan(
        target, {make_resolved("sodium", "sodium.jar", same_hash)}, options, cache);
    REQUIRE(same.skip_count == 1);
    REQUIRE(same.items[0].action == sscl::core::ModPlanAction::kSkipSame);

    auto different = sscl::core::build_install_plan(
        target, {make_resolved("sodium", "sodium.jar", "different")}, options, cache);
    REQUIRE(different.conflict_count == 1);
    REQUIRE(different.items[0].action == sscl::core::ModPlanAction::kConflict);

    cleanup(root);
}

TEST_CASE("mod.plan: disabled 同名视为冲突") {
    const fs::path root = make_root("disabled");
    const fs::path mods = root / L"mods";
    std::error_code ec;
    fs::create_directories(mods, ec);
    REQUIRE(sscl::platform::write_file(mods / L"sodium.jar.disabled", "abc").is_ok());

    sscl::core::InstanceTarget target;
    target.mods_dir = mods;
    sscl::core::ModHashCache cache(root / L"cache" / L"hashes.json");
    auto plan = sscl::core::build_install_plan(
        target, {make_resolved("sodium", "sodium.jar")}, {}, cache);
    REQUIRE(plan.conflict_count == 1);
    REQUIRE(plan.items[0].action == sscl::core::ModPlanAction::kConflict);
    REQUIRE(plan.items[0].note.find(".disabled") != std::string::npos);

    cleanup(root);
}

TEST_CASE("mod.plan: 计划内同名与可选依赖/禁止分发") {
    const fs::path root = make_root("dupe");
    const fs::path mods = root / L"mods";
    std::error_code ec;
    fs::create_directories(mods, ec);

    sscl::core::InstanceTarget target;
    target.mods_dir = mods;
    sscl::core::ModHashCache cache(root / L"cache" / L"hashes.json");

    auto duplicate = sscl::core::build_install_plan(
        target,
        {make_resolved("a", "same.jar"), make_resolved("b", "same.jar")},
        {}, cache);
    REQUIRE(duplicate.install_count == 1);
    REQUIRE(duplicate.conflict_count == 1);
    REQUIRE(duplicate.items[1].note.find("同名") != std::string::npos);

    auto optional = make_resolved("optional", "optional.jar");
    optional.role = sscl::core::ModPlanRole::kOptional;
    auto with_optional = sscl::core::build_install_plan(
        target, {make_resolved("root", "root.jar"), optional}, {}, cache);
    REQUIRE(with_optional.install_count == 1);
    REQUIRE(with_optional.notices.size() == 1);
    REQUIRE(with_optional.notices[0].role == sscl::core::ModPlanRole::kOptional);

    auto blocked = sscl::core::build_install_plan(
        target, {make_resolved("blocked", "blocked.jar", "abc", false)}, {}, cache);
    REQUIRE(blocked.unavailable_count == 1);
    REQUIRE(blocked.items[0].action == sscl::core::ModPlanAction::kUnavailable);

    cleanup(root);
}

TEST_CASE("mod.install: 未确认冲突直接失败，空计划成功") {
    const fs::path root = make_root("guard");
    const fs::path mods = root / L"mods";
    std::error_code ec;
    fs::create_directories(mods, ec);
    REQUIRE(sscl::platform::write_file(mods / L"sodium.jar", "old").is_ok());

    sscl::core::InstanceTarget target;
    target.mods_dir = mods;
    sscl::core::ModHashCache cache(root / L"cache" / L"hashes.json");
    auto plan = sscl::core::build_install_plan(
        target, {make_resolved("sodium", "sodium.jar", "different")}, {}, cache);
    REQUIRE(plan.has_conflicts());

    sscl::core::DownloadScheduler scheduler;
    sscl::core::ModInstallOptions options;
    auto failed = sscl::core::install_mod_plan(plan, options, scheduler, cache,
                                               root / L"tmp", nullptr, {});
    REQUIRE(failed.is_error());
    REQUIRE(failed.error().code == "mod.conflict");

    sscl::core::ModInstallPlan empty;
    auto ok = sscl::core::install_mod_plan(empty, options, scheduler, cache, root / L"tmp",
                                           nullptr, {});
    REQUIRE(ok.is_ok());
    cleanup(root);
}
