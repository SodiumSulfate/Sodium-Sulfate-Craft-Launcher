#include "core/account/account_skin.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/account/authlib_auth.hpp"
#include "platform/file_utils.hpp"
#include "platform/http_client.hpp"
#include "platform/path_utils.hpp"
#include "util/hash.hpp"
#include "util/log.hpp"

namespace sscl::core {

namespace {

using nlohmann::json;

constexpr std::size_t kMaxSkinBytes = 2 * 1024 * 1024;  // 皮肤 PNG 上限（PCL 更严：24 KB）

// PCL 用 PathTemp\Cache\Skin\IndexAuth.ini 持久化「UUID → 皮肤地址」。
// 这里两层缓存：进程内 5 分钟（主页账号卡与子窗口列表同时取同一账号时只发一次请求），
// 磁盘 24 小时（重启后无需再查 sessionserver，配合已有的 PNG 缓存可秒显头像）。
constexpr auto kUrlCacheTtl = std::chrono::minutes(5);
constexpr std::int64_t kUrlIndexTtlSeconds = 24 * 60 * 60;
struct CachedSkinUrl {
    std::chrono::steady_clock::time_point at;
    std::string url;
};
std::mutex g_url_mutex;
std::unordered_map<std::string, CachedSkinUrl> g_url_cache;

std::filesystem::path url_index_file() {
    return account_skin_cache_dir() / "urls.json";
}

std::string cached_skin_url(const std::string& key) {
    // 内存与磁盘索引共用一把锁：remember_skin_url 是「读-改-写」，并发时不能丢更新。
    std::lock_guard<std::mutex> lock(g_url_mutex);
    // ① 进程内缓存
    const auto found = g_url_cache.find(key);
    if (found != g_url_cache.end()) {
        if (std::chrono::steady_clock::now() - found->second.at <= kUrlCacheTtl) {
            return found->second.url;
        }
        g_url_cache.erase(found);
    }
    // ② 磁盘索引（重启后命中，避免启动期网络等待）
    util::Result<std::string> text = sscl::platform::read_file(url_index_file());
    if (text.is_error()) {
        return {};
    }
    const json root = json::parse(text.value(), nullptr, false);
    if (root.is_discarded() || !root.is_object() || !root.contains(key) ||
        !root[key].is_object()) {
        return {};
    }
    const json& node = root[key];
    const std::int64_t stamp = node.value("ts", static_cast<std::int64_t>(0));
    if (stamp <= 0 || std::time(nullptr) - stamp > kUrlIndexTtlSeconds) {
        return {};
    }
    return node.value("url", "");
}

void remember_skin_url(const std::string& key, const std::string& url) {
    std::lock_guard<std::mutex> lock(g_url_mutex);
    g_url_cache[key] = CachedSkinUrl{std::chrono::steady_clock::now(), url};
    // 磁盘索引：读-改-写一次性完成，后台线程调用，不阻塞 UI。
    const std::filesystem::path file = url_index_file();
    json root = json::object();
    if (util::Result<std::string> existing = sscl::platform::read_file(file);
        existing.is_ok()) {
        const json parsed = json::parse(existing.value(), nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object()) {
            root = parsed;
        }
    }
    json entry;
    entry["url"] = url;
    entry["ts"] = static_cast<std::int64_t>(std::time(nullptr));
    root[key] = std::move(entry);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (!ec) {
        util::Result<void> saved = sscl::platform::write_file_atomic(
            file, root.dump(), /*flush_to_disk=*/false);
        if (saved.is_error()) {
            sscl::util::log_warn("[skin] url index write failed: " + saved.error().detail);
        }
    }
}

bool starts_with(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

// 标准 Base64（兼容无填充、URL-safe 的 -_ 与内嵌空白）。
// 严格拒绝非法字符，避免把半解码的垃圾当成 textures JSON。
bool base64_decode(std::string_view input, std::string* out) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int table[256];
    for (int& value : table) {
        value = -1;
    }
    for (int i = 0; i < 64; ++i) {
        table[static_cast<unsigned char>(kAlphabet[i])] = i;
    }
    table[static_cast<unsigned char>('-')] = 62;  // URL-safe 变体
    table[static_cast<unsigned char>('_')] = 63;

    out->clear();
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char ch : input) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (std::isspace(byte) != 0) {
            continue;
        }
        if (ch == '=') {
            break;  // 填充：剩余低位不是数据
        }
        const int value = table[byte];
        if (value < 0) {
            return false;
        }
        acc = (acc << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<char>((acc >> bits) & 0xFFu));
            acc &= bits == 0 ? 0u : ((1u << bits) - 1u);
        }
    }
    return !out->empty();
}

