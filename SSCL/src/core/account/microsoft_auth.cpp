#include "core/account/microsoft_auth.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <ctime>
#include <random>

#include "platform/http_client.hpp"
#include "util/hash.hpp"
#include "util/log.hpp"

namespace sscl::core {

namespace {

using nlohmann::json;
using platform::HttpRequest;
using platform::HttpResponse;

constexpr const char* kDeviceCodeUrl =
    "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode";
constexpr const char* kTokenUrl =
    "https://login.microsoftonline.com/consumers/oauth2/v2.0/token";
constexpr const char* kAuthorizeUrl =
    "https://login.microsoftonline.com/consumers/oauth2/v2.0/authorize";
constexpr const char* kXblUrl = "https://user.auth.xboxlive.com/user/authenticate";
constexpr const char* kXstsUrl = "https://xsts.auth.xboxlive.com/xsts/authorize";
constexpr const char* kLoginWithXboxUrl =
    "https://api.minecraftservices.com/authentication/login_with_xbox";
constexpr const char* kEntitlementsUrl =
    "https://api.minecraftservices.com/entitlements/mcstore";
constexpr const char* kProfileUrl = "https://api.minecraftservices.com/minecraft/profile";

constexpr const char* kScope = "XboxLive.signin offline_access";

// 现在的 unix 秒（系统时钟；PCL 同款口径）。
std::int64_t now_unix() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

// application/x-www-form-urlencoded 编码。
std::string form_url_encode(const std::string& value) {
    std::string out;
    out.reserve(value.size() * 3);
    for (const unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out.push_back(static_cast<char>(ch));
        } else {
            static constexpr char kHex[] = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(kHex[ch >> 4]);
            out.push_back(kHex[ch & 0x0F]);
        }
    }
    return out;
}

std::string form_build(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out;
    for (const auto& [key, value] : fields) {
        if (!out.empty()) {
            out.push_back('&');
        }
        out += form_url_encode(key);
        out.push_back('=');
        out += form_url_encode(value);
    }
    return out;
}

util::Result<HttpResponse> post_form(const char* url,
                                     const std::vector<std::pair<std::string, std::string>>& fields) {
    HttpRequest request;
    request.method = "POST";
    request.url = url;
    request.body = form_build(fields);
    request.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
    request.headers.emplace_back("Accept", "application/json");
    return platform::http_request(request);
}

util::Result<HttpResponse> post_json(const char* url, const json& payload) {
    HttpRequest request;
    request.method = "POST";
    request.url = url;
    request.body = payload.dump();
    request.headers.emplace_back("Content-Type", "application/json");
    request.headers.emplace_back("Accept", "application/json");
    return platform::http_request(request);
}

util::Result<HttpResponse> get_bearer(const char* url, const std::string& token) {
    HttpRequest request;
    request.method = "GET";
    request.url = url;
    request.headers.emplace_back("Authorization", "Bearer " + token);
    request.headers.emplace_back("Accept", "application/json");
    return platform::http_request(request);
}

json parse_json(const std::string& body) {
    json node = json::parse(body, nullptr, false);
    return node.is_discarded() ? json::object() : node;
}

// OAuth 错误 → 中文提示（knowledge/microsoft-auth.md §2）。
util::Error oauth_error(const std::string& response_body) {
    if (response_body.find("authorization_pending") != std::string::npos) {
        return util::Error::make("msa.pending", "等待授权中", {});
    }
    if (response_body.find("slow_down") != std::string::npos) {
        return util::Error::make("msa.slow_down", "请求过快，放慢轮询", {});
    }
    if (response_body.find("expired_token") != std::string::npos) {
        return util::Error::make("msa.expired", "登录用时太长，请重新登录", {});
    }
    if (response_body.find("authorization_declined") != std::string::npos) {
        return util::Error::make("msa.declined", "你拒绝了登录授权", {});
    }
    if (response_body.find("Account security interrupt") != std::string::npos) {
        return util::Error::make("msa.security_interrupt", "账号存在安全问题，请前往微软账户页处理后再登录",
                                 {});
    }
    if (response_body.find("service abuse") != std::string::npos) {
        return util::Error::make("msa.abuse", "该账号已被微软封禁，无法登录", {});
    }
    if (response_body.find("AADSTS70000") != std::string::npos ||
        response_body.find("invalid_grant") != std::string::npos ||
        response_body.find("must sign in again") != std::string::npos ||
        response_body.find("password expired") != std::string::npos ||
        response_body.find("is not valid") != std::string::npos) {
        return util::Error::make("msa.relogin", "登录已失效，需要重新登录", {});
    }
    return util::Error::make("msa.oauth_failed", "微软登录失败", response_body);
}

// XSTS XErr → 中文提示（对照 PCL/HMCL 错误码表）。
util::Error xsts_error(const std::string& response_body) {
    const json node = parse_json(response_body);
    const long long xerr = node.value("XErr", 0LL);
    switch (xerr) {
        case 2148916227:
            return util::Error::make("msa.xsts.banned", "该账号已被微软封禁，无法登录", response_body);
        case 2148916233:
            return util::Error::make("msa.xsts.no_xbox",
                                     "该账号尚未注册 Xbox 账户，请先注册后再登录", response_body);
        case 2148916235:
            return util::Error::make("msa.xsts.country",
                                     "当前网络所在国家或地区无法登录微软账号，请使用加速器或 VPN 后重试",
                                     response_body);
        case 2148916238:
            return util::Error::make("msa.xsts.child",
                                     "该账号为未成年账号，需要先在微软账户页修改出生日期（≥18 岁）后再登录",
                                     response_body);
        default:
            return util::Error::make("msa.xsts.failed", "Xbox 授权失败", response_body);
    }
}

// 简单 base64url（无填充），用于 PKCE。
std::string base64url(const unsigned char* data, std::size_t size) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3) {
        const unsigned int b0 = data[i];
        const unsigned int b1 = i + 1 < size ? data[i + 1] : 0;
        const unsigned int b2 = i + 2 < size ? data[i + 2] : 0;
        const unsigned int triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kTable[(triple >> 18) & 0x3F]);
        out.push_back(kTable[(triple >> 12) & 0x3F]);
        if (i + 1 < size) out.push_back(kTable[(triple >> 6) & 0x3F]);
        if (i + 2 < size) out.push_back(kTable[triple & 0x3F]);
    }
    return out;
}

}  // namespace

