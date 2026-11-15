#pragma once

// 账号体系（开发文档 §7.2 + knowledge/offline-account.md）。
// 微软登录暂缓（仅类型预留）；离线账号 UUID = MD5 v3("OfflinePlayer:"+name)。
// IAccountProvider 保留 authlib-injector 注入参数能力（extra_*_arguments）。

#include <cstdint>
#include <string>
#include <vector>

#include "util/result.hpp"

namespace sscl::core {

enum class AccountType {
    kOffline,
    kAuthlibInjector,
    kNide8,
    kMicrosoft,  // 预留（§7.2 暂缓）
};

struct AccountInfo {
    AccountType type = AccountType::kOffline;
    std::string username;
    std::string uuid_compact;   // 32 位无连字符
    std::string access_token;
    std::string user_type = "msa";        // OPEN_QUESTIONS #16：跟随 HMCL
    std::string user_properties = "{}";
    std::string server_url;               // 外置登录 / Nide8

    // 微软账号专属（其他类型忽略）：refresh_token 换新登录、expires_at_utc 预判过期。
    std::string refresh_token;
    std::int64_t expires_at_utc = 0;      // MC access_token 过期（unix 秒，提前 1h 策略后）
    std::string profile_json;             // skins/capes 原文（皮肤显示，C-6）

    // 外置登录（authlib-injector）专属：续期时回传的客户端令牌。
    std::string client_token;
};

std::string account_type_name(AccountType type);

// 离线 UUID（Java nameUUIDFromBytes 等价）：MD5 v3，紧凑 hex。
std::string offline_uuid_compact(const std::string& player_name);

// 随机 UUID v4 紧凑 hex（用于离线 accessToken，knowledge §1）。
std::string random_uuid_compact();

class IAccountProvider {
public:
    virtual ~IAccountProvider() = default;

    virtual std::string type_name() const = 0;

    // 登录/生成账号信息（离线 = 纯本地派生）。
    virtual util::Result<AccountInfo> login(const std::string& username) = 0;

    // 账号特有的附加 JVM 参数（authlib-injector 的 -javaagent 预留位，§2）。
    virtual std::vector<std::string> extra_jvm_arguments(const AccountInfo& account) const {
        (void)account;
        return {};
    }

    virtual std::vector<std::string> extra_game_arguments(const AccountInfo& account) const {
        (void)account;
        return {};
    }
};

class OfflineAccount final : public IAccountProvider {
public:
    std::string type_name() const override;
    util::Result<AccountInfo> login(const std::string& username) override;
};

// 微软账号（§7.2）：交互式登录在 UI 层（设备码/浏览器），本 provider 只负责
// 非交互部分——用已存 refresh_token 静默续登（msa_login_with_refresh）。
class MicrosoftAccount final : public IAccountProvider {
public:
    std::string type_name() const override;
    // username 参数复用为 refresh_token（IAccountProvider 接口保持统一）。
    util::Result<AccountInfo> login(const std::string& refresh_token) override;
};

}  // namespace sscl::core
