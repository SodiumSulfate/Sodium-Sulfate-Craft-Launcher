#include "core/modplatform/mod_api.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include "core/download/mirror_map.hpp"
#include "platform/http_client.hpp"
#include "util/log.hpp"

namespace sscl::core {

namespace {

bool starts_with(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// D-107：官方 URL + 慢速切换档 → 跳过官方重试，直接换镜像。
bool slow_switch_official(const std::string& url) {
    return global_source_policy().community_slow_switch() &&
           url.find("mod.mcimirror.top") == std::string::npos;
}

int parse_retry_after(const std::map<std::string, std::string>& headers) {
    const auto it = headers.find("retry-after");
    if (it == headers.end()) {
        return 2;
    }
    try {
        const int seconds = std::stoi(it->second);
        return std::clamp(seconds, 1, 30);
    } catch (...) {
        return 2;
    }
}

}  // namespace

ModHttpClient::ModHttpClient(ModHttpOptions options)
    : m_options(std::move(options)), m_cache(m_options.cache_dir) {}

bool ModHttpClient::platform_available(std::string* reason) const {
    if (m_options.platform == ModPlatform::kCurseForge &&
        m_options.curseforge_api_key.empty() && !m_options.use_mcim) {
        if (reason != nullptr) {
            *reason = "CurseForge 需要 API Key，或启用 MCIM 镜像";
        }
        return false;
    }
    return true;
}

std::string ModHttpClient::mcim_url(const std::string& official_url) {
    static const struct {
        const char* from;
        const char* to;
    } kMappings[] = {
        {"https://api.modrinth.com", "https://mod.mcimirror.top/modrinth"},
        {"https://cdn.modrinth.com", "https://mod.mcimirror.top"},
        {"https://api.curseforge.com", "https://mod.mcimirror.top/curseforge"},
        {"https://edge.forgecdn.net", "https://mod.mcimirror.top"},
    };
    for (const auto& mapping : kMappings) {
        if (starts_with(official_url, mapping.from)) {
            return std::string(mapping.to) + official_url.substr(std::string(mapping.from).size());
        }
    }
    // mediafilez.forgecdn.net 等不做替换（MCIM 明确要求）。
    return official_url;
}

std::vector<std::string> ModHttpClient::candidate_urls(const ModHttpOptions& options,
                                                       const std::string& official_url) {
    std::vector<std::string> urls;
    if (!options.use_mcim) {
        urls.push_back(official_url);
        return urls;
    }
    const std::string mirror = mcim_url(official_url);
    if (options.platform == ModPlatform::kCurseForge &&
        options.curseforge_api_key.empty()) {
        // 没有 Key 时官方必然 403，直接走镜像，省一次失败。
        urls.push_back(mirror);
        return urls;
    }
    // D-107「尽量使用镜像源」：镜像优先，官方仍保留兜底。
    if (global_source_policy().community_prefer_mirror()) {
        urls.push_back(mirror);
        if (mirror != official_url) {
            urls.push_back(official_url);
        }
        return urls;
    }
    urls.push_back(official_url);
    if (mirror != official_url) {
        urls.push_back(mirror);
    }
    return urls;
}

util::Error ModHttpClient::map_status_error(ModPlatform platform, int status) {
    const std::string detail = "HTTP " + std::to_string(status);
    if (status == 401 || status == 403) {
        if (platform == ModPlatform::kCurseForge) {
            return util::Error::make("mod.invalid_key",
                                     "CurseForge API Key 无效或已失效", detail);
        }
        return util::Error::make("mod.forbidden", "平台拒绝了请求（可能被限流/封禁）",
                                 detail);
    }
    if (status == 404) {
        return util::Error::make("mod.not_found", "项目或文件不存在/已下架", detail);
    }
    if (status == 429) {
        return util::Error::make("mod.rate_limited", "请求过于频繁，请稍后重试", detail);
    }
    if (status >= 500) {
        return util::Error::make("mod.api_invalid", "平台服务暂时不可用", detail);
    }
    return util::Error::make("mod.api_invalid", "平台返回了异常状态", detail);
}

util::Result<std::string> ModHttpClient::request_once(const std::string& url,
                                                      std::stop_token stop, int* status,
                                                      int* retry_after_seconds,
                                                      const std::string& method,
                                                      const std::string& body,
                                                      const std::string& content_type) const {
    if (status != nullptr) {
        *status = 0;
    }
    if (retry_after_seconds != nullptr) {
        *retry_after_seconds = 2;
    }
    platform::HttpRequest request;
    request.method = method;
    request.url = url;
    request.body = body;
    // D-107「仅在官方源加载缓慢时改用镜像源」：官方候选给短超时，
    // 超时后由 get_json/post_json 立刻切到 MCIM，不在官方上反复重试。
    const bool is_official = url.find("mod.mcimirror.top") == std::string::npos;
    const bool slow_switch = global_source_policy().community_slow_switch();
    request.receive_timeout_ms = (slow_switch && is_official)
                                     ? (std::min)(m_options.timeout_ms, 5000)
                                     : m_options.timeout_ms;
    request.connect_timeout_ms = (std::min)(request.receive_timeout_ms, 10000);
    request.headers.emplace_back("User-Agent", m_options.user_agent);
    request.headers.emplace_back("Accept", "application/json");
    if (!content_type.empty()) {
        request.headers.emplace_back("Content-Type", content_type);
    }
    if (m_options.platform == ModPlatform::kCurseForge &&
        starts_with(url, "https://api.curseforge.com") &&
        !m_options.curseforge_api_key.empty()) {
        request.headers.emplace_back("x-api-key", m_options.curseforge_api_key);
    }
    if (stop.stop_requested()) {
        return util::Result<std::string>::fail(
            util::Error::make("mod.cancelled", "已取消", {}));
    }
    util::Result<platform::HttpResponse> response = platform::http_request(request);
    if (response.is_error()) {
        return util::Result<std::string>::fail(util::Error::make(
            "mod.network", "网络连接失败", response.error().detail));
    }
    if (status != nullptr) {
        *status = response.value().status_code;
    }
    if (response.value().status_code >= 200 && response.value().status_code < 300) {
        return util::Result<std::string>::ok(std::move(response.value().body));
    }
    if (response.value().status_code == 429 && retry_after_seconds != nullptr) {
        *retry_after_seconds = parse_retry_after(response.value().headers);
    }
    return util::Result<std::string>::fail(
        map_status_error(m_options.platform, response.value().status_code));
}

util::Result<std::string> ModHttpClient::get_json(const std::string& official_url,
                                                  std::chrono::seconds ttl,
                                                  std::stop_token stop, bool* stale) {
    if (stale != nullptr) {
        *stale = false;
    }
    std::string reason;
    if (!platform_available(&reason)) {
        return util::Result<std::string>::fail(util::Error::make(
            "mod.platform_unavailable", "该平台当前不可用", reason));
    }
    const std::int64_t now = now_seconds();
    util::Result<CachedJson> cached = m_cache.read(m_options.platform, official_url);
    if (cached.is_ok() && cached.value().found && ttl.count() > 0 &&
        now - cached.value().saved_at <= ttl.count()) {
        return util::Result<std::string>::ok(cached.value().body);
    }

    const std::vector<std::string> urls = candidate_urls(m_options, official_url);
    util::Error last_error = util::Error::make("mod.network", "网络连接失败", {});
    for (const std::string& url : urls) {
        for (int attempt = 0; attempt <= m_options.retries; ++attempt) {
            if (stop.stop_requested()) {
                return util::Result<std::string>::fail(
                    util::Error::make("mod.cancelled", "已取消", {}));
            }
            int status = 0;
            int retry_after = 2;
            util::Result<std::string> result =
                request_once(url, stop, &status, &retry_after);
            if (result.is_ok()) {
                util::Result<void> saved = m_cache.write(m_options.platform, official_url,
                                                         result.value());
                if (saved.is_error()) {
                    util::log_warn("[modapi] cache write failed: " + saved.error().detail);
                }
                return result;
            }
            last_error = result.error();
            if (last_error.code == "mod.cancelled") {
                return result;
            }
            // D-107：官方候选超时/连不上且处于「慢速切换」档 → 不做同候选重试，
            // 直接换下一个候选（MCIM 镜像）。
            if (status == 0 && slow_switch_official(url)) {
                break;
            }
            const bool retryable = status == 0 || status == 429 || status >= 500;
            if (!retryable || attempt >= m_options.retries) {
                break;
            }
            const int wait_ms = status == 429 ? retry_after * 1000 : (400 * (attempt + 1));
            if (!sleep_with_stop(std::chrono::milliseconds(wait_ms), stop)) {
                return util::Result<std::string>::fail(
                    util::Error::make("mod.cancelled", "已取消", {}));
            }
        }
    }

    // 全部候选失败：允许回退过期缓存（UI 会提示“离线缓存”）。
    if (cached.is_ok() && cached.value().found) {
        if (stale != nullptr) {
            *stale = true;
        }
        util::log_warn("[modapi] 使用过期缓存：" + last_error.code);
        return util::Result<std::string>::ok(cached.value().body);
    }
    return util::Result<std::string>::fail(std::move(last_error));
}

util::Result<std::string> ModHttpClient::post_json(const std::string& official_url,
                                                   const std::string& body,
                                                   std::stop_token stop) {
    std::string reason;
    if (!platform_available(&reason)) {
        return util::Result<std::string>::fail(util::Error::make(
            "mod.platform_unavailable", "该平台当前不可用", reason));
    }
    const std::vector<std::string> urls = candidate_urls(m_options, official_url);
    util::Error last_error = util::Error::make("mod.network", "网络连接失败", {});
    for (const std::string& url : urls) {
        for (int attempt = 0; attempt <= m_options.retries; ++attempt) {
            if (stop.stop_requested()) {
                return util::Result<std::string>::fail(
                    util::Error::make("mod.cancelled", "已取消", {}));
            }
            int status = 0;
            int retry_after = 2;
            util::Result<std::string> result = request_once(
                url, stop, &status, &retry_after, "POST", body, "application/json");
            if (result.is_ok()) {
                return result;
            }
            last_error = result.error();
            if (last_error.code == "mod.cancelled") {
                return result;
            }
            // D-107：官方候选超时/连不上且处于「慢速切换」档 → 不做同候选重试，
            // 直接换下一个候选（MCIM 镜像）。
            if (status == 0 && slow_switch_official(url)) {
                break;
            }
            const bool retryable = status == 0 || status == 429 || status >= 500;
            if (!retryable || attempt >= m_options.retries) {
                break;
            }
            const int wait_ms = status == 429 ? retry_after * 1000 : (400 * (attempt + 1));
            if (!sleep_with_stop(std::chrono::milliseconds(wait_ms), stop)) {
                return util::Result<std::string>::fail(
                    util::Error::make("mod.cancelled", "已取消", {}));
            }
        }
    }
    return util::Result<std::string>::fail(std::move(last_error));
}

bool sleep_with_stop(std::chrono::milliseconds duration, std::stop_token stop) {    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        if (stop.stop_requested()) {
            return false;
        }
        const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        std::this_thread::sleep_for((std::min)(remain, std::chrono::milliseconds(100)));
    }
    return !stop.stop_requested();
}

}  // namespace sscl::core