// ── 设备码流程 ──────────────────────────────────────────────────────

util::Result<DeviceCodeInfo> msa_request_device_code(const std::string& client_id) {
    util::Result<HttpResponse> response =
        post_form(kDeviceCodeUrl, {{"client_id", client_id}, {"scope", kScope}});
    if (response.is_error()) {
        return util::Result<DeviceCodeInfo>::fail(std::move(response.error()));
    }
    if (response.value().status_code != 200) {
        return util::Result<DeviceCodeInfo>::fail(
            oauth_error(response.value().body));
    }
    const json node = parse_json(response.value().body);
    if (!node.contains("device_code") || !node.contains("user_code")) {
        return util::Result<DeviceCodeInfo>::fail(
            util::Error::make("msa.bad_response", "微软登录响应格式错误", response.value().body));
    }
    DeviceCodeInfo info;
    info.user_code = node.value("user_code", "");
    info.device_code = node.value("device_code", "");
    info.verification_uri = node.value("verification_uri", "https://microsoft.com/link");
    info.interval_seconds = std::max(1, node.value("interval", 5));
    info.expires_in_seconds = std::max(60, node.value("expires_in", 900));
    return util::Result<DeviceCodeInfo>::ok(std::move(info));
}

util::Result<MsOAuthTokens> msa_poll_device_token(const std::string& client_id,
                                                  const std::string& device_code) {
    util::Result<HttpResponse> response = post_form(
        kTokenUrl, {{"grant_type", "urn:ietf:params:oauth:grant-type:device_code"},
                    {"client_id", client_id},
                    {"device_code", device_code},
                    {"scope", kScope}});
    if (response.is_error()) {
        return util::Result<MsOAuthTokens>::fail(std::move(response.error()));
    }
    if (response.value().status_code != 200) {
        return util::Result<MsOAuthTokens>::fail(oauth_error(response.value().body));
    }
    const json node = parse_json(response.value().body);
    if (!node.contains("access_token") || !node.contains("refresh_token")) {
        return util::Result<MsOAuthTokens>::fail(
            util::Error::make("msa.bad_response", "微软登录响应格式错误", response.value().body));
    }
    MsOAuthTokens tokens;
    tokens.access_token = node.value("access_token", "");
    tokens.refresh_token = node.value("refresh_token", "");
    tokens.expires_at_utc = now_unix() + std::max<int>(60, node.value("expires_in", 3600));
    return util::Result<MsOAuthTokens>::ok(std::move(tokens));
}

