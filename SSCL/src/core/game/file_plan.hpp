#pragma once

// 缺失文件计划（M1 收官；knowledge/version-json.md §1 + HMCL Constants.java 行为数据）：
// 版本 JSON → 需下载的文件清单（client jar / libraries / natives / assetIndex / log4j）；
// 资产索引 → assets/objects 清单。实际下载走 DownloadScheduler。

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "core/download/download_scheduler.hpp"
#include "core/launch/version_json.hpp"
#include "util/result.hpp"

namespace sscl::core {

// 安装布局（game_dir 下的约定目录）。
struct InstallLayout {
    std::filesystem::path game_dir;       // .minecraft 根
    std::filesystem::path versions_dir;   // <game>/versions
    std::filesystem::path libraries_dir;  // <game>/libraries
    std::filesystem::path assets_dir;     // <game>/assets
    std::filesystem::path natives_dir;    // <versions>/<id>/<id>-natives
    std::string version_id;
};

struct AssetObject {
    std::string virtual_path;  // 索引内的对象名（如 minecraft/sounds/...）
    std::string hash;          // 40 位 SHA1
    std::uint64_t size = 0;
};

struct AssetIndex {
    std::vector<AssetObject> objects;
};

// 解析资产索引 JSON（{objects: {path: {hash, size}}}）。
util::Result<AssetIndex> parse_asset_index(const std::string& json_text);

// 库文件相对 <game>/libraries 的路径：downloads.artifact.path 优先，否则按 Maven 坐标（含 @ext）。
std::string library_relative_file_path(const Library& library);

// 库文件下载 URL：downloads.artifact.url > libraries[].url 基址 + 相对路径 > 官方默认。
// 本地生成库（local_only）或无法推导时返回空串（调用方不得下载）。
std::string library_download_url(const Library& library);

// 当前环境需要、但磁盘上缺失的"本地生成"库（Forge/NeoForge 处理器产物）。
// 用于安装收尾与启动前校验；返回 Maven 坐标列表。
std::vector<std::string> missing_local_libraries(
    const VersionJson& version, const InstallLayout& layout, const OsInfo& os,
    const std::map<std::string, bool>& features);

// 版本文件下载计划（rules/natives 按 os+features 筛选）。
std::vector<DownloadRequest> plan_version_files(const VersionJson& version,
                                                const InstallLayout& layout, const OsInfo& os,
                                                const std::map<std::string, bool>& features);

// 资产对象下载计划（resources.download.minecraft.net/<h2>/<hash>）。
std::vector<DownloadRequest> plan_asset_files(const AssetIndex& index,
                                              const InstallLayout& layout);

// 资产对象的目标路径：<assets>/objects/<hash 前 2>/<hash>
std::filesystem::path asset_object_path(const InstallLayout& layout, const std::string& hash);

}  // namespace sscl::core
