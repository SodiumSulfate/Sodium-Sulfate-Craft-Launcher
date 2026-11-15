#pragma once

// Minecraft 版本清单（Mojang version_manifest.json v1；HMCL MojangDownloadProvider.java:75、
// PCL ModDownload.vb:235/275 行为数据）。下载走 DownloadScheduler（官方→BMCLAPI 回退）。

#include <filesystem>
#include <string>
#include <vector>

#include "core/download/download_scheduler.hpp"
#include "util/result.hpp"

namespace sscl::core {

// 清单来源（PCL DlVersionListOrder：版本列表源与文件源可分开设置；SSCL §7.4 固定官方优先）。
constexpr const char* kVersionListUrl =
    "https://piston-meta.mojang.com/mc/game/version_manifest.json";

struct VersionListEntry {
    std::string id;
    std::string type;          // release / snapshot / old_beta / old_alpha
    std::string url;           // 版本 JSON 地址
    std::string time;
    std::string release_time;
};

struct VersionList {
    std::string latest_release;
    std::string latest_snapshot;
    std::vector<VersionListEntry> entries;  // 清单原序（新→旧）
};

// 解析清单 JSON。
util::Result<VersionList> parse_version_list(const std::string& json_text);

// 下载并解析清单；缓存写 cache_file，下载失败时回退已有缓存。
util::Result<VersionList> fetch_version_list(DownloadScheduler& scheduler,
                                             const std::filesystem::path& cache_file,
                                             std::stop_token stop = {});

}  // namespace sscl::core