// ── 浏览器授权码流程（PKCE）────────────────────────────────────────

PkcePair msa_generate_pkce() {
    std::array<unsigned char, 64> raw{};
    std::random_device rd;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        raw[i] = static_cast<unsigned char>(rd());
    }
    PkcePair pair;
    pair.verifier = base64url(raw.data(), raw.size());
    // SHA-256 摘要取原始字节再 base64url（hash_hex 返回 hex 文本，手动转回字节）。
    const std::string hex = util::hash_hex(util::HashAlgorithm::kSha256, pair.verifier);
    std::array<unsigned char, 32> digest{};
    for (std::size_t i = 0; i < digest.size(); ++i) {
        const char hi = hex[i * 2];
        const char lo = hex[i * 2 + 1];
        auto nib = [](char c) -> unsigned char {
            return c >= '0' && c <= '9' ? static_cast<unsigned char>(c - '0')
                                        : static_cast<unsigned char>(c - 'a' + 10);
        };
        digest[i] = static_cast<unsigned char>((nib(hi) << 4) | nib(lo));
    }
    pair.challenge = base64url(digest.data(), digest.size());
    return pair;
}

std::string msa_authorize_url(const std::string& client_id, const std::string& challenge,
                              const std::string& redirect_uri, const std::string& state) {
    std::string url = kAuthorizeUrl;
    url += "?client_id=" + form_url_encode(client_id);
    url += "&response_type=code";
    url += "&redirect_uri=" + form_url_encode(redirect_uri);
    url += "&scope=" + form_url_encode(kScope);
    url += "&state=" + form_url_encode(state);
    url += "&code_challenge=" + form_url_encode(challenge);
    url += "&code_challenge_method=S256";
    return url;
}

util::Result<MsOAuthTokens> msa_exchange_code(const std::string& client_id,
                                              const std::string& code,
                                              const std::string& verifier,
                                              const std::string& redirect_uri) {
    util::Result<HttpResponse> response =
        post_form(kTokenUrl, {{"grant_type", "authorization_code"},
                              {"client_id", client_id},
                              {"code", code},
                              {"code_verifier", verifier},
                              {"redirect_uri", redirect_uri},
                              {"scope", kScope}});
    if (response.is_error()) {
        return util::Result<MsOAuthTokens>::fail(std::move(response.error()));
    }
    if (response.value().status_code != 200) {
        return util::Result<MsOAuthTokens>::fail(oauth_error(response.value().body));
    }
    const json node = parse_json(response.value().body);
    if (!node.contains("access_token") || !node.contains("refresh_token")) {
        return util::Result<MsOAuthTokens>::fail(
            util::Error::make("msa.bad_response", "微软登录响应格式错误", response.value().body));
    }
    MsOAuthTokens tokens;
    tokens.access_token = node.value("access_token", "");
    tokens.refresh_token = node.value("refresh_token", "");
    tokens.expires_at_utc = now_unix() + std::max<int>(60, node.value("expires_in", 3600));
    return util::Result<MsOAuthTokens>::ok(std::move(tokens));
}

// ── 刷新 ────────────────────────────────────────────────────────────

util::Result<MsOAuthTokens> msa_refresh(const std::string& client_id,
                                        const std::string& refresh_token) {
    util::Result<HttpResponse> response =
        post_form(kTokenUrl, {{"grant_type", "refresh_token"},
                              {"client_id", client_id},
                              {"refresh_token", refresh_token},
                              {"scope", kScope}});
    if (response.is_error()) {
        return util::Result<MsOAuthTokens>::fail(std::move(response.error()));
    }
    if (response.value().status_code != 200) {
        return util::Result<MsOAuthTokens>::fail(oauth_error(response.value().body));
    }
    const json node = parse_json(response.value().body);
    if (!node.contains("access_token")) {
        return util::Result<MsOAuthTokens>::fail(
            util::Error::make("msa.bad_response", "微软登录响应格式错误", response.value().body));
    }
    MsOAuthTokens tokens;
    tokens.access_token = node.value("access_token", "");
    // 微软可能轮换 refresh token；缺省沿用旧值。
    tokens.refresh_token = node.value("refresh_token", refresh_token);
    tokens.expires_at_utc = now_unix() + std::max<int>(60, node.value("expires_in", 3600));
    return util::Result<MsOAuthTokens>::ok(std::move(tokens));
}

