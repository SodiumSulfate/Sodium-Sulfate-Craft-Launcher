#pragma once

// PCL 2.99 版本识别（McVersion.cs）+ Drop 语义。
// 仅做纯计算，不访问磁盘；jar 内 version.json 的 name 由调用方读取后传入。
// 依据：knowledge/pcl-version-management.md §3。

#include <string>

#include "core/launch/version_json.hpp"

namespace sscl::core {

struct PclVersionInfo {
    std::string vanilla_name = "Unknown";  // 1.20.1 / 26.1 / 16w01a / Old / Unknown
    int drop = 209;                        // major*10+minor；旧快照/未知 209
    bool valid = false;                    // Vanilla.Major < 1000
    bool reliable = false;                 // 是否由明确字段识别
    bool is_fool = false;
    bool is_snapshot = false;
    bool is_release = false;  // !fool && !snapshot && 不是 Old/Unknown/pending
    std::string release_time;
};

// PCL 顺序：发布时间老版本 → pending → clientVersion → HMCL game patch（已在 parse 归一化）
// → --fml.mcVersion → inheritsFrom/jar → 下载 URL → libraries → jar → jar version.json
// → JSON id → 文件夹名 → JSON 全文。jar_version_name 为空表示未读取。
PclVersionInfo detect_pcl_version(const VersionJson& version, const std::string& instance_name,
                                  const std::string& jar_version_name = {});

// PCL IsStandardFormat：1.x 或 26+。
bool is_standard_mc_version(const std::string& name);

// D-116：判定字符串是否「像 Minecraft 版本号」（1.21.11 / 26.1 / 16w01a / Old / pending）。
// 用于挡掉加载器 id 与 jar 名（neoforge-21.11.48、fabric-loader-0.20.1-1.21.11），
// 它们绝不能当作原版号参与 Mod 平台筛选或版本列表展示。
bool is_plausible_mc_version(const std::string& name);

// PCL VersionToDrop：有效版本 major*10+minor；否则 209。愚人节名称按 PCL 映射到基础版。
int mc_version_drop(const std::string& name);

}  // namespace sscl::core
