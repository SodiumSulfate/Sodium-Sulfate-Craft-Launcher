#pragma once

// 全局布局常量（DIP；D2D 目标 DPI 唯一缩放，§8 / D-013 对标 PCL）。
// PCL 参考值：标题导航条高 48、左页面板宽 300（PageLaunchLeft Width=300）。

namespace sscl::ui {

inline constexpr float kNavHeightDip = 48.0f;       // 顶部导航条高度（兼任自绘标题栏）
inline constexpr float kSidebarWidthDip = 300.0f;   // 左面板宽度
inline constexpr float kSidebarMarginDip = 24.0f;   // 左面板内边距
inline constexpr float kNavLogoWidthDip = 120.0f;   // 导航条 logo 区
inline constexpr float kNavItemWidthDip = 88.0f;    // 导航项宽度
inline constexpr float kNavItemHeightDip = 28.0f;   // 导航项高度

// 自绘标题栏（D-016 无边框）：窗口按钮区与可点击导航胶囊区；
// WM_NCHITTEST 据此判定 HTCLIENT，其余顶栏空白为 HTCAPTION（拖拽）。
inline constexpr float kWindowButtonWidthDip = 46.0f;                 // Win11 标题按钮宽度
inline constexpr float kWindowButtonsWidthDip = kWindowButtonWidthDip * 3.0f;  // min/max/close
inline constexpr float kNavPillsLeftDip = kNavLogoWidthDip + 8.0f;
inline constexpr float kNavPillsRightDip =
    kNavPillsLeftDip + 4.0f * (kNavItemWidthDip + 8.0f);  // 4 个导航项

}  // namespace sscl::ui