// ── 六步链路（步骤 2-6）────────────────────────────────────────────

util::Result<MinecraftSession> msa_minecraft_login(
    const MsOAuthTokens& oauth,
    const std::function<void(const std::string& stage)>& on_stage) {
    const auto stage = [&on_stage](const char* text) {
        if (on_stage) {
            on_stage(text);
        }
    };
    // 步 2：XBL
    stage("正在登录 Xbox Live…");
    json xbl_body = {
        {"Properties",
         {{"AuthMethod", "RPS"},
          {"SiteName", "user.auth.xboxlive.com"},
          {"RpsTicket", "d=" + oauth.access_token}}},
        {"RelyingParty", "http://auth.xboxlive.com"},
        {"TokenType", "JWT"},
    };
    util::Result<HttpResponse> xbl = post_json(kXblUrl, xbl_body);
    if (xbl.is_error()) {
        return util::Result<MinecraftSession>::fail(std::move(xbl.error()));
    }
    if (xbl.value().status_code != 200) {
        return util::Result<MinecraftSession>::fail(util::Error::make(
            "msa.xbl.failed", "Xbox Live 登录失败", xbl.value().body));
    }
    const json xbl_node = parse_json(xbl.value().body);
    const std::string xbl_token = xbl_node.value("Token", "");
    const json xui = xbl_node.value("DisplayClaims", json::object()).value("xui", json::array());
    if (xbl_token.empty() || xui.empty() || !xui[0].contains("uhs")) {
        return util::Result<MinecraftSession>::fail(util::Error::make(
            "msa.xbl.bad_response", "Xbox Live 登录响应格式错误", xbl.value().body));
    }
    const std::string uhs = xui[0].value("uhs", "");

    // 步 3：XSTS
    stage("正在获取 Xbox 授权…");
    json xsts_body = {
        {"Properties", {{"SandboxId", "RETAIL"}, {"UserTokens", json::array({xbl_token})}}},
        {"RelyingParty", "rp://api.minecraftservices.com/"},
        {"TokenType", "JWT"},
    };
    util::Result<HttpResponse> xsts = post_json(kXstsUrl, xsts_body);
    if (xsts.is_error()) {
        return util::Result<MinecraftSession>::fail(std::move(xsts.error()));
    }
    if (xsts.value().status_code != 200) {
        return util::Result<MinecraftSession>::fail(xsts_error(xsts.value().body));
    }
    const json xsts_node = parse_json(xsts.value().body);
    const std::string xsts_token = xsts_node.value("Token", "");
    if (xsts_token.empty()) {
        return util::Result<MinecraftSession>::fail(util::Error::make(
            "ma.xsts.bad_response", "Xbox 授权响应格式错误", xsts.value().body));
    }

    // 步 4：Minecraft token
    stage("正在登录 Minecraft…");
    json login_body = {{"identityToken", "XBL3.0 x=" + uhs + ";" + xsts_token}};
    util::Result<HttpResponse> mc = post_json(kLoginWithXboxUrl, login_body);
    if (mc.is_error()) {
        return util::Result<MinecraftSession>::fail(std::move(mc.error()));
    }
    if (mc.value().status_code != 200) {
        const std::string& body = mc.value().body;
        if (mc.value().status_code == 429) {
            return util::Result<MinecraftSession>::fail(
                util::Error::make("msa.rate_limited", "登录尝试过于频繁，请几分钟后再试", body));
        }
        if (mc.value().status_code == 403) {
            return util::Result<MinecraftSession>::fail(util::Error::make(
                "msa.ip_forbidden",
                "当前 IP 的登录尝试异常，请关闭或更换 VPN/加速器节点后重试", body));
        }
        if (mc.value().status_code == 503) {
            return util::Result<MinecraftSession>::fail(
                util::Error::make("msa.mojang_down", "Mojang 服务器暂时故障，请稍后再试", body));
        }
        if (body.find("ACCOUNT_SUSPENDED") != std::string::npos) {
            return util::Result<MinecraftSession>::fail(
                util::Error::make("msa.suspended", "该账号已被微软封禁，无法登录", body));
        }
        return util::Result<MinecraftSession>::fail(
            util::Error::make("msa.login_failed", "Minecraft 登录失败", body));
    }
    const json mc_node = parse_json(mc.value().body);
    const std::string mc_token = mc_node.value("access_token", "");
    if (mc_token.empty()) {
        return util::Result<MinecraftSession>::fail(util::Error::make(
            "msa.bad_response", "Minecraft 登录响应格式错误", mc.value().body));
    }

    // 步 5：持有校验（未购买/XGP 过期 → items 为空）
    stage("正在验证正版资格…");
    util::Result<HttpResponse> entitlements = get_bearer(kEntitlementsUrl, mc_token);
    if (entitlements.is_error()) {
        return util::Result<MinecraftSession>::fail(std::move(entitlements.error()));
    }
    if (entitlements.value().status_code != 200) {
        return util::Result<MinecraftSession>::fail(util::Error::make(
            "msa.entitlements_failed", "无法验证正版资格", entitlements.value().body));
    }
    const json ent_node = parse_json(entitlements.value().body);
    const json items = ent_node.value("items", json::array());
    if (items.empty()) {
        return util::Result<MinecraftSession>::fail(util::Error::make(
            "msa.not_owned",
            "你尚未购买正版 Minecraft，或者 Xbox Game Pass 已到期", entitlements.value().body));
    }

    // 步 6：档案（UUID/名/皮肤）
    stage("正在获取玩家档案…");
    util::Result<HttpResponse> profile = get_bearer(kProfileUrl, mc_token);
    if (profile.is_error()) {
        return util::Result<MinecraftSession>::fail(std::move(profile.error()));
    }
    if (profile.value().status_code == 404) {
        return util::Result<MinecraftSession>::fail(util::Error::make(
            "msa.no_profile", "请先在 minecraft.net 创建 Minecraft 玩家档案，然后重新登录",
            profile.value().body));
    }
    if (profile.value().status_code == 429) {
        return util::Result<MinecraftSession>::fail(
            util::Error::make("msa.rate_limited", "登录尝试过于频繁，请几分钟后再试",
                              profile.value().body));
    }
    if (profile.value().status_code != 200) {
        return util::Result<MinecraftSession>::fail(util::Error::make(
            "msa.profile_failed", "获取玩家档案失败", profile.value().body));
    }
    const json profile_node = parse_json(profile.value().body);
    const std::string uuid = profile_node.value("id", "");
    const std::string name = profile_node.value("name", "");
    if (uuid.size() != 32 || name.empty()) {
        return util::Result<MinecraftSession>::fail(util::Error::make(
            "msa.bad_response", "玩家档案响应格式错误", profile.value().body));
    }

    MinecraftSession session;
    session.access_token = mc_token;
    session.refresh_token = oauth.refresh_token;
    // PCL 策略：提前 1 小时视作过期。
    session.expires_at_utc = now_unix() + std::max<int>(60, mc_node.value("expires_in", 86400)) - 3600;
    session.uuid_compact = uuid;
    session.username = name;
    session.profile_json = profile.value().body;
    return util::Result<MinecraftSession>::ok(std::move(session));
}

util::Result<MinecraftSession> msa_login_with_refresh(const std::string& client_id,
                                                       const std::string& refresh_token) {
    util::Result<MsOAuthTokens> oauth = msa_refresh(client_id, refresh_token);
    if (oauth.is_error()) {
        return util::Result<MinecraftSession>::fail(std::move(oauth.error()));
    }
    return msa_minecraft_login(oauth.value());
}

bool msa_validate(const std::string& mc_access_token) {
    util::Result<HttpResponse> response = get_bearer(kProfileUrl, mc_access_token);
    return response.is_ok() && response.value().status_code == 200;
}

}  // namespace sscl::core
