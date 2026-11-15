#pragma once

// 加载器安装：纯函数与 Fabric 落盘补丁（design/mod-loader-install.md §6.3）。
// 网络/Java 相关编排在 GameService::install_with_loader（M3-3 先做 Fabric，Forge/NeoForge 见 M3-4/M3-5）。

#include <string>

#include "core/loader/loader_types.hpp"
#include "util/result.hpp"

namespace sscl::core {

// 版本 id / 目录名（D-037）：
//   fabric-loader-<loader>-<mc> / forge-<mc>-<loader> / neoforge-<ver>（1.20.1 legacy 用 neoforge-<mc>-<loader>）
// 结果只含文件系统安全字符（非法字符替换为 '-'）。
std::string make_loader_version_id(LoaderKind kind, const std::string& minecraft_version,
                                   const LoaderEntry& entry);

// 把 Fabric 官方 profile JSON 补丁为可直接落盘的子版本 JSON：
//   - id = version_id
//   - inheritsFrom 缺失时补 minecraft_version
//   - 注入 sscl 元数据（loader/loaderVersion/minecraftVersion/installerUrl），启动期忽略
// 非 JSON 对象 / 缺少关键字段 ⇒ loader.installer_invalid。
util::Result<std::string> patch_loader_profile(const std::string& profile_json,
                                               LoaderKind kind,
                                               const std::string& version_id,
                                               const std::string& minecraft_version,
                                               const LoaderEntry& entry);

}  // namespace sscl::core
