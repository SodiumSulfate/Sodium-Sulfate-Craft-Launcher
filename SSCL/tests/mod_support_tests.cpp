#include <chrono>
#include <filesystem>
#include <string>

#include <doctest/doctest.h>

#include "core/config/secret_store.hpp"
#include "core/mod/mod_scanner.hpp"
#include "core/modplatform/mod_catalog.hpp"
#include "platform/file_utils.hpp"
#include "util/hash.hpp"

namespace {

namespace fs = std::filesystem;

fs::path make_root(const char* tag) {
    return fs::temp_directory_path() /
           ("sscl_test_modsupport_" + std::string(tag) + "_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

void cleanup(const fs::path& root) {
    std::error_code ec;
    fs::remove_all(root, ec);
}

}  // namespace

TEST_CASE("secret: DPAPI 往返、删除与文件不含明文") {
    const fs::path root = make_root("secret");
    const fs::path file = root / L"secrets.dat";
    std::error_code ec;
    fs::create_directories(root, ec);

    auto missing = sscl::core::load_secret_value(file, "curseforge_api_key");
    REQUIRE(missing.is_ok());
    REQUIRE(missing.value().empty());

    REQUIRE(sscl::core::store_secret_value(file, "curseforge_api_key", "abc-123").is_ok());
    auto loaded = sscl::core::load_secret_value(file, "curseforge_api_key");
    REQUIRE(loaded.is_ok());
    REQUIRE(loaded.value() == "abc-123");

    auto raw = sscl::platform::read_file(file);
    REQUIRE(raw.is_ok());
    REQUIRE(raw.value().find("abc-123") == std::string::npos);

    REQUIRE(sscl::core::store_secret_value(file, "curseforge_api_key", "").is_ok());
    auto removed = sscl::core::load_secret_value(file, "curseforge_api_key");
    REQUIRE(removed.is_ok());
    REQUIRE(removed.value().empty());

    cleanup(root);
}

TEST_CASE("mod.catalog: 默认选项与平台可用性") {
    const fs::path root = make_root("catalog");
    auto options = sscl::core::make_mod_catalog_options(root, "key", true);
    REQUIRE(options.modrinth.platform == sscl::core::ModPlatform::kModrinth);
    REQUIRE(options.curseforge.platform == sscl::core::ModPlatform::kCurseForge);
    REQUIRE(options.curseforge.curseforge_api_key == "key");
    REQUIRE(options.modrinth.use_mcim);
    REQUIRE(options.modrinth.user_agent == "SSCL/0.1.0");
    const bool cache_dir_ok = options.modrinth.cache_dir == root / L"cache" / L"modplatform";
    REQUIRE(cache_dir_ok);

    sscl::core::ModCatalog catalog(options);
    REQUIRE(catalog.available(sscl::core::ModPlatform::kModrinth));
    REQUIRE(catalog.available(sscl::core::ModPlatform::kCurseForge));

    auto no_key = sscl::core::make_mod_catalog_options(root, "", false);
    sscl::core::ModCatalog blocked(no_key);
    std::string reason;
    REQUIRE_FALSE(blocked.available(sscl::core::ModPlatform::kCurseForge, &reason));
    REQUIRE_FALSE(reason.empty());
    REQUIRE(blocked.available(sscl::core::ModPlatform::kModrinth));
    cleanup(root);
}

TEST_CASE("mod.scanner: 列出 jar/disabled/old") {
    const fs::path root = make_root("scan");
    const fs::path mods = root / L"mods";
    std::error_code ec;
    fs::create_directories(mods, ec);
    REQUIRE(sscl::platform::write_file(mods / L"a.jar", "a").is_ok());
    REQUIRE(sscl::platform::write_file(mods / L"b.jar.disabled", "b").is_ok());
    REQUIRE(sscl::platform::write_file(mods / L"c.jar.old", "c").is_ok());
    REQUIRE(sscl::platform::write_file(mods / L"readme.txt", "x").is_ok());

    auto normal = sscl::core::list_local_mods(mods, false);
    REQUIRE(normal.size() == 2);
    REQUIRE(normal[0].file_name == "a.jar");
    REQUIRE_FALSE(normal[0].disabled);
    REQUIRE(normal[1].file_name == "b.jar.disabled");
    REQUIRE(normal[1].disabled);

    auto with_old = sscl::core::list_local_mods(mods, true);
    REQUIRE(with_old.size() == 3);
    cleanup(root);
}

TEST_CASE("mod.scanner: SHA1 缓存命中与文件变化失效") {
    const fs::path root = make_root("hash");
    const fs::path mods = root / L"mods";
    const fs::path file = mods / L"a.jar";
    std::error_code ec;
    fs::create_directories(mods, ec);
    REQUIRE(sscl::platform::write_file(file, "abc").is_ok());

    const std::string expected = sscl::util::hash_hex(sscl::util::HashAlgorithm::kSha1, "abc");
    sscl::core::ModHashCache cache(root / L"cache" / L"mod_hashes.json");
    REQUIRE(cache.sha1(file).is_ok());
    REQUIRE(cache.sha1(file).value() == expected);
    REQUIRE(cache.save().is_ok());

    sscl::core::ModHashCache reloaded(root / L"cache" / L"mod_hashes.json");
    REQUIRE(reloaded.sha1(file).value() == expected);

    REQUIRE(sscl::platform::write_file(file, "abcd").is_ok());
    auto changed = reloaded.sha1(file);
    REQUIRE(changed.is_ok());
    REQUIRE(changed.value() != expected);
    cleanup(root);
}


TEST_CASE("mod.scanner: PCL 同名启用优先/zip/启停") {
    const fs::path root = make_root("pclscan");
    const fs::path mods = root / L"mods";
    std::error_code ec;
    fs::create_directories(mods, ec);
    REQUIRE(sscl::platform::write_file(mods / L"a.jar", "enabled").is_ok());
    REQUIRE(sscl::platform::write_file(mods / L"a.jar.disabled", "disabled").is_ok());
    REQUIRE(sscl::platform::write_file(mods / L"b.zip.disabled", "b").is_ok());

    auto scan = sscl::core::scan_pcl_mods(mods);
    REQUIRE(scan.size() == 2);
    REQUIRE(scan[0].file_name == "a.jar");
    REQUIRE(scan[0].enabled);
    REQUIRE(scan[0].enabled_name == "a.jar");
    REQUIRE(scan[1].file_name == "b.zip.disabled");
    REQUIRE_FALSE(scan[1].enabled);
    REQUIRE(scan[1].enabled_name == "b.zip");

    REQUIRE(sscl::core::set_mod_enabled(mods / L"b.zip.disabled", true).is_ok());
    REQUIRE(fs::is_regular_file(mods / L"b.zip", ec));
    REQUIRE_FALSE(fs::exists(mods / L"b.zip.disabled", ec));
    REQUIRE(sscl::core::set_mod_enabled(mods / L"b.zip", false).is_ok());
    REQUIRE(fs::is_regular_file(mods / L"b.zip.disabled", ec));
    cleanup(root);
}

TEST_CASE("hash: CurseForge 指纹过滤空白且具有确定性") {
    REQUIRE(sscl::util::murmurhash2_filtered("abc") ==
            sscl::util::murmurhash2_filtered(" a\tb\nc\r "));
    REQUIRE(sscl::util::murmurhash2_filtered("abc") !=
            sscl::util::murmurhash2_filtered("abd"));
}

TEST_CASE("mod.scanner: PCL 导入语义") {
    const fs::path root = make_root("import");
    const fs::path mods = root / L"mods";
    const fs::path source = root / L"incoming" / L"example.jar.disabled";
    std::error_code ec;
    fs::create_directories(source.parent_path(), ec);
    REQUIRE(sscl::platform::write_file(source, "data").is_ok());

    auto imported = sscl::core::import_mod_files(mods, {source});
    REQUIRE(imported.is_ok());
    REQUIRE(imported.value() == 1);
    REQUIRE(fs::is_regular_file(mods / L"example.jar", ec));

    REQUIRE(sscl::core::import_mod_files(mods, {mods / L"example.jar"}).is_ok());
    cleanup(root);
}
