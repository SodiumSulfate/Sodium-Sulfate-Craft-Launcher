#pragma once

// 加载器版本清单（design/mod-loader-install.md §5）。
// 纯解析函数（可单测、不触网）+ 带缓存/镜像回退的获取函数（M3-2）。
// 端点与解析规则见 knowledge/mod-loader-install.md §2~§4。

#include <filesystem>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "core/loader/loader_types.hpp"
#include "core/loader/maven_metadata.hpp"
#include "util/result.hpp"

namespace sscl::core {

struct LoaderCatalogOptions {
    std::filesystem::path cache_dir;   // <data>/cache/loaders；空 = 不缓存
    int request_timeout_ms = 20000;
    int cache_ttl_seconds = 3600;
};

// ---------- 纯解析（单测覆盖） ----------

// Fabric：/v2/versions/loader 的 JSON 数组；只保留 stable 字段并过滤掉与该 MC 无关的项。
// mc_version 为空 = 不过滤（列表页可能先展示全部）。
util::Result<std::vector<LoaderEntry>> parse_fabric_loader_versions(
    const std::string& loader_json, const std::string& mc_version);

// Forge：`<mc>-<loader>[-<branch>]` 切分（容忍尾部重复 `-<mc>`）。
struct ForgeVersionId {
    std::string minecraft;
    std::string loader;
    std::string branch;
};
util::Result<ForgeVersionId> parse_forge_version_id(const std::string& full_version);

// Forge promotions_slim.json（{"promos": {"1.20.1-latest": "47.3.0", ...}}）。
struct ForgePromotions {
    std::map<std::string, std::string> promos;
};
util::Result<ForgePromotions> parse_forge_promotions(const std::string& json_text);

// Forge：maven-metadata + promotions → 该 MC 的条目（新→旧；stable=recommended）。
util::Result<std::vector<LoaderEntry>> parse_forge_versions(const MavenMetadata& metadata,
                                                           const ForgePromotions& promotions,
                                                           const std::string& mc_version);

// Forge：BMCLAPI `/forge/minecraft/<mc>` 数组（备用源；files[].category/format/hash）。
util::Result<std::vector<LoaderEntry>> parse_forge_bmclapi_versions(const std::string& json_text,
                                                                    const std::string& mc_version,
                                                                    const ForgePromotions& promotions);

// NeoForge 版本号 → MC 版本（四形态，见 knowledge §4.2）；无法解析返回空串。
std::string neoforge_minecraft_version(const std::string& neoforge_version);

// NeoForge 版本号 → 展示用 loader 版本（去掉 MC 前缀）。
std::string neoforge_loader_display(const std::string& neoforge_version);

// NeoForge：官方 maven JSON（{"versions":[...]}）现行 + legacy（net/neoforged/forge）。
util::Result<std::vector<LoaderEntry>> parse_neoforge_versions(const std::string& current_json,
                                                              const std::string& legacy_json,
                                                              const std::string& mc_version);

// NeoForge：BMCLAPI 明细（{"files":[{"name","type"}],"type":"DIRECTORY"}；PCL ModDownload.vb:926）。
util::Result<std::vector<LoaderEntry>> parse_neoforge_detail_versions(
    const std::string& detail_json, bool legacy, const std::string& mc_version);

// NeoForge：任意版本号集合（maven-metadata 镜像兜底）→ 条目。
util::Result<std::vector<LoaderEntry>> parse_neoforge_version_list(
    const std::vector<std::string>& versions, bool legacy, const std::string& mc_version);

// ---------- 网络获取（缓存 + 官方优先/镜像回退） ----------

// 取某 MC 的加载器版本列表（新→旧）。失败时若有任何缓存则回退缓存并标记 from_cache。
util::Result<std::vector<LoaderEntry>> fetch_loader_versions(
    LoaderKind kind, const std::string& mc_version, const LoaderCatalogOptions& options,
    std::stop_token stop = {});

}  // namespace sscl::core
