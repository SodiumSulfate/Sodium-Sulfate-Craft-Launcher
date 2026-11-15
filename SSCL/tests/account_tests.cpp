#include <doctest/doctest.h>

#include <filesystem>

#include "core/account/account.hpp"
#include "core/account/account_store.hpp"
#include "platform/path_utils.hpp"

namespace core = sscl::core;

TEST_CASE("account.uuid: 离线 UUID v3 已知向量") {
    // Java nameUUIDFromBytes(UTF8("OfflinePlayer:"+name))，与 MC 离线 UUID 公开值一致
    CHECK(core::offline_uuid_compact("Notch") == "b50ad385829d3141a2167e7d7539ba7f");
    CHECK(core::offline_uuid_compact("Steve") == "5627dd98e6be3c21b8a8e92344183641");
}

TEST_CASE("account.uuid: 随机 UUID v4 形态") {
    const std::string first = core::random_uuid_compact();
    const std::string second = core::random_uuid_compact();
    REQUIRE(first.size() == 32);
    REQUIRE(second.size() == 32);
    CHECK(first != second);
    CHECK(first[12] == '4');  // version 4
    CHECK((first[16] == '8' || first[16] == '9' || first[16] == 'a' || first[16] == 'b'));
}

TEST_CASE("account.offline: 登录派生字段") {
    core::OfflineAccount provider;
    CHECK(provider.type_name() == "offline");

    auto logged = provider.login("Notch");
    REQUIRE(logged.is_ok());
    const core::AccountInfo& account = logged.value();
    CHECK(account.username == "Notch");
    CHECK(account.uuid_compact == "b50ad385829d3141a2167e7d7539ba7f");
    CHECK(account.user_type == "msa");  // OPEN_QUESTIONS #16
    CHECK(account.user_properties == "{}");
    CHECK(account.access_token.size() == 32);
    CHECK(provider.extra_jvm_arguments(account).empty());  // 离线无附加参数

    CHECK(provider.login("").is_error());  // 空用户名报错
}

TEST_CASE("account.store: DPAPI 加密往返") {
    const std::filesystem::path file =
        sscl::platform::data_dir() / L"unittest_tmp" / L"accounts-test.dat";
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::remove(file, ec);

    core::AccountStore store(file);
    auto empty = store.load();
    REQUIRE(empty.is_ok());
    CHECK(empty.value().empty());  // 无文件 = 空列表

    std::vector<core::AccountInfo> accounts;
    auto logged = core::OfflineAccount().login("Steve");
    REQUIRE(logged.is_ok());
    accounts.push_back(logged.value());

    REQUIRE(store.save(accounts).is_ok());
    auto loaded = store.load();
    REQUIRE(loaded.is_ok());
    REQUIRE(loaded.value().size() == 1);
    CHECK(loaded.value()[0].username == "Steve");
    CHECK(loaded.value()[0].uuid_compact == accounts[0].uuid_compact);
    CHECK(loaded.value()[0].access_token == accounts[0].access_token);
    CHECK(loaded.value()[0].type == core::AccountType::kOffline);

    std::filesystem::remove(file, ec);
}
