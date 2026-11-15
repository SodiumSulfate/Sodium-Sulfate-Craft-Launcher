#pragma once

// UI 主题（D-026：OLED 真黑 × 电光樱花粉 × 暗莓红；自动跟随 Windows 浅/深色应用模式）。
// 颜色全部集中于本模块（语义化命名，两套调色板）；组件一律经 theme_*() 取色，
// 禁止在组件里硬编码颜色。切换模式后组件按绘制时解析（Label 亦动态解析，D-026）。

#include <d2d1.h>

#include <string>

namespace sscl::ui {

enum class ThemeMode {
    kLight,
    kDark,
};

// —— 主题模式（§五：系统检测 + 动态切换；沿用 D-025 机制）————————
ThemeMode theme_mode();
void set_theme_mode(ThemeMode mode);

// 读 Windows **应用主题**（AppsUseLightTheme；不是 SystemUsesLightTheme）。
// 键缺失/读取失败/权限异常 → 回退浅色（不影响启动）。
ThemeMode detect_system_theme_mode();

// 检测并应用系统主题；返回是否发生变化（变化时调用方需重绘）。
// 主题偏好为非“跟随系统”时直接返回 false（忽略 Windows 广播）。
bool apply_system_theme();

// —— 主题偏好（M5 设置页；LauncherConfig::theme_mode）—————————————
// 键："system" 跟随系统 / "light" 浅色 / "dark" 深色。非法值按 system 处理。
bool theme_follow_system();
void set_theme_follow_system(bool follow);
// 应用偏好键；返回是否改变当前调色板（调用方需重绘）。
bool apply_theme_preference(const std::string& preference);

// —— 语义颜色（浅色 Sakura Light / 深色 OLED True Black，
//    色值全表见 knowledge/theme-colors.md）————————————————
D2D1_COLOR_F theme_background();            // Background：主窗口/主工作区（深色=纯黑）
D2D1_COLOR_F theme_surface();               // Surface：卡片/面板/普通容器
D2D1_COLOR_F theme_container();             // Secondary Surface：侧栏/分组/次级区域
D2D1_COLOR_F theme_elevated_surface();      // Elevated Surface：弹窗/下拉/浮层
D2D1_COLOR_F theme_hover_background();      // Hover Background：可交互区悬停
D2D1_COLOR_F theme_primary();               // Primary：主按钮/关键强调
D2D1_COLOR_F theme_primary_hover();
D2D1_COLOR_F theme_primary_pressed();
D2D1_COLOR_F theme_primary_container();     // 选中行/活动菜单项/局部强调
D2D1_COLOR_F theme_text_primary();
D2D1_COLOR_F theme_text_secondary();
D2D1_COLOR_F theme_text_muted();            // Text Muted：弱化文字
D2D1_COLOR_F theme_text_on_primary();       // 主色上的文字（深色主题=深色 #250014）
D2D1_COLOR_F theme_text_disabled();
D2D1_COLOR_F theme_border();                // Outline：边框/分隔线
D2D1_COLOR_F theme_focus();                 // Focus：键盘焦点/输入框聚焦
// 状态色（批次 42 终端输出：apt 风格彩色文本；两套调色板各自保证对比度）
D2D1_COLOR_F theme_success();               // 完成/成功（Fetched/Done）
D2D1_COLOR_F theme_warning();               // 警告
D2D1_COLOR_F theme_danger();                // 失败/错误（E:）

// 版本类型识别色（D-085）：4 类默认图标各自成色，浅/深两套独立保证对比度。
// 仅用于版本徽标图标，不参与页面/侧栏/按钮底色。
D2D1_COLOR_F theme_version_vanilla();       // 原版：草绿
D2D1_COLOR_F theme_version_fabric();        // Fabric：暖金棕
D2D1_COLOR_F theme_version_forge();         // Forge：橙红
D2D1_COLOR_F theme_version_neoforge();      // NeoForge：蓝

// 派生（导航/标题栏/降级底）
D2D1_COLOR_F theme_nav_background();
D2D1_COLOR_F theme_nav_item_active();      // 选中导航胶囊（= PrimaryContainer）
D2D1_COLOR_F theme_nav_text();             // 未选中导航文字
D2D1_COLOR_F theme_card_shadow();
D2D1_COLOR_F theme_sidebar_background();   // = Secondary Surface
D2D1_COLOR_F theme_glass_base();           // 渲染降级不透明底色
D2D1_COLOR_F theme_titlebar_hover();
D2D1_COLOR_F theme_titlebar_close_hover();

const wchar_t* theme_font_family();   // Microsoft YaHei UI
float theme_radius();                 // 圆角（逻辑像素）

}  // namespace sscl::ui
