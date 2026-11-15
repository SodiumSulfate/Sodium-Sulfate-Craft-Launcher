#pragma once

// Forge/NeoForge 安装器（installer jar）内的 install_profile.json 解析
// （knowledge/mod-loader-install.md §5；判定顺序按结构，不看版本号）。
//   spec            ⇒ 现代 processors 流程
//   install+versionInfo ⇒ 旧版 universal 流程
//   只有 json       ⇒ 中古 maven 流程
// 纯解析，不做任何 IO/网络/进程。

#include <map>
#include <string>
#include <vector>

#include "core/launch/version_json.hpp"
#include "util/result.hpp"

namespace sscl::core {

enum class InstallerFlavor {
    kUnknown = 0,
    kModern,     // spec
    kLegacy,     // install + versionInfo
    kMavenOnly,  // 只有 json（中古）
};

struct InstallerProfile {
    InstallerFlavor flavor = InstallerFlavor::kUnknown;
    int spec = 0;
    std::string profile;    // "forge" / "NeoForge"（仅诊断）
    std::string version;    // 加载器完整版本（1.20.1-47.2.0）
    std::string minecraft;  // 目标 MC（必须与用户选择一致）
    std::string json_entry = "version.json";  // jar 内版本 JSON 条目（去前导 '/'）

    // 旧版（install + versionInfo）
    std::string legacy_install_path;  // install.path（Maven 坐标）
    std::string legacy_file_path;     // install.filePath（jar 内 universal jar 条目）
    std::string embedded_version_json;  // versionInfo 的原始 JSON 文本

    std::map<std::string, SidedData> data;   // 仅现代格式
    std::vector<InstallerProcessor> processors;
    std::vector<Library> libraries;          // 安装器自带库（可能含 url 为空的处理器产物）
};

util::Result<InstallerProfile> parse_installer_profile(const std::string& json_text);

}  // namespace sscl::core
