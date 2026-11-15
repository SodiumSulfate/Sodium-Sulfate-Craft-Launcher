#include "core/account/account.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdio>

#include "core/account/microsoft_auth.hpp"
#include "core/config/build_config.hpp"
#include "util/hash.hpp"

#pragma comment(lib, "bcrypt.lib")

namespace sscl::core {

namespace {

std::string to_hex(const unsigned char* data, std::size_t size) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0F]);
    }
    return out;
}

}  // namespace

std::string account_type_name(AccountType type) {
    switch (type) {
        case AccountType::kOffline:
            return "offline";
        case AccountType::kAuthlibInjector:
            return "authlib_injector";
        case AccountType::kNide8:
            return "nide8";
        case AccountType::kMicrosoft:
            return "microsoft";
    }
    return "offline";
}

std::string offline_uuid_compact(const std::string& player_name) {
    // Java: UUID.nameUUIDFromBytes(UTF8("OfflinePlayer:" + name)) —— MD5 v3
    const std::string input = "OfflinePlayer:" + player_name;
    const std::string hex = util::hash_hex(util::HashAlgorithm::kMd5, input);
    if (hex.size() != 32) {
        return {};
    }
    // hex → 16 字节（手写解析，避免 /sdl 下 sscanf 的 C4996）
    auto hex_nibble = [](char c) -> unsigned char {
        if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
        return static_cast<unsigned char>(c - 'A' + 10);
    };
    std::array<unsigned char, 16> bytes{};
    for (std::size_t i = 0; i < 16; ++i) {
        bytes[i] = static_cast<unsigned char>((hex_nibble(hex[i * 2]) << 4) |
                                              hex_nibble(hex[i * 2 + 1]));
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x30);  // version 3
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);  // RFC4122 variant
    return to_hex(bytes.data(), bytes.size());
}

std::string random_uuid_compact() {
    std::array<unsigned char, 16> bytes{};
    const NTSTATUS status =
        ::BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        return {};
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);  // version 4
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);  // RFC4122 variant
    return to_hex(bytes.data(), bytes.size());
}

std::string OfflineAccount::type_name() const {
    return account_type_name(AccountType::kOffline);
}

util::Result<AccountInfo> OfflineAccount::login(const std::string& username) {
    if (username.empty()) {
        return util::Result<AccountInfo>::fail(
            util::Error::make("account.empty_username", "用户名不能为空", {}));
    }
    AccountInfo account;
    account.type = AccountType::kOffline;
    account.username = username;
    account.uuid_compact = offline_uuid_compact(username);
    account.access_token = random_uuid_compact();  // knowledge §1：随机 UUID，无需持久化
    account.user_type = "msa";                     // OPEN_QUESTIONS #16
    account.user_properties = "{}";
    return util::Result<AccountInfo>::ok(std::move(account));
}

std::string MicrosoftAccount::type_name() const {
    return account_type_name(AccountType::kMicrosoft);
}

util::Result<AccountInfo> MicrosoftAccount::login(const std::string& refresh_token) {
    if (refresh_token.empty()) {
        return util::Result<AccountInfo>::fail(
            util::Error::make("account.no_refresh_token", "缺少登录凭据，请重新登录微软账号", {}));
    }
    const std::string client_id(microsoft_client_id());
    if (client_id.empty()) {
        return util::Result<AccountInfo>::fail(
            util::Error::make("account.no_client_id", "微软登录未配置应用 ID", {}));
    }
    util::Result<MinecraftSession> session = msa_login_with_refresh(client_id, refresh_token);
    if (session.is_error()) {
        return util::Result<AccountInfo>::fail(std::move(session.error()));
    }
    AccountInfo account;
    account.type = AccountType::kMicrosoft;
    account.username = session.value().username;
    account.uuid_compact = session.value().uuid_compact;
    account.access_token = session.value().access_token;
    account.user_type = "msa";
    account.user_properties = "{}";
    account.refresh_token = session.value().refresh_token;
    account.expires_at_utc = session.value().expires_at_utc;
    account.profile_json = session.value().profile_json;
    return util::Result<AccountInfo>::ok(std::move(account));
}

}  // namespace sscl::core
