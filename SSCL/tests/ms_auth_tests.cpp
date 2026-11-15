#include <doctest/doctest.h>

#include <filesystem>

#include "core/account/account.hpp"
#include "core/account/account_manager.hpp"
#include "core/account/account_store.hpp"
#include "core/account/microsoft_auth.hpp"
#include "core/launch/launch_command.hpp"
#include "platform/path_utils.hpp"

namespace core = sscl::core;

TEST_CASE("msa.pkce: verifier/challenge 形态（RFC 7636）") {
    const core::PkcePair pair = core::msa_generate_pkce();
    REQUIRE(pair.verifier.size() >= 43);
    REQUIRE(pair.verifier.size() <= 128);
    REQUIRE(pair.challenge.size() == 43);  // base64url(32 字节 SHA256) 无填充
    CHECK(pair.verifier != pair.challenge);
    // 仅 base64url 字符
    for (const char ch : pair.challenge) {
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        CHECK(ok);
    }
    // 每次生成应不同
    CHECK(core::msa_generate_pkce().verifier != pair.verifier);
}

TEST_CASE("msa.authorize_url: 参数完整且转义正确") {
    const std::string url =
        core::msa_authorize_url("00000000-0000-0000-0000-000000000001", "CHALLENGE",
                                "http://localhost:23380/auth-response", "STATE123");
    CHECK(url.find("client_id=00000000-0000-0000-0000-000000000001") != std::string::npos);
    CHECK(url.find("response_type=code") != std::string::npos);
    CHECK(url.find("redirect_uri=http%3A%2F%2Flocalhost%3A23380%2Fauth-response") !=
          std::string::npos);
    CHECK(url.find("code_challenge=CHALLENGE") != std::string::npos);
    CHECK(url.find("code_challenge_method=S256") != std::string::npos);
    CHECK(url.find("state=STATE123") != std::string::npos);
    CHECK(url.find("scope=XboxLive.signin%20offline_access") != std::string::npos);
}

TEST_CASE("account.store: 微软字段持久化往返") {
    const std::filesystem::path file =
        sscl::platform::data_dir() / L"unittest_tmp" / L"accounts-ms-test.dat";
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::remove(file, ec);

    core::AccountInfo account;
    account.type = core::AccountType::kMicrosoft;
    account.username = "TestPlayer";
    account.uuid_compact = "0123456789abcdef0123456789abcdef";
    account.access_token = "mc-token";
    account.refresh_token = "refresh-token-xyz";
    account.expires_at_utc = 1900000000;
    account.profile_json = R"({"id":"0123","skins":[]})";

    core::AccountStore store(file);
    REQUIRE(store.save({account}).is_ok());
    auto loaded = store.load();
    REQUIRE(loaded.is_ok());
    REQUIRE(loaded.value().size() == 1);
    const core::AccountInfo& out = loaded.value()[0];
    CHECK(out.type == core::AccountType::kMicrosoft);
    CHECK(out.username == "TestPlayer");
    CHECK(out.refresh_token == "refresh-token-xyz");
    CHECK(out.expires_at_utc == 1900000000);
    CHECK(out.profile_json == account.profile_json);

    std::filesystem::remove(file, ec);
}

TEST_CASE("account.manager: 增删选与覆盖") {
    const std::filesystem::path file =
        sscl::platform::data_dir() / L"unittest_tmp" / L"accounts-mgr-test.dat";
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::remove(file, ec);

    core::AccountManager manager(file);
    REQUIRE(manager.load().is_ok());
    CHECK(!manager.selected().has_value());

    auto first = core::OfflineAccount().login("Alice");
    REQUIRE(first.is_ok());
    REQUIRE(manager.add_and_select(first.value()).is_ok());
    REQUIRE(manager.selected().has_value());
    CHECK(manager.selected()->username == "Alice");

    auto second = core::OfflineAccount().login("Bob");
    REQUIRE(second.is_ok());
    REQUIRE(manager.add_and_select(second.value()).is_ok());
    CHECK(manager.selected()->username == "Bob");
    CHECK(manager.accounts().size() == 2);

    // 选回第一项
    manager.select_uuid(first.value().uuid_compact);
    CHECK(manager.selected()->username == "Alice");

    // 同 uuid 覆盖（不追加）
    core::AccountInfo renamed = first.value();
    renamed.username = "Alice2";
    REQUIRE(manager.add_and_select(renamed).is_ok());
    CHECK(manager.accounts().size() == 2);
    CHECK(manager.selected()->username == "Alice2");

    // 删除选中 → 回退首项
    REQUIRE(manager.remove(renamed.uuid_compact).is_ok());
    CHECK(manager.accounts().size() == 1);
    CHECK(manager.selected()->username == "Bob");

    // 持久化重载
    core::AccountManager reloaded(file);
    REQUIRE(reloaded.load().is_ok());
    REQUIRE(reloaded.accounts().size() == 1);
    CHECK(reloaded.accounts()[0].username == "Bob");

    std::filesystem::remove(file, ec);
}

TEST_CASE("launch.placeholders: ${clientid}/${auth_xuid} 替换（C-7）") {
    core::VersionJson version;
    version.id = "1.20.1";
    version.type = "release";
    core::LaunchContext context;
    context.client_id = "00000000-0000-0000-0000-000000000001";
    context.auth_xuid = "0";
    const std::map<std::string, std::string> values =
        core::build_placeholder_map(version, context, "cp.jar");
    CHECK(values.at("${clientid}") == "00000000-0000-0000-0000-000000000001");
    CHECK(values.at("${auth_xuid}") == "0");
    CHECK(values.at("${user_type}") == "msa");
}
