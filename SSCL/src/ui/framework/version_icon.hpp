#pragma once

// 版本类型默认图标（D-085）：把 assets/icons/version-*.svg 应用到版本管理相关界面。
//
// 资源名用下划线（RC 标识符不允许连字符），见 sscl.rc。
// 类型判定优先用 VersionSummary.loader，InstanceState 兜底；Quilt 暂无专属资源，
// 复用 Fabric 图形但保持自身语义（与 D-041 的「Quilt 推迟」一致）。
// 颜色差异由 theme_version_*() 四组语义色承担，浅/深两套各自保证对比度；
// 徽标底色一律中性 Container + 1px Outline（D-082），不用 PrimaryContainer，
// 避免与品牌粉混淆、也避免浅色下彩色图标被压住。

#include <d2d1.h>
#include <wrl/client.h>

#include <string>

#include "core/game/instance_types.hpp"
#include "ui/framework/icon_manager.hpp"
#include "ui/theme/theme.hpp"

namespace sscl::ui {

enum class VersionIconKind {
    kVanilla,
    kFabric,
    kForge,
    kNeoForge,
};

inline VersionIconKind version_icon_kind(const std::string& loader,
                                         sscl::core::InstanceState state) {
    if (loader == "fabric" || loader == "quilt") {
        return VersionIconKind::kFabric;
    }
    if (loader == "forge") {
        return VersionIconKind::kForge;
    }
    if (loader == "neoforge") {
        return VersionIconKind::kNeoForge;
    }
    switch (state) {
        case sscl::core::InstanceState::kFabric:
        case sscl::core::InstanceState::kQuilt:
            return VersionIconKind::kFabric;
        case sscl::core::InstanceState::kForge:
            return VersionIconKind::kForge;
        case sscl::core::InstanceState::kNeoForge:
            return VersionIconKind::kNeoForge;
        default:
            return VersionIconKind::kVanilla;
    }
}

inline VersionIconKind version_icon_kind(const sscl::core::VersionSummary& summary) {
    return version_icon_kind(summary.loader, summary.state);
}

inline const char* version_icon_name(VersionIconKind kind) {
    switch (kind) {
        case VersionIconKind::kFabric:
            return "version_fabric";
        case VersionIconKind::kForge:
            return "version_forge";
        case VersionIconKind::kNeoForge:
            return "version_neoforge";
        case VersionIconKind::kVanilla:
        default:
            return "version_vanilla";
    }
}

inline D2D1_COLOR_F version_icon_color(VersionIconKind kind) {
    switch (kind) {
        case VersionIconKind::kFabric:
            return theme_version_fabric();
        case VersionIconKind::kForge:
            return theme_version_forge();
        case VersionIconKind::kNeoForge:
            return theme_version_neoforge();
        case VersionIconKind::kVanilla:
        default:
            return theme_version_vanilla();
    }
}

// 徽标：中性圆角底 + 1px Outline + 按类型着色的矢量图标。
// 图标内缩 18%，给描边型图形留出呼吸空间。
inline void draw_version_badge(ID2D1RenderTarget* target, const D2D1_RECT_F& rect,
                               float radius, VersionIconKind kind) {
    if (target == nullptr || rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> background;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border;
    target->CreateSolidColorBrush(theme_container(), background.GetAddressOf());
    target->CreateSolidColorBrush(theme_border(), border.GetAddressOf());
    D2D1_ROUNDED_RECT rounded{rect, radius, radius};
    if (background != nullptr) {
        target->FillRoundedRectangle(rounded, background.Get());
    }
    if (border != nullptr) {
        target->DrawRoundedRectangle(rounded, border.Get(), 1.0f);
    }
    const float pad_x = (rect.right - rect.left) * 0.18f;
    const float pad_y = (rect.bottom - rect.top) * 0.18f;
    const D2D1_RECT_F inner{rect.left + pad_x, rect.top + pad_y, rect.right - pad_x,
                            rect.bottom - pad_y};
    IconManager::instance().draw(target, version_icon_name(kind), inner,
                                 version_icon_color(kind));
}

}  // namespace sscl::ui
