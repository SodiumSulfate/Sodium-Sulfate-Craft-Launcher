#include "ui/theme/theme.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#pragma comment(lib, "advapi32.lib")

namespace sscl::ui {

namespace {

// 调色板（语义色；HEX 为主题任务书规定值，rgba 为 0..1 浮点）。
// D-026：全部不透明（OLED 真黑要求主背景纯黑，不再使用玻璃透明度）。
struct Palette {
    D2D1_COLOR_F background;
    D2D1_COLOR_F surface;
    D2D1_COLOR_F container;
    D2D1_COLOR_F elevated_surface;
    D2D1_COLOR_F hover_background;
    D2D1_COLOR_F primary;
    D2D1_COLOR_F primary_hover;
    D2D1_COLOR_F primary_pressed;
    D2D1_COLOR_F primary_container;
    D2D1_COLOR_F text_primary;
    D2D1_COLOR_F text_secondary;
    D2D1_COLOR_F text_muted;
    D2D1_COLOR_F text_on_primary;
    D2D1_COLOR_F text_disabled;
    D2D1_COLOR_F outline;
    D2D1_COLOR_F focus;
    D2D1_COLOR_F nav_background;
    D2D1_COLOR_F nav_text;
    D2D1_COLOR_F nav_item_active;
    D2D1_COLOR_F card_shadow;
    D2D1_COLOR_F glass_base;
    D2D1_COLOR_F titlebar_hover;
    D2D1_COLOR_F success;
    D2D1_COLOR_F warning;
    D2D1_COLOR_F danger;
    D2D1_COLOR_F version_vanilla;
    D2D1_COLOR_F version_fabric;
    D2D1_COLOR_F version_forge;
    D2D1_COLOR_F version_neoforge;
};

constexpr D2D1_COLOR_F rgba(float r, float g, float b, float a = 1.0f) {
    return D2D1_COLOR_F{r, g, b, a};
}

// —— 深色 OLED True Black（任务书 §三）————————————————————
// Background #000000 / Surface #08060A / Secondary Surface #100A12
// Elevated #17101A / Hover #21101B / PrimaryContainer #351126
// Primary #FF5CA8 / PrimaryHover #FF8CC4 / PrimaryPressed #E83786 / OnPrimary #250014
// TextPrimary #F7F0F5 / TextSecondary #B9A8B5 / TextMuted #806F7C
// Outline #2B202A / Focus #FF8CC4
const Palette kDark = {
    /*background */ rgba(0.0000f, 0.0000f, 0.0000f),  // #000000（纯黑，OLED）
    /*surface    */ rgba(0.0314f, 0.0235f, 0.0392f),  // #08060A
    /*container  */ rgba(0.0627f, 0.0392f, 0.0706f),  // #100A12（侧栏/次级）
    /*elevated   */ rgba(0.0902f, 0.0627f, 0.1020f),  // #17101A（浮层）
    /*hover_bg   */ rgba(0.1294f, 0.0627f, 0.1059f),  // #21101B（悬停）
    /*primary    */ rgba(1.0000f, 0.3608f, 0.6588f),  // #FF5CA8
    /*prim_hover */ rgba(1.0000f, 0.5490f, 0.7686f),  // #FF8CC4
    /*prim_press */ rgba(0.9098f, 0.2157f, 0.5255f),  // #E83786
    /*prim_cont  */ rgba(0.2078f, 0.0667f, 0.1490f),  // #351126（选中行）
    /*text_pri   */ rgba(0.9686f, 0.9412f, 0.9608f),  // #F7F0F5
    /*text_sec   */ rgba(0.7255f, 0.6588f, 0.7098f),  // #B9A8B5
    /*text_muted */ rgba(0.5020f, 0.4353f, 0.4863f),  // #806F7C
    /*text_on_pr */ rgba(0.1451f, 0.0000f, 0.0784f),  // #250014（对 #FF5CA8 ≈7.6:1）
    /*text_dis   */ rgba(0.3529f, 0.2941f, 0.3373f),  // #5A4B56（muted 再弱化）
    /*outline    */ rgba(0.1686f, 0.1255f, 0.1647f),  // #2B202A
    /*focus      */ rgba(1.0000f, 0.5490f, 0.7686f),  // #FF8CC4
    /*nav_bg     */ rgba(0.0627f, 0.0392f, 0.0706f),  // Secondary Surface #100A12
    /*nav_text   */ rgba(0.7255f, 0.6588f, 0.7098f),  // TextSecondary #B9A8B5
    /*nav_active */ rgba(0.2078f, 0.0667f, 0.1490f),  // PrimaryContainer #351126
    /*shadow     */ rgba(0.0f, 0.0f, 0.0f, 0.35f),
    /*glass_base */ rgba(0.0f, 0.0f, 0.0f),           // #000000
    /*ttl_hover  */ rgba(1.0f, 1.0f, 1.0f, 0.10f),
    /*success    */ rgba(0.3608f, 0.8392f, 0.5412f),  // #5CD68A
    /*warning    */ rgba(1.0000f, 0.7686f, 0.4118f),  // #FFC469
    /*danger     */ rgba(1.0000f, 0.4196f, 0.4196f),  // #FF6B6B
    /*ver_vanilla*/ rgba(0.4941f, 0.8510f, 0.3412f),  // #7ED957 草绿（对 #000000 ≈9:1）
    /*ver_fabric */ rgba(0.9098f, 0.7255f, 0.4157f),  // #E8B96A 暖金（≈8:1）
    /*ver_forge  */ rgba(1.0000f, 0.5412f, 0.3961f),  // #FF8A65 橙红（≈6:1）
    /*ver_neoforg*/ rgba(0.5098f, 0.6902f, 1.0000f),  // #82B0FF 蓝（≈8:1）
};

// —— 浅色 Sakura Light（任务书 §四）——————————————————————
// Background #FFF7FA / Surface #FFFFFF / Container #F5E8EF
// Primary #C72E71 / PrimaryHover #A91F5D / PrimaryContainer #F8D9E7
// TextPrimary #321B28 / TextSecondary #75616D / Outline #E3CDD8
const Palette kLight = {
    /*background */ rgba(1.0000f, 0.9686f, 0.9804f),  // #FFF7FA
    /*surface    */ rgba(1.0000f, 1.0000f, 1.0000f),  // #FFFFFF
    /*container  */ rgba(0.9608f, 0.9098f, 0.9373f),  // #F5E8EF（侧栏/次级）
    /*elevated   */ rgba(1.0000f, 1.0000f, 1.0000f),  // #FFFFFF（浮层）
    /*hover_bg   */ rgba(0.9608f, 0.9098f, 0.9373f),  // #F5E8EF（悬停）
    /*primary    */ rgba(0.7804f, 0.1804f, 0.4431f),  // #C72E71
    /*prim_hover */ rgba(0.6627f, 0.1216f, 0.3647f),  // #A91F5D
    /*prim_press */ rgba(0.5569f, 0.0980f, 0.3020f),  // #8E194D（按压变体）
    /*prim_cont  */ rgba(0.9725f, 0.8510f, 0.9059f),  // #F8D9E7
    /*text_pri   */ rgba(0.1961f, 0.1059f, 0.1569f),  // #321B28
    /*text_sec   */ rgba(0.4588f, 0.3804f, 0.4275f),  // #75616D
    /*text_muted */ rgba(0.6078f, 0.5294f, 0.5725f),  // #9B8792
    /*text_on_pr */ rgba(1.0000f, 1.0000f, 1.0000f),  // #FFFFFF（对 #C72E71 ≈5.3:1）
    /*text_dis   */ rgba(0.7255f, 0.6471f, 0.6902f),  // #B9A5B0
    /*outline    */ rgba(0.8902f, 0.8039f, 0.8471f),  // #E3CDD8
    /*focus      */ rgba(0.7804f, 0.1804f, 0.4431f),  // Primary #C72E71
    /*nav_bg     */ rgba(0.7804f, 0.1804f, 0.4431f),  // Primary #C72E71（品牌条）
    /*nav_text   */ rgba(1.0000f, 1.0000f, 1.0000f),  // #FFFFFF
    /*nav_active */ rgba(0.9725f, 0.8510f, 0.9059f),  // PrimaryContainer #F8D9E7
    /*shadow     */ rgba(0.0f, 0.0f, 0.0f, 0.10f),
    /*glass_base */ rgba(1.0000f, 0.9686f, 0.9804f),  // #FFF7FA
    /*ttl_hover  */ rgba(1.0f, 1.0f, 1.0f, 0.18f),
    /*success    */ rgba(0.1176f, 0.4784f, 0.2745f),  // #1E7A46
    /*warning    */ rgba(0.6039f, 0.3922f, 0.0000f),  // #9A6400
    /*danger     */ rgba(0.7529f, 0.2235f, 0.1686f),  // #C0392B
    /*ver_vanilla*/ rgba(0.1804f, 0.4392f, 0.1922f),  // #2E7031 深草绿（对 #F5E8EF ≈5.5:1）
    /*ver_fabric */ rgba(0.5412f, 0.3529f, 0.0863f),  // #8A5A16 棕（≈5.8:1）
    /*ver_forge  */ rgba(0.7098f, 0.2706f, 0.1059f),  // #B5451B 深橙（≈5.2:1）
    /*ver_neoforg*/ rgba(0.1725f, 0.3529f, 0.6275f),  // #2C5AA0 深蓝（≈5.6:1）
};

ThemeMode g_mode = ThemeMode::kLight;
// 主题偏好（M5）：true = 跟随 Windows 应用主题；false = 用户明确指定浅/深色。
bool g_follow_system = true;

const Palette& active_palette() {
    return g_mode == ThemeMode::kDark ? kDark : kLight;
}

}  // namespace

// —— 主题模式 ————————————————————————————————————————————

ThemeMode theme_mode() {
    return g_mode;
}

void set_theme_mode(ThemeMode mode) {
    g_mode = mode;
}

ThemeMode detect_system_theme_mode() {
    // Windows **应用主题**（AppsUseLightTheme ≠ SystemUsesLightTheme，§5.1）。
    DWORD value = 1;
    DWORD size = sizeof(value);
    const LSTATUS status = ::RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (status != ERROR_SUCCESS) {
        return ThemeMode::kLight;  // 键缺失/读取失败/权限异常 → 默认浅色
    }
    return value == 0 ? ThemeMode::kDark : ThemeMode::kLight;
}

bool apply_system_theme() {
    if (!g_follow_system) {
        return false;  // 用户已指定浅/深色：忽略系统广播
    }
    const ThemeMode mode = detect_system_theme_mode();
    if (mode == g_mode) {
        return false;
    }
    g_mode = mode;
    return true;
}

bool theme_follow_system() {
    return g_follow_system;
}

void set_theme_follow_system(bool follow) {
    g_follow_system = follow;
}

bool apply_theme_preference(const std::string& preference) {
    if (preference == "light" || preference == "dark") {
        const ThemeMode mode = preference == "dark" ? ThemeMode::kDark : ThemeMode::kLight;
        g_follow_system = false;
        if (g_mode == mode) {
            return false;
        }
        g_mode = mode;
        return true;
    }
    g_follow_system = true;
    return apply_system_theme();
}

// —— 语义颜色（读当前调色板）————————————————————————————

D2D1_COLOR_F theme_background() {
    return active_palette().background;
}

D2D1_COLOR_F theme_surface() {
    return active_palette().surface;
}

D2D1_COLOR_F theme_container() {
    return active_palette().container;
}

D2D1_COLOR_F theme_elevated_surface() {
    return active_palette().elevated_surface;
}

D2D1_COLOR_F theme_hover_background() {
    return active_palette().hover_background;
}

D2D1_COLOR_F theme_primary() {
    return active_palette().primary;
}

D2D1_COLOR_F theme_primary_hover() {
    return active_palette().primary_hover;
}

D2D1_COLOR_F theme_primary_pressed() {
    return active_palette().primary_pressed;
}

D2D1_COLOR_F theme_primary_container() {
    return active_palette().primary_container;
}

D2D1_COLOR_F theme_text_primary() {
    return active_palette().text_primary;
}

D2D1_COLOR_F theme_text_secondary() {
    return active_palette().text_secondary;
}

D2D1_COLOR_F theme_text_muted() {
    return active_palette().text_muted;
}

D2D1_COLOR_F theme_text_on_primary() {
    return active_palette().text_on_primary;
}

D2D1_COLOR_F theme_text_disabled() {
    return active_palette().text_disabled;
}

D2D1_COLOR_F theme_border() {
    return active_palette().outline;
}

D2D1_COLOR_F theme_focus() {
    return active_palette().focus;
}

D2D1_COLOR_F theme_success() {
    return active_palette().success;
}

D2D1_COLOR_F theme_warning() {
    return active_palette().warning;
}

D2D1_COLOR_F theme_danger() {
    return active_palette().danger;
}

D2D1_COLOR_F theme_nav_background() {
    return active_palette().nav_background;
}

D2D1_COLOR_F theme_nav_item_active() {
    return active_palette().nav_item_active;
}

D2D1_COLOR_F theme_nav_text() {
    return active_palette().nav_text;
}

D2D1_COLOR_F theme_card_shadow() {
    return active_palette().card_shadow;
}

D2D1_COLOR_F theme_sidebar_background() {
    return active_palette().container;
}

D2D1_COLOR_F theme_glass_base() {
    return active_palette().glass_base;
}

D2D1_COLOR_F theme_titlebar_hover() {
    return active_palette().titlebar_hover;
}

D2D1_COLOR_F theme_titlebar_close_hover() {
    return D2D1::ColorF(0.769f, 0.169f, 0.110f);  // #C42B1C（Win11 关闭悬停，语义状态色）
}

D2D1_COLOR_F theme_version_vanilla() {
    return active_palette().version_vanilla;
}

D2D1_COLOR_F theme_version_fabric() {
    return active_palette().version_fabric;
}

D2D1_COLOR_F theme_version_forge() {
    return active_palette().version_forge;
}

D2D1_COLOR_F theme_version_neoforge() {
    return active_palette().version_neoforge;
}

const wchar_t* theme_font_family() {
    return L"Microsoft YaHei UI";
}

float theme_radius() {
    return 8.0f;
}

}  // namespace sscl::ui
