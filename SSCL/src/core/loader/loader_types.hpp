#pragma once

// 加载器安装的公共类型（design/mod-loader-install.md §4.2）。
// 只描述"要装什么"，不含安装实现；实现见 loader_installer（M3-3/M3-5）。

#include <string>
#include <vector>

#include "core/game/version_list.hpp"

namespace sscl::core {

enum class LoaderKind {
    kNone,      // 不装加载器（纯原版安装，保持既有 install_full 行为）
    kFabric,
    kForge,
    kNeoForge,
};

// 稳定键：用于版本 id / 目录 / 缓存文件名（"vanilla" 表示无加载器）。
inline const char* loader_kind_key(LoaderKind kind) {
    switch (kind) {
        case LoaderKind::kFabric:
            return "fabric";
        case LoaderKind::kForge:
            return "forge";
        case LoaderKind::kNeoForge:
            return "neoforge";
        case LoaderKind::kNone:
        default:
            return "vanilla";
    }
}

// 展示名（"原版" / "Fabric" / "Forge" / "NeoForge"）。
inline const char* loader_kind_display(LoaderKind kind) {
    switch (kind) {
        case LoaderKind::kFabric:
            return "Fabric";
        case LoaderKind::kForge:
            return "Forge";
        case LoaderKind::kNeoForge:
            return "NeoForge";
        case LoaderKind::kNone:
        default:
            return "原版";
    }
}

// 清单条目（UI 列表项 + 安装所需 URL）。
struct LoaderEntry {
    std::string version;              // 展示用加载器版本（Fabric 0.16.9 / Forge 47.2.0 / NeoForge 21.1.0）
    std::string minecraft_version;    // 归属 MC（NeoForge 由版本号推导；安装时以 installer 为准）
    bool stable = true;               // Fabric stable / NeoForge 非 beta-alpha / Forge recommended
    bool recommended = false;         // Forge recommended / NeoForge maven release
    bool from_cache = false;          // 是否来自磁盘缓存（离线回退）
    std::string download_url;         // installer jar 或 profile json URL
    std::string full_version;         // Maven 完整版本（Forge/NeoForge：1.20.1-47.2.0）；Fabric 为空
};

// 用户选定的加载器。
struct LoaderSpec {
    LoaderKind kind = LoaderKind::kNone;
    LoaderEntry entry;
};

// 一次安装请求：目标原版 + 可选加载器。
struct InstallJob {
    VersionListEntry minecraft;
    LoaderSpec loader;
};

}  // namespace sscl::core
