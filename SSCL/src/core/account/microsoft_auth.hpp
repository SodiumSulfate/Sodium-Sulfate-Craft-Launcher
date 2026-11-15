#pragma once

// 微软正版登录（§7.2 + knowledge/microsoft-auth.md）：
// OAuth（设备码首选 / 浏览器授权码 PKCE 可选）→ XBL → XSTS →
// Minecraft Services（login_with_xbox → entitlements → profile）。
// 所有错误映射为中文 message（Error.code 前缀 msa.*）；核心层不抛异常。

#include <cstdint>
#include <functional>
#include <string>

#include "util/result.hpp"

namespace sscl::core {

// ── 数据结构 ─────────────────────────────────────────────────────────

struct DeviceCodeInfo {
    std::string user_code;          // 用户需在网页输入的代码
    std::string device_code;        // 轮询用
    std::string verification_uri;   // 验证网页
    int interval_seconds = 5;       // 轮询间隔
    int expires_in_seconds = 900;   // 有效期
};

struct MsOAuthTokens {
    std::string access_token;
    std::string refresh_token;
    std::int64_t expires_at_utc = 0;  // unix 秒（OAuth access token 过期时间）
};

struct MinecraftSession {
    std::string access_token;       // MC access token（启动参数用）
    std::string refresh_token;      // OAuth refresh token（续期/重登用）
    std::int64_t expires_at_utc = 0;  // MC token 过期（提前 1 小时视作过期，PCL 策略）
    std::string uuid_compact;       // 32 位无连字符
    std::string username;
    std::string profile_json;       // skins/capes 原文（皮肤显示用）
};

// ── 设备码流程（首选）────────────────────────────────────────────────

// 步 1a：申请设备码（POST .../oauth2/v2.0/devicecode）。
util::Result<DeviceCodeInfo> msa_request_device_code(const std::string& client_id);

// 轮询设备码令牌：
//   Ok(tokens)            —— 用户已完成授权
//   Error("msa.pending")  —— 仍在等待（继续轮询）
//   Error("msa.slow_down")—— 需放慢（interval + 5s 后继续）
//   其他错误码            —— 终止性失败（过期/拒绝/封禁等）
util::Result<MsOAuthTokens> msa_poll_device_token(const std::string& client_id,
                                                  const std::string& device_code);

// ── 浏览器授权码流程（PKCE，可选）──────────────────────────────────

struct PkcePair {
    std::string verifier;   // 43-128 字符 base64url
    std::string challenge;  // BASE64URL(SHA256(verifier))
};

PkcePair msa_generate_pkce();

// 构造授权页 URL（redirect_uri 指向本地回环服务，见 platform/oauth_loopback_server）。
std::string msa_authorize_url(const std::string& client_id, const std::string& challenge,
                              const std::string& redirect_uri, const std::string& state);

// 用授权码换令牌（POST .../oauth2/v2.0/token，grant_type=authorization_code）。
util::Result<MsOAuthTokens> msa_exchange_code(const std::string& client_id,
                                              const std::string& code,
                                              const std::string& verifier,
                                              const std::string& redirect_uri);

// ── 令牌刷新 ────────────────────────────────────────────────────────

// grant_type=refresh_token。失效（must sign in again / expired / AADSTS70000 等）
// 返回 Error("msa.relogin")，调用方应回退完整登录。
util::Result<MsOAuthTokens> msa_refresh(const std::string& client_id,
                                        const std::string& refresh_token);

// ── 六步链路（步骤 2-6）────────────────────────────────────────────

// OAuth 令牌 → XBL → XSTS → MC token → entitlements 校验 → profile。
// on_stage 供 UI 显示阶段进度（中文短句），可空。
util::Result<MinecraftSession> msa_minecraft_login(
    const MsOAuthTokens& oauth,
    const std::function<void(const std::string& stage)>& on_stage = {});

// 便捷入口：refresh_token → 刷新 OAuth → 完整链路。
util::Result<MinecraftSession> msa_login_with_refresh(const std::string& client_id,
                                                       const std::string& refresh_token);

// 启动前校验 MC token 是否仍可用（GET profile，200 = 有效）。
bool msa_validate(const std::string& mc_access_token);

}  // namespace sscl::core
