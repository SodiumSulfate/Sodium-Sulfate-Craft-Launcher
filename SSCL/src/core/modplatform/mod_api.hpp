#pragma once

// Mod 平台 HTTP 层（D-055）：
// - 统一 User-Agent（Modrinth 强制唯一标识）；
// - CurseForge 官方 API 带 x-api-key；MCIM 镜像不带；
// - 官方优先、MCIM 回退（可配置）；仅对已缓存 URL 做 TTL 缓存；
// - 429 尊重 Retry-After，网络/5xx 有限重试，其余 4xx 直接失败；
// - 统一错误码（mod.platform_unavailable / invalid_key / rate_limited / network / ...）。

#include <chrono>
#include <filesystem>
#include <string>
#include <stop_token>
#include <vector>

#include "core/modplatform/mod_cache.hpp"
#include "core/modplatform/mod_types.hpp"
#include "util/result.hpp"

namespace sscl::core {

struct ModHttpOptions {
    ModPlatform platform = ModPlatform::kModrinth;
    std::string user_agent = "SSCL/0.1.0";
    std::string curseforge_api_key;
    bool use_mcim = false;
    std::filesystem::path cache_dir;
    int timeout_ms = 20000;
    int retries = 2;
};

class ModHttpClient {
public:
    explicit ModHttpClient(ModHttpOptions options);

    const ModHttpOptions& options() const { return m_options; }
    bool platform_available(std::string* reason = nullptr) const;

    // stale=true 表示“网络失败，返回了过期缓存”。
    util::Result<std::string> get_json(const std::string& official_url,
                                       std::chrono::seconds ttl,
                                       std::stop_token stop = {},
                                       bool* stale = nullptr);

    // POST JSON（不做 TTL 缓存；用于 version_files/update_individual/fingerprints）。
    util::Result<std::string> post_json(const std::string& official_url, const std::string& body,
                                        std::stop_token stop = {});

    // 供测试：按 options 生成候选 URL（官方/MCIM；CF 无 Key 时只走 MCIM）。
    static std::vector<std::string> candidate_urls(const ModHttpOptions& options,
                                                   const std::string& official_url);
    static std::string mcim_url(const std::string& official_url);
    static util::Error map_status_error(ModPlatform platform, int status);

private:
    util::Result<std::string> request_once(const std::string& url, std::stop_token stop,
                                           int* status, int* retry_after_seconds,
                                           const std::string& method = "GET",
                                           const std::string& body = {},
                                           const std::string& content_type = {}) const;

    ModHttpOptions m_options;
    ModCache m_cache;
};

// 1 秒粒度睡眠，期间响应取消；返回 false 表示被取消。
bool sleep_with_stop(std::chrono::milliseconds duration, std::stop_token stop);

}  // namespace sscl::core