bool is_png(const std::string& bytes) {
    static constexpr unsigned char kMagic[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return bytes.size() >= 8 &&
           std::equal(kMagic, kMagic + 8,
                      reinterpret_cast<const unsigned char*>(bytes.data()));
}

// 只允许 http/https；PCL 还会把 minecraft.net 的 http 链接升为 https。
util::Result<std::string> normalize_skin_url(std::string url) {
    if (starts_with(url, "http://") && url.find("minecraft.net/") != std::string::npos) {
        url = "https://" + url.substr(7);
    }
    if (!starts_with(url, "http://") && !starts_with(url, "https://")) {
        return util::Result<std::string>::fail(util::Error::make(
            "skin.bad_url", "皮肤地址不是 HTTP(S) 链接，已拒绝加载", url));
    }
    return util::Result<std::string>::ok(std::move(url));
}

std::string texture_skin_url(const json& textures) {
    if (!textures.is_object()) {
        return {};
    }
    // Mojang / Yggdrasil 标准是大写 SKIN；部分皮肤站返回小写或首字母大写。
    for (const char* key : {"SKIN", "skin", "Skin"}) {
        if (!textures.contains(key)) {
            continue;
        }
        const json& skin = textures[key];
        if (skin.is_object()) {
            const std::string url = skin.value("url", "");
            if (!url.empty()) {
                return url;
            }
        }
    }
    return {};
}

}  // namespace

std::filesystem::path account_skin_cache_dir() {
    return sscl::platform::data_dir() / L"cache" / L"skins";
}

std::string skin_url_from_ms_profile(const std::string& profile_json) {
    const json node = json::parse(profile_json, nullptr, false);
    if (node.is_discarded() || !node.is_object()) {
        return {};
    }
    const json skins = node.value("skins", json::array());
    if (!skins.is_array() || skins.empty() || !skins[0].is_object()) {
        return {};
    }
    const std::string url = skins[0].value("url", "");
    const util::Result<std::string> checked = normalize_skin_url(url);
    return checked.is_ok() ? checked.value() : std::string();
}

util::Result<std::string> parse_authlib_skin_url(const std::string& profile_body) {
    if (profile_body.empty()) {
        return util::Result<std::string>::fail(
            util::Error::make("skin.no_profile", "服务器未返回角色资料", {}));
    }
    const json root = json::parse(profile_body, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return util::Result<std::string>::fail(
            util::Error::make("skin.bad_response", "角色资料不是合法 JSON", {}));
    }

    std::string encoded;
    const json properties = root.value("properties", json::array());
    if (properties.is_array()) {
        for (const json& property : properties) {
            if (property.is_object() && property.value("name", "") == "textures") {
                encoded = property.value("value", "");
                break;
            }
        }
    }
    if (encoded.empty()) {
        return util::Result<std::string>::fail(
            util::Error::make("skin.no_textures", "该角色尚未设置皮肤", {}));
    }

    std::string decoded;
    if (!base64_decode(encoded, &decoded)) {
        return util::Result<std::string>::fail(
            util::Error::make("skin.bad_base64", "皮肤属性不是合法 Base64", {}));
    }
    const json payload = json::parse(decoded, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
        return util::Result<std::string>::fail(
            util::Error::make("skin.bad_textures", "皮肤属性内容不是合法 JSON", {}));
    }
    const std::string url = texture_skin_url(payload.value("textures", json::object()));
    if (url.empty()) {
        return util::Result<std::string>::fail(
            util::Error::make("skin.no_textures", "该角色尚未设置皮肤", {}));
    }
    return normalize_skin_url(url);
}

util::Result<std::string> authlib_skin_url(const std::string& server_url,
                                          const std::string& uuid_compact) {
    const std::string root = normalize_authlib_server(server_url);
    if (root.empty() || uuid_compact.empty()) {
        return util::Result<std::string>::fail(
            util::Error::make("skin.bad_server", "缺少认证服务器地址或角色 UUID", {}));
    }
    // 与 PCL McSkinGetAddress("Auth") / HMCL getProfilePropertiesURL 一致：
    // 不带 ?unsigned=false，兼容性最好（unsigned=true 时 textures 属性同样返回）。
    const std::string cache_key = root + "|" + uuid_compact;
    const std::string remembered = cached_skin_url(cache_key);
    if (!remembered.empty()) {
        return util::Result<std::string>::ok(remembered);
    }
    sscl::platform::HttpRequest request;
    request.url = root + "/sessionserver/session/minecraft/profile/" + uuid_compact;
    request.headers.emplace_back("Accept", "application/json");
    request.headers.emplace_back("User-Agent", "SSCL/1.0 (authlib-injector)");
    request.connect_timeout_ms = 10000;
    request.receive_timeout_ms = 12000;

    util::Result<sscl::platform::HttpResponse> response =
        sscl::platform::http_request(request);
    if (response.is_error()) {
        return util::Result<std::string>::fail(util::Error::make(
            "skin.network", "无法获取角色皮肤资料", response.error().detail));
    }
    // Yggdrasil 规范：无该角色 → 204 No Content。
    if (response.value().status_code == 204) {
        return util::Result<std::string>::fail(
            util::Error::make("skin.no_profile", "服务器上没有这个角色的皮肤", {}));
    }
    if (response.value().status_code != 200) {
        return util::Result<std::string>::fail(util::Error::make(
            "skin.http_status", "获取角色皮肤资料失败",
            request.url + " status " + std::to_string(response.value().status_code)));
    }
    util::Result<std::string> parsed = parse_authlib_skin_url(response.value().body);
    if (parsed.is_ok()) {
        remember_skin_url(cache_key, parsed.value());
    }
    return parsed;
}

util::Result<std::vector<std::uint8_t>> fetch_account_skin_png(const AccountInfo& account) {
    std::string url;
    switch (account.type) {
        case AccountType::kMicrosoft:
            url = skin_url_from_ms_profile(account.profile_json);
            break;
        case AccountType::kAuthlibInjector:
        case AccountType::kNide8:
            if (!account.server_url.empty()) {
                util::Result<std::string> resolved =
                    authlib_skin_url(account.server_url, account.uuid_compact);
                if (resolved.is_error()) {
                    return util::Result<std::vector<std::uint8_t>>::fail(
                        std::move(resolved.error()));
                }
                url = std::move(resolved.value());
            }
            break;
        case AccountType::kOffline:
            break;  // 离线 UUID 服务器上没有正版皮肤（PCL 同样跳过）
    }
    if (url.empty()) {
        return util::Result<std::vector<std::uint8_t>>::fail(
            util::Error::make("skin.unavailable", "该账号没有可用皮肤", {}));
    }

    // 按皮肤 URL 缓存：换皮肤 → URL 变化 → 自然失效，无需手动刷新。
    const std::filesystem::path file =
        account_skin_cache_dir() /
        (sscl::util::hash_hex(sscl::util::HashAlgorithm::kSha256, url) + ".png");
    util::Result<std::string> cached = sscl::platform::read_file(file);
    if (cached.is_ok() && is_png(cached.value()) && cached.value().size() <= kMaxSkinBytes) {
        return util::Result<std::vector<std::uint8_t>>::ok(
            std::vector<std::uint8_t>(cached.value().begin(), cached.value().end()));
    }

    sscl::platform::HttpRequest request;
    request.url = url;
    request.headers.emplace_back("Accept", "image/png,image/*");
    request.headers.emplace_back("User-Agent", "SSCL/1.0 (skin)");
    request.connect_timeout_ms = 10000;
    request.receive_timeout_ms = 15000;
    util::Result<sscl::platform::HttpResponse> response =
        sscl::platform::http_request(request);
    if (response.is_error()) {
        return util::Result<std::vector<std::uint8_t>>::fail(util::Error::make(
            "skin.network", "无法下载皮肤", response.error().detail));
    }
    if (response.value().status_code != 200) {
        return util::Result<std::vector<std::uint8_t>>::fail(util::Error::make(
            "skin.http_status", "下载皮肤失败",
            url + " status " + std::to_string(response.value().status_code)));
    }
    const std::string& png = response.value().body;
    if (!is_png(png) || png.size() > kMaxSkinBytes) {
        return util::Result<std::vector<std::uint8_t>>::fail(
            util::Error::make("skin.bad_image", "皮肤文件不是有效的 PNG", url));
    }

    // 缓存只影响显示速度，失败不阻塞头像；不等强制刷盘（D-119）。
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (!ec) {
        util::Result<void> saved = sscl::platform::write_file_atomic(
            file, std::string_view(png.data(), png.size()), /*flush_to_disk=*/false);
        if (saved.is_error()) {
            sscl::util::log_warn("[skin] cache write failed: " + saved.error().detail);
        }
    }
    return util::Result<std::vector<std::uint8_t>>::ok(
        std::vector<std::uint8_t>(png.begin(), png.end()));
}

}  // namespace sscl::core
