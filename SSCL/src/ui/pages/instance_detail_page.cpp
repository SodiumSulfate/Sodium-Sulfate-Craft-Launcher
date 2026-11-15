#include "ui/pages/instance_detail_page.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <utility>
#include <vector>

#include "platform/file_utils.hpp"
#include "platform/shell_utils.hpp"
#include "platform/zip_utils.hpp"
#include "ui/framework/icon_manager.hpp"
#include "ui/framework/image_loader.hpp"
#include "ui/framework/version_icon.hpp"
#include "ui/framework/window.hpp"
#include "ui/shell/shell_layout.hpp"
#include "ui/theme/theme.hpp"
#include "util/string_utils.hpp"

namespace sscl::ui {

namespace {

// D-093：与版本选择页/下载页统一的左侧栅格（300 栏宽 + 24 内边距）。
constexpr float kContentLeft = kSidebarWidthDip + 24.0f;
// 左侧 tab 条宽度：填满栏宽（300 - 2×24），与父页侧栏视觉对齐。
constexpr float kTabWidth = kSidebarWidthDip - 48.0f;
// 概览“个性化”行距：28 高按钮 + 6px 间距（rebuild_hits 与 draw_overview 共用）。
constexpr float kOverviewRowPitch = 34.0f;

D2D1_RECT_F rect_of(float left, float top, float right, float bottom) {
    return D2D1::RectF(left, top, right, bottom);
}

std::wstring widen(const std::string& text) {
    return sscl::util::utf8_to_wide(text);
}

bool contains(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

bool rect_valid(const D2D1_RECT_F& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool same_rect(const D2D1_RECT_F& left, const D2D1_RECT_F& right) {
    return left.left == right.left && left.top == right.top && left.right == right.right &&
           left.bottom == right.bottom;
}

struct Ctx {
    ID2D1RenderTarget* target = nullptr;
    IDWriteFactory* dwrite = nullptr;
    // 当前悬停/按下的可交互矩形（轮次 115）：draw_button 据此自动决定三态配色，
    // 调用点无需逐个传参。
    D2D1_RECT_F hover_rect{};
    D2D1_RECT_F pressed_rect{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> primary;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> primary_hover;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> primary_pressed;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> primary_container;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hover;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> on_primary;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> secondary;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> muted;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> background;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> surface;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> elevated;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> danger;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> warning;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f9;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f10;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f11;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f12;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f13b;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f14b;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f16b;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f20b;
};

Microsoft::WRL::ComPtr<IDWriteTextFormat> make_format(IDWriteFactory* dwrite, float size,
                                                       DWRITE_FONT_WEIGHT weight) {
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (dwrite == nullptr) {
        return format;
    }
    if (FAILED(dwrite->CreateTextFormat(theme_font_family(), nullptr, weight,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        size, L"zh-cn", format.GetAddressOf()))) {
        format.Reset();
    }
    return format;
}

bool make_ctx(Ctx& ctx, ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (target == nullptr || dwrite == nullptr) {
        return false;
    }
    ctx.target = target;
    ctx.dwrite = dwrite;
    if (FAILED(target->CreateSolidColorBrush(theme_primary(), ctx.primary.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_primary_hover(),
                                             ctx.primary_hover.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_primary_pressed(),
                                             ctx.primary_pressed.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_hover_background(),
                                             ctx.hover.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_text_on_primary(),
                                             ctx.on_primary.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_primary_container(),
                                             ctx.primary_container.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_text_primary(), ctx.text.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_text_secondary(),
                                             ctx.secondary.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_text_muted(), ctx.muted.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_border(), ctx.border.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_background(),
                                             ctx.background.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_surface(), ctx.surface.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_elevated_surface(),
                                             ctx.elevated.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_danger(), ctx.danger.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_warning(), ctx.warning.GetAddressOf()))) {
        return false;
    }
    ctx.f9 = make_format(dwrite, 9.0f, DWRITE_FONT_WEIGHT_NORMAL);
    ctx.f10 = make_format(dwrite, 10.0f, DWRITE_FONT_WEIGHT_NORMAL);
    ctx.f11 = make_format(dwrite, 11.0f, DWRITE_FONT_WEIGHT_NORMAL);
    ctx.f12 = make_format(dwrite, 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
    ctx.f13b = make_format(dwrite, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ctx.f14b = make_format(dwrite, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ctx.f16b = make_format(dwrite, 16.0f, DWRITE_FONT_WEIGHT_BOLD);
    ctx.f20b = make_format(dwrite, 20.0f, DWRITE_FONT_WEIGHT_BOLD);
    return ctx.f9 && ctx.f10 && ctx.f11 && ctx.f12 && ctx.f13b && ctx.f14b && ctx.f16b &&
           ctx.f20b;
}

void draw_text(Ctx& ctx, const std::wstring& text, const D2D1_RECT_F& rect,
               const Microsoft::WRL::ComPtr<IDWriteTextFormat>& format, ID2D1Brush* brush,
               DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING,
               DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
    if (format.Get() == nullptr || brush == nullptr || text.empty() || !rect_valid(rect)) {
        return;
    }
    format->SetTextAlignment(alignment);
    format->SetParagraphAlignment(valign);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ctx.target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), rect,
                          brush, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

void fill_round(Ctx& ctx, const D2D1_RECT_F& rect, float radius, ID2D1Brush* brush) {
    if (brush == nullptr || !rect_valid(rect)) {
        return;
    }
    ctx.target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
}

void stroke_round(Ctx& ctx, const D2D1_RECT_F& rect, float radius, ID2D1Brush* brush) {
    if (brush == nullptr || !rect_valid(rect)) {
        return;
    }
    ctx.target->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush, 1.0f);
}

float measure_text(Ctx& ctx, const std::wstring& text,
                   const Microsoft::WRL::ComPtr<IDWriteTextFormat>& format) {
    if (format == nullptr || text.empty()) {
        return 0.0f;
    }
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(ctx.dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                            format.Get(), 10000.0f, 40.0f,
                                            layout.GetAddressOf())) ||
        layout == nullptr) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) {
        return 0.0f;
    }
    return metrics.widthIncludingTrailingWhitespace;
}

void draw_button(Ctx& ctx, const D2D1_RECT_F& rect, const std::wstring& label, bool primary,
                 bool enabled = true, const char* icon = nullptr) {
    // 三态与首页 Button 控件一致：主按钮 primary → hover → pressed；
    // 次按钮 surface → Hover Background + Primary 描边 → PrimaryContainer。
    // D-083：全部自绘按钮统一取全局 hover/pressed 渐变权重，逐页完全一致。
    const motion::StateColors state = motion::button_colors(primary, enabled, rect);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> stroke_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_brush;
    if (FAILED(ctx.target->CreateSolidColorBrush(state.fill, fill_brush.GetAddressOf())) ||
        FAILED(ctx.target->CreateSolidColorBrush(state.border, stroke_brush.GetAddressOf())) ||
        FAILED(ctx.target->CreateSolidColorBrush(state.text, text_brush.GetAddressOf()))) {
        return;
    }
    ID2D1Brush* background = fill_brush.Get();
    ID2D1Brush* foreground = text_brush.Get();
    fill_round(ctx, rect, theme_radius(), background);
    if (!primary) {
        stroke_round(ctx, rect, theme_radius(), stroke_brush.Get());
    }
    D2D1_RECT_F text_rect = rect;
    if (icon != nullptr && *icon != '\0') {
        const float icon_size = 16.0f;
        const float gap = 5.0f;
        const float text_width = measure_text(ctx, label, ctx.f12);
        const float total = icon_size + gap + text_width;
        float start = rect.left + ((rect.right - rect.left) - total) * 0.5f;
        if (start < rect.left + 5.0f) {
            start = rect.left + 5.0f;
        }
        const float center_y = (rect.top + rect.bottom) * 0.5f;
        IconManager::instance().draw(
            ctx.target, icon,
            rect_of(start, center_y - icon_size * 0.5f, start + icon_size,
                    center_y + icon_size * 0.5f),
            state.text);
        text_rect.left = start + icon_size + gap;
        text_rect.right = rect.right - 3.0f;
    }
    draw_text(ctx, label, text_rect, ctx.f12, foreground,
              (icon != nullptr && *icon != '\0') ? DWRITE_TEXT_ALIGNMENT_LEADING
                                                 : DWRITE_TEXT_ALIGNMENT_CENTER);
}

std::wstring category_label(int category) {
    switch (category) {
        case 1:
            return L"隐藏";
        case 2:
            return L"可安装 Mod";
        case 3:
            return L"常规版本";
        case 4:
            return L"不常用";
        case 5:
            return L"愚人节";
        case 6:
            return L"错误";
        case 0:
        default:
            return L"自动分类";
    }
}

enum SettingKey {
    kIsolation = 1,
    kWindowTitle,
    kVersionInfo,
    kJavaMode,
    kJavaRange,
    kJavaPath,
    kRamType,
    kRamCustom,
    kRamOptimize,
    kServerLogin,
    kServerNide,
    kServerAuthUrl,
    kServerAuthRegister,
    kServerAuthName,
    kServerEnter,
    kJvmArgs,
    kGameArgs,
    kGcMode,
    kPreLaunch,
    kPreLaunchWait,
    kDisableModUpdate,
    kDisableFileCheck,
    kDisableJlw,
    kDisableLua,
    kReset,
};

// 开关类设置项（滑块动画统一遍历，D-093）。
constexpr int kToggleKeys[] = {kIsolation,   kPreLaunchWait,  kDisableModUpdate,
                               kDisableFileCheck, kDisableJlw, kDisableLua};

void choice_items(int key, std::vector<int>* values, std::vector<std::wstring>* labels) {
    values->clear();
    labels->clear();
    switch (key) {
        case kJavaMode:
            *values = {0, 1, 2, 3};
            *labels = {L"自动选择", L"指定版本范围", L"使用版本文件夹", L"指定 Java"};
            break;
        case kRamType:
            *values = {0, 1, 2};
            *labels = {L"自动分配", L"自定义", L"跟随全局"};
            break;
        case kRamOptimize:
            *values = {0, 1, 2};
            *labels = {L"跟随全局", L"开启内存优化", L"关闭内存优化"};
            break;
        case kServerLogin:
            *values = {0, 1, 2, 3, 4};
            *labels = {L"正版或离线", L"仅正版", L"仅离线", L"Nide8", L"Authlib-Injector"};
            break;
        case kGcMode:
            *values = {0, 1, 2, 3, 4, 5};
            *labels = {L"跟随全局", L"ZGC", L"分代 ZGC", L"G1", L"调优 G1", L"不指定"};
            break;
        default:
            break;
    }
}

}  // namespace

InstanceDetailPage::InstanceDetailPage(AppWindow* window,
                                       std::shared_ptr<sscl::core::GameService> service)
    : m_window(window), m_service(std::move(service)) {}

void InstanceDetailPage::set_instance(const std::string& version_id) {
    m_version_id = version_id;
    m_tab = Tab::kOverview;
    m_mods.clear();
    m_mod_hits.clear();
    m_mods_dir.clear();
    m_mod_error.clear();
    m_scroll.jump_to(0.0f);
    m_tab_fade = 1.0f;  // 页面整体由 AppShell 覆盖过渡，tab 内容不再二次淡入
    m_logo_loaded_path.clear();
    m_logo_bitmap.Reset();
    reload();
}

void InstanceDetailPage::set_message(std::wstring message) {
    m_message = std::move(message);
    if (request_repaint) {
        request_repaint();
    }
}

void InstanceDetailPage::reload() {
    m_loaded = false;
    m_summary = {};
    m_settings = {};
    if (m_service == nullptr || m_version_id.empty()) {
        rebuild_hits();
        return;
    }
    m_loaded = load_summary();
    // 进入页面时开关直接落在目标位置，不做入场滑动（页面本身已有覆盖过渡）。
    for (int key : kToggleKeys) {
        m_toggle_anim[key] = setting_toggle_value(key) ? 1.0f : 0.0f;
    }
    rebuild_hits();
    if (request_repaint) {
        request_repaint();
    }
}

bool InstanceDetailPage::load_summary() {
    for (auto& summary : m_service->version_summaries()) {
        if (summary.version.id == m_version_id) {
            m_summary = std::move(summary);
            auto settings = m_service->instance_settings(m_version_id);
            if (settings.is_ok()) {
                m_settings = settings.value();
            }
            // 没有显式决定时，界面使用 resolve_instance_target 的判定结果。
            m_settings.isolated = m_summary.isolated;
            return true;
        }
    }
    return false;
}

std::wstring InstanceDetailPage::state_text() const {
    switch (m_summary.state) {
        case sscl::core::InstanceState::kSnapshot:
            return L"快照";
        case sscl::core::InstanceState::kOld:
            return L"旧版";
        case sscl::core::InstanceState::kFool:
            return L"愚人节";
        case sscl::core::InstanceState::kForge:
            return L"Forge";
        case sscl::core::InstanceState::kNeoForge:
            return L"NeoForge";
        case sscl::core::InstanceState::kFabric:
            return L"Fabric";
        case sscl::core::InstanceState::kQuilt:
            return L"Quilt";
        case sscl::core::InstanceState::kError:
            return L"错误";
        case sscl::core::InstanceState::kOriginal:
        default:
            return L"正式版";
    }
}

std::wstring InstanceDetailPage::category_text() const {
    if (m_settings.starred) {
        return L"收藏夹";
    }
    return category_label(m_settings.category);
}

std::wstring InstanceDetailPage::setting_label(int key) const {
    switch (key) {
        case kIsolation:
            return L"版本隔离";
        case kWindowTitle:
            return L"窗口标题";
        case kVersionInfo:
            return L"游戏内信息";
        case kJavaMode:
            return L"Java 选择";
        case kJavaRange:
            return L"Java 版本范围";
        case kJavaPath:
            return L"指定 Java";
        case kRamType:
            return L"内存分配";
        case kRamCustom:
            return L"自定义内存";
        case kRamOptimize:
            return L"内存优化";
        case kServerLogin:
            return L"登录方式";
        case kServerNide:
            return L"Nide8 地址";
        case kServerAuthUrl:
            return L"Authlib 地址";
        case kServerAuthRegister:
            return L"注册地址";
        case kServerAuthName:
            return L"服务器名称";
        case kServerEnter:
            return L"自动进入服务器";
        case kJvmArgs:
            return L"JVM 参数";
        case kGameArgs:
            return L"游戏参数";
        case kGcMode:
            return L"垃圾回收器";
        case kPreLaunch:
            return L"启动前命令";
        case kPreLaunchWait:
            return L"等待启动前命令";
        case kDisableModUpdate:
            return L"禁用 Mod 更新";
        case kDisableFileCheck:
            return L"关闭文件校验";
        case kDisableJlw:
            return L"禁用 JLW";
        case kDisableLua:
            return L"禁用 LUA";
        case kReset:
            return L"初始化版本设置";
        default:
            return L"";
    }
}

std::wstring InstanceDetailPage::setting_desc(int key) const {
    switch (key) {
        case kIsolation:
            return L"隔离后存档、Mod、配置使用独立目录；切换不会自动搬迁文件";
        case kWindowTitle:
            return L"为空时跟随全局设置";
        case kVersionInfo:
            return L"显示在游戏窗口标题/信息中；为空跟随全局";
        case kJavaMode:
            return L"自动优先使用版本要求；也可直接指定 Java";
        case kJavaRange:
            return L"例如 [17.0.1, 25.0)；仅在“指定版本范围”时使用";
        case kJavaPath:
            return L"java.exe、bin 目录或 JDK 根目录";
        case kRamType:
            return L"跟随全局使用启动页设置；自定义只影响此版本";
        case kRamCustom:
            return L"1–64 GB；仅在“自定义”时使用";
        case kRamOptimize:
            return L"PCL 内存优化；跟随全局时不单独设置";
        case kServerLogin:
            return L"仅影响此版本的登录方式";
        case kServerNide:
            return L"Nide8 统一登录地址";
        case kServerAuthUrl:
            return L"Authlib-Injector 认证服务器地址";
        case kServerAuthRegister:
            return L"注册/忘记密码跳转地址";
        case kServerAuthName:
            return L"显示在登录界面的服务器名称";
        case kServerEnter:
            return L"启动后自动进入的服务器地址，如 127.0.0.1:25565";
        case kJvmArgs:
            return L"追加到 JVM 参数末尾；为空跟随全局";
        case kGameArgs:
            return L"追加到游戏参数末尾；为空跟随全局";
        case kGcMode:
            return L"跟随全局使用启动页 GC 设置";
        case kPreLaunch:
            return L"启动游戏前执行的命令；为空不执行";
        case kPreLaunchWait:
            return L"命令执行完成后再启动游戏";
        case kDisableModUpdate:
            return L"启动时跳过 Mod 更新检查";
        case kDisableFileCheck:
            return L"跳过资源/文件完整性校验";
        case kDisableJlw:
            return L"关闭 Java Launch Wrapper";
        case kDisableLua:
            return L"关闭 Lua 脚本支持";
        case kReset:
            return L"重置此版本的启动选项，不影响隔离开关";
        default:
            return L"";
    }
}

bool InstanceDetailPage::setting_toggle_value(int key) const {
    switch (key) {
        case kIsolation:
            return m_settings.isolated;
        case kPreLaunchWait:
            return m_settings.pre_launch_wait;
        case kDisableModUpdate:
            return m_settings.disable_mod_update;
        case kDisableFileCheck:
            return m_settings.disable_file_check;
        case kDisableJlw:
            return m_settings.disable_jlw;
        case kDisableLua:
            return m_settings.disable_lua;
        default:
            return false;
    }
}

std::wstring InstanceDetailPage::setting_value(int key) const {
    auto text_or_global = [](const std::string& value) {
        return value.empty() ? std::wstring(L"跟随全局") : widen(value);
    };
    switch (key) {
        case kIsolation:
            return m_settings.isolated ? L"开启" : L"关闭";
        case kWindowTitle:
            return text_or_global(m_settings.window_title);
        case kVersionInfo:
            return text_or_global(m_settings.version_info);
        case kJavaMode: {
            static const wchar_t* kModes[] = {L"自动选择", L"指定版本范围",
                                              L"使用版本文件夹", L"指定 Java"};
            const int mode = (std::max)(0, (std::min)(3, m_settings.java_mode));
            return kModes[mode];
        }
        case kJavaRange:
            return m_settings.java_range.empty() ? std::wstring(L"[17, 25)") : widen(m_settings.java_range);
        case kJavaPath:
            return m_settings.java_path.empty() ? std::wstring(L"未指定") : widen(m_settings.java_path);
        case kRamType: {
            static const wchar_t* kTypes[] = {L"自动分配", L"自定义", L"跟随全局"};
            const int type = (std::max)(0, (std::min)(2, m_settings.ram_type));
            return kTypes[type];
        }
        case kRamCustom:
            return std::to_wstring(m_settings.ram_custom_gb) + L" GB";
        case kRamOptimize: {
            static const wchar_t* kOpt[] = {L"跟随全局", L"开启", L"关闭"};
            const int mode = (std::max)(0, (std::min)(2, m_settings.ram_optimize));
            return kOpt[mode];
        }
        case kServerLogin: {
            static const wchar_t* kLogin[] = {L"正版或离线", L"仅正版", L"仅离线", L"Nide8",
                                              L"Authlib-Injector"};
            const int mode = (std::max)(0, (std::min)(4, m_settings.server_login));
            return kLogin[mode];
        }
        case kServerNide:
            return text_or_global(m_settings.server_nide);
        case kServerAuthUrl:
            return text_or_global(m_settings.server_auth_url);
        case kServerAuthRegister:
            return text_or_global(m_settings.server_auth_register);
        case kServerAuthName:
            return text_or_global(m_settings.server_auth_name);
        case kServerEnter:
            return text_or_global(m_settings.server_enter);
        case kJvmArgs:
            return text_or_global(m_settings.jvm_args);
        case kGameArgs:
            return text_or_global(m_settings.game_args);
        case kGcMode: {
            static const wchar_t* kGc[] = {L"跟随全局", L"ZGC", L"分代 ZGC", L"G1",
                                           L"调优 G1", L"不指定"};
            const int mode = (std::max)(0, (std::min)(5, m_settings.gc_mode));
            return kGc[mode];
        }
        case kPreLaunch:
            return text_or_global(m_settings.pre_launch);
        case kPreLaunchWait:
            return m_settings.pre_launch_wait ? L"开启" : L"关闭";
        case kDisableModUpdate:
            return m_settings.disable_mod_update ? L"开启" : L"关闭";
        case kDisableFileCheck:
            return m_settings.disable_file_check ? L"开启" : L"关闭";
        case kDisableJlw:
            return m_settings.disable_jlw ? L"开启" : L"关闭";
        case kDisableLua:
            return m_settings.disable_lua ? L"开启" : L"关闭";
        case kReset:
            return L"";
        default:
            return L"";
    }
}

void InstanceDetailPage::rebuild_settings_hits() {
    struct Def {
        SettingKind kind;
        int key;
        std::wstring text;
    };
    std::vector<Def> defs;
    auto section = [&defs](const wchar_t* text) {
        defs.push_back(Def{SettingKind::kSection, 0, text});
    };
    auto row = [&defs](SettingKind kind, int key) {
        defs.push_back(Def{kind, key, {}});
    };

    section(L"常规");
    row(SettingKind::kToggle, kIsolation);
    row(SettingKind::kText, kWindowTitle);
    row(SettingKind::kText, kVersionInfo);
    section(L"Java");
    row(SettingKind::kChoice, kJavaMode);
    if (m_settings.java_mode == 1) {
        row(SettingKind::kText, kJavaRange);
    }
    if (m_settings.java_mode == 3) {
        row(SettingKind::kText, kJavaPath);
    }
    section(L"内存");
    row(SettingKind::kChoice, kRamType);
    if (m_settings.ram_type == 1) {
        row(SettingKind::kNumber, kRamCustom);
    }
    row(SettingKind::kChoice, kRamOptimize);
    section(L"服务器");
    row(SettingKind::kChoice, kServerLogin);
    if (m_settings.server_login == 3) {
        row(SettingKind::kText, kServerNide);
    }
    if (m_settings.server_login == 4) {
        row(SettingKind::kText, kServerAuthUrl);
        row(SettingKind::kText, kServerAuthRegister);
        row(SettingKind::kText, kServerAuthName);
    }
    row(SettingKind::kText, kServerEnter);
    section(L"高级");
    row(SettingKind::kText, kJvmArgs);
    row(SettingKind::kText, kGameArgs);
    row(SettingKind::kChoice, kGcMode);
    row(SettingKind::kText, kPreLaunch);
    row(SettingKind::kToggle, kPreLaunchWait);
    row(SettingKind::kToggle, kDisableModUpdate);
    row(SettingKind::kToggle, kDisableFileCheck);
    row(SettingKind::kToggle, kDisableJlw);
    row(SettingKind::kToggle, kDisableLua);
    section(L"其他");
    row(SettingKind::kAction, kReset);

    const float left = kContentLeft;
    const float right = m_width_dip - 24.0f;
    const float top = kNavHeightDip + 88.0f;
    float content = 0.0f;
    for (const Def& def : defs) {
        content += def.kind == SettingKind::kSection ? 34.0f : 58.0f;
    }
    m_scroll.layout(top, m_height_dip - 40.0f, content, m_width_dip);
    const float offset = m_scroll.offset();

    m_setting_hits.clear();
    float y = top - offset;
    for (const Def& def : defs) {
        SettingHit hit;
        hit.kind = def.kind;
        hit.key = def.key;
        hit.text = def.text;
        if (def.kind == SettingKind::kSection) {
            hit.row = rect_of(left, y, right, y + 34.0f);
            y += 34.0f;
            m_setting_hits.push_back(std::move(hit));
            continue;
        }
        hit.row = rect_of(left, y, right, y + 52.0f);
        // 控件统一 28 高、行内上下各 12（行高 52）。
        if (def.kind == SettingKind::kToggle) {
            hit.control = rect_of(right - 66.0f, y + 12.0f, right - 16.0f, y + 40.0f);
        } else if (def.kind == SettingKind::kNumber) {
            hit.minus = rect_of(right - 122.0f, y + 12.0f, right - 90.0f, y + 40.0f);
            hit.plus = rect_of(right - 62.0f, y + 12.0f, right - 30.0f, y + 40.0f);
            hit.control = rect_of(hit.minus.left, hit.minus.top, hit.plus.right, hit.plus.bottom);
        } else if (def.kind == SettingKind::kAction) {
            hit.control = rect_of(right - 120.0f, y + 12.0f, right - 16.0f, y + 40.0f);
        } else {
            hit.control = rect_of(right - 86.0f, y + 12.0f, right - 16.0f, y + 40.0f);
        }
        y += 58.0f;
        m_setting_hits.push_back(std::move(hit));
    }
}

void InstanceDetailPage::rebuild_hits() {
    // 统一刻度（D-094）：返回条 28 高；tab 36 高、节距 44（8px 间隔）；
    // 卡片间距 12、卡内边距 16；按钮 28 高、概览行距 34（6px 间隔）。
    constexpr float kCardGap = 12.0f;
    constexpr float kCardPad = 16.0f;
    constexpr float kButtonH = 28.0f;

    m_tabs.clear();
    const float left = 24.0f;
    const float top = kNavHeightDip + 16.0f;
    m_back_rect = rect_of(left, top, left + 150.0f, top + 28.0f);
    const float tab_top = top + 84.0f;  // 标题底 + 24
    const Tab tabs[] = {Tab::kOverview, Tab::kSettings, Tab::kModify, Tab::kMods, Tab::kExport};
    for (int i = 0; i < 5; ++i) {
        TabHit hit;
        hit.tab = tabs[i];
        hit.enabled = true;
        const float tab_y = tab_top + static_cast<float>(i) * 44.0f;
        hit.rect = rect_of(left, tab_y, left + kTabWidth, tab_y + 36.0f);
        m_tabs.push_back(hit);
    }

    const float content_left = kContentLeft;
    const float content_right = m_width_dip - 24.0f;
    const float content_top = top + 72.0f;

    // 概览卡片总高：版本头 104 + 12 + 个性化 220 + 12 + 快捷方式 84 + 12 + 删除 72。
    // 最小窗口（850×520）装不下，必须滚动 —— 否则「删除版本」会被窗口底边裁掉。
    constexpr float kOverviewHeight = 104.0f + 12.0f + 220.0f + 12.0f + 84.0f + 12.0f + 72.0f;
    if (m_tab == Tab::kOverview) {
        m_scroll.layout(content_top, m_height_dip - 40.0f, kOverviewHeight + 8.0f, m_width_dip);
    } else if (m_tab != Tab::kSettings && m_tab != Tab::kMods) {
        // 设置/Mod 由各自的 rebuild_*_hits 负责滚动布局；这里只清掉“无可滚动内容”的页。
        // 若对所有非概览页都 layout(0,…)，会把内容高清零 → max_offset=0 →
        // 每帧把设置页的滚动位置钳回 0（2026-11-12 设置页无法滚动事故）。
        m_scroll.layout(0.0f, m_height_dip, 0.0f, m_width_dip);
    }
    // 概览的所有纵向坐标都相对 base：滚动偏移一处生效（draw_overview 同源）。
    const float base = content_top - m_scroll.offset();
    m_star_rect = rect_of(content_right - 114.0f, base + 16.0f, content_right - kCardPad,
                          base + 16.0f + kButtonH);

    // 版本头卡片 104 高 → 间距 12 → 个性化卡片。
    const float personal_top = base + 116.0f;
    const float row_top = personal_top + 44.0f;
    for (int i = 0; i < 5; ++i) {
        const float y = row_top + static_cast<float>(i) * kOverviewRowPitch;
        const D2D1_RECT_F button = rect_of(content_right - kCardPad - 84.0f, y + 2.0f,
                                           content_right - kCardPad, y + 2.0f + kButtonH);
        switch (i) {
            case 0:
                m_category_rect = button;
                break;
            case 1:
                m_rename_rect = button;
                break;
            case 2:
                m_desc_rect = button;
                break;
            case 3:
                m_logo_rect = button;
                break;
            default:
                m_isolation_rect = button;
                break;
        }
    }

    // 个性化卡片 220 高（5×34 行、行尾 +208、下留白 12）→ 间距 12 → 快捷方式。
    const float shortcut_top = personal_top + 220.0f + kCardGap;
    m_open_folder_rect = rect_of(content_right - 160.0f, shortcut_top + 28.0f,
                                 content_right - kCardPad, shortcut_top + 28.0f + kButtonH);
    m_shortcut_rect = rect_of(content_left + kCardPad, shortcut_top + 28.0f,
                              content_left + 250.0f, shortcut_top + 28.0f + kButtonH);

    // 快捷方式卡片 84 高 → 间距 12 → 删除卡片（此前为 0 间距）。
    const float danger_top = shortcut_top + 84.0f + kCardGap;
    m_delete_rect = rect_of(content_right - 130.0f, danger_top + 20.0f,
                            content_right - kCardPad, danger_top + 20.0f + 32.0f);

    if (m_category_menu_open) {
        m_category_menu_rect = rect_of(m_category_rect.left - 60.0f, m_category_rect.bottom + 2.0f,
                                       m_category_rect.left + 130.0f,
                                       m_category_rect.bottom + 2.0f + 7.0f * 28.0f + 8.0f);
    }
    if (m_tab == Tab::kSettings) {
        rebuild_settings_hits();
    } else if (m_tab == Tab::kMods) {
        rebuild_mod_hits();
    } else {
        m_setting_hits.clear();
        m_mod_hits.clear();
        // 概览自己有滚动布局（上面已按 kOverviewHeight 设好），不要在这里清掉。
        if (m_tab != Tab::kOverview) {
            m_scroll.layout(0.0f, m_height_dip, 0.0f, m_width_dip);
        }
    }
}

void InstanceDetailPage::layout(UINT width, UINT height, UINT dpi) {
    m_to_dip = 96.0f / static_cast<float>(dpi == 0 ? 96 : dpi);
    m_width_dip = static_cast<float>(width) * m_to_dip;
    m_height_dip = static_cast<float>(height) * m_to_dip;
    m_scroll.layout(0.0f, m_height_dip, 0.0f, m_width_dip);
    rebuild_hits();
}

void InstanceDetailPage::draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    Ctx ctx;
    if (!make_ctx(ctx, target, dwrite)) {
        return;
    }
    ctx.hover_rect = m_hover_rect;
    ctx.pressed_rect = m_pressed_rect;
    rebuild_hits();
    // 页面底色统一 Background（D-079）：浅色下 Surface(#FFFFFF) 与卡片同色会糊成一片。
    ctx.target->FillRectangle(rect_of(0, 0, m_width_dip, m_height_dip),
                              ctx.background.Get());

    IconManager::instance().draw(
        ctx.target, "arrow_back",
        rect_of(m_back_rect.left, m_back_rect.top + 4.0f, m_back_rect.left + 16.0f,
                m_back_rect.bottom - 4.0f),
        theme_text_secondary());
    draw_text(ctx, L"返回版本选择",
              rect_of(m_back_rect.left + 20.0f, m_back_rect.top, m_back_rect.right,
                      m_back_rect.bottom),
              ctx.f12, ctx.secondary.Get());
    const std::wstring title =
        m_loaded ? (std::wstring(L"版本设置 - ") + widen(m_version_id))
                 : std::wstring(L"版本设置");
    draw_text(ctx, title, rect_of(24.0f, kNavHeightDip + 52.0f, m_width_dip - 24.0f,
                                  kNavHeightDip + 82.0f),
              ctx.f20b, ctx.text.Get());

    if (!m_loaded) {
        draw_text(ctx, L"找不到这个版本，可能已被删除或移动。",
                  rect_of(kContentLeft, kNavHeightDip + 120.0f, m_width_dip - 24.0f,
                          kNavHeightDip + 148.0f),
                  ctx.f12, ctx.danger.Get());
        if (m_overlay != Overlay::kNone || motion::modal().visible()) {
            draw_overlay(target, dwrite);
        }
        return;
    }

    static const wchar_t* kTabLabels[] = {L"概览", L"设置", L"修改", L"Mod 管理", L"导出"};
    static const char* kTabIcons[] = {"list", "settings", "build", "extension", "archive"};
    for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i) {
        const auto& tab = m_tabs[static_cast<std::size_t>(i)];
        const bool active = tab.tab == m_tab;
        const bool hovered = same_rect(tab.rect, m_hover_rect);
        const bool pressed = same_rect(tab.rect, m_pressed_rect);
        if (active || pressed) {
            fill_round(ctx, tab.rect, 6.0f, ctx.primary_container.Get());
            stroke_round(ctx, tab.rect, 6.0f, ctx.primary.Get());
        }
        motion::draw_hot_overlay(ctx.target, tab.rect, 6.0f, theme_hover_background(),
                             theme_primary_container());
        const bool highlight = active || hovered || pressed;
        ID2D1Brush* brush =
            highlight ? ctx.primary.Get() : (tab.enabled ? ctx.text.Get() : ctx.muted.Get());
        IconManager::instance().draw(
            ctx.target, kTabIcons[i],
            rect_of(tab.rect.left + 10.0f, tab.rect.top + 9.0f, tab.rect.left + 28.0f,
                    tab.rect.bottom - 9.0f),
            highlight ? theme_primary() : theme_text_primary());
        draw_text(ctx, kTabLabels[i],
                  rect_of(tab.rect.left + 36.0f, tab.rect.top, tab.rect.right - 10.0f,
                          tab.rect.bottom),
                  ctx.f12, brush);
    }

    // D-093：tab 内容整体淡入 + 4px 上移；tab 栏与左侧返回保持在图层之外。
    const float tab_op = motion::ease_out(m_tab_fade);
    const bool tab_layer = motion::push_fade(target, tab_op, 4.0f * (1.0f - tab_op));
    if (m_tab == Tab::kOverview) {
        draw_overview(target, dwrite);
        m_scroll.draw(target);
    } else if (m_tab == Tab::kSettings) {
        draw_settings(target, dwrite);
        m_scroll.draw(target);
    } else if (m_tab == Tab::kModify) {
        draw_modify(target, dwrite);
    } else if (m_tab == Tab::kMods) {
        draw_mods(target, dwrite);
        m_scroll.draw(target);
    } else {
        draw_export(target, dwrite);
    }
    if (tab_layer) {
        motion::pop_layer(target);
    }

    motion::menu().set_open(m_choice_open || m_category_menu_open,
                        m_choice_open ? 4 : (m_category_menu_open ? 5 : 0));
    const bool menu_layer = motion::push_fade(target, motion::menu().opacity(),
                                              motion::menu().offset_y());
    if (m_choice_open || motion::menu().visible_for(4)) {
        fill_round(ctx, m_choice_rect, theme_radius(), ctx.elevated.Get());
        stroke_round(ctx, m_choice_rect, theme_radius(), ctx.border.Get());
        for (std::size_t i = 0; i < m_choice_labels.size(); ++i) {
            const D2D1_RECT_F item =
                rect_of(m_choice_rect.left + 4.0f,
                        m_choice_rect.top + 4.0f + static_cast<float>(i) * 28.0f,
                        m_choice_rect.right - 4.0f,
                        m_choice_rect.top + 4.0f + static_cast<float>(i + 1) * 28.0f);
            motion::draw_hot_overlay(ctx.target, item, 6.0f, theme_hover_background(),
                                 theme_primary_container());
            draw_text(ctx, m_choice_labels[i], item, ctx.f12, ctx.text.Get());
        }
    }

    if (!m_message.empty()) {
        draw_text(ctx, m_message, rect_of(kContentLeft, m_height_dip - 32.0f, m_width_dip - 24.0f,
                                          m_height_dip - 12.0f),
                  ctx.f11, ctx.muted.Get());
    }
    if (m_category_menu_open || motion::menu().visible_for(5)) {
        fill_round(ctx, m_category_menu_rect, theme_radius(), ctx.elevated.Get());
        stroke_round(ctx, m_category_menu_rect, theme_radius(), ctx.border.Get());
        const int current = m_settings.starred ? 0 : m_settings.category;
        for (int i = 0; i < 7; ++i) {
            const D2D1_RECT_F item =
                rect_of(m_category_menu_rect.left + 4.0f,
                        m_category_menu_rect.top + 4.0f + static_cast<float>(i) * 28.0f,
                        m_category_menu_rect.right - 4.0f,
                        m_category_menu_rect.top + 4.0f + static_cast<float>(i + 1) * 28.0f);
            motion::draw_hot_overlay(ctx.target, item, 6.0f, theme_hover_background(),
                                 theme_primary_container());
            draw_text(ctx, category_label(i), item, ctx.f12,
                      i == current ? ctx.primary.Get() : ctx.text.Get());
        }
    }
    if (menu_layer) {
        motion::pop_layer(target);
    }
    if (m_overlay != Overlay::kNone || motion::modal().visible()) {
        draw_overlay(target, dwrite);
    }
}

void InstanceDetailPage::draw_logo(ID2D1RenderTarget* target, IDWriteFactory* dwrite,
                                   const D2D1_RECT_F& rect) {
    Ctx ctx;
    if (!make_ctx(ctx, target, dwrite)) {
        return;
    }
    ctx.hover_rect = m_hover_rect;
    ctx.pressed_rect = m_pressed_rect;
    ensure_logo_bitmap(target);
    if (m_logo_bitmap) {
        target->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        target->DrawBitmap(m_logo_bitmap.Get(), rect, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        target->PopAxisAlignedClip();
        stroke_round(ctx, rect, theme_radius(), ctx.border.Get());
    } else {
        // D-085：无自定义 Logo 时用版本类型默认图标（带中性底 + 1px Outline）。
        draw_version_badge(target, rect, 8.0f, version_icon_kind(m_summary));
    }
}

void InstanceDetailPage::ensure_logo_bitmap(ID2D1RenderTarget* target) {
    if (m_settings.logo == m_logo_loaded_path) {
        return;
    }
    m_logo_loaded_path = m_settings.logo;
    m_logo_bitmap.Reset();
    if (m_settings.logo.empty() || target == nullptr) {
        return;
    }
    const std::filesystem::path path = std::filesystem::path(sscl::util::utf8_to_wide(m_settings.logo));
    auto bytes = sscl::platform::read_file(path);
    if (bytes.is_error()) {
        return;
    }
    std::vector<std::uint8_t> data(bytes.value().begin(), bytes.value().end());
    m_logo_bitmap = load_bitmap_from_bytes(target, data);
}

void InstanceDetailPage::draw_overview(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    Ctx ctx;
    if (!make_ctx(ctx, target, dwrite)) {
        return;
    }
    ctx.hover_rect = m_hover_rect;
    ctx.pressed_rect = m_pressed_rect;
    const float left = kContentLeft;
    const float right = m_width_dip - 24.0f;
    // 概览可滚动：裁剪到内容视口，向上滚动时不会画到导航条/标题上。
    target->PushAxisAlignedClip(rect_of(kContentLeft - 8.0f, kNavHeightDip + 80.0f,
                                        m_width_dip, m_height_dip - 40.0f),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    // 与 rebuild_hits 同源：base = content_top - 滚动偏移。
    const float top = kNavHeightDip + 88.0f - m_scroll.offset();

    // 版本头（浅色下 Elevated=Surface=白，必须靠 1px Outline 定义边界）
    fill_round(ctx, rect_of(left, top, right, top + 104.0f), theme_radius(), ctx.elevated.Get());
    stroke_round(ctx, rect_of(left, top, right, top + 104.0f), theme_radius(), ctx.border.Get());
    draw_logo(target, dwrite, rect_of(left + 16.0f, top + 20.0f, left + 80.0f, top + 84.0f));
    draw_text(ctx, widen(m_version_id), rect_of(left + 92.0f, top + 18.0f, right - 132.0f,
                                                top + 44.0f),
              ctx.f16b, ctx.text.Get());
    std::wstring info = state_text();
    if (!m_summary.minecraft_version.empty()) {
        info += L"  ·  " + widen(m_summary.minecraft_version);
    }
    if (!m_summary.loader.empty()) {
        info += L"  ·  " + widen(m_summary.loader);
    }
    info += m_summary.isolated ? L"  ·  已隔离" : L"  ·  未隔离";
    draw_text(ctx, info, rect_of(left + 92.0f, top + 48.0f, right - 132.0f, top + 70.0f),
              ctx.f11, ctx.secondary.Get());
    draw_button(ctx, m_star_rect, m_summary.starred ? L"已收藏" : L"收藏", false, true,
                "star");

    // 个性化：220 高（行区 +44，5×34 → +208，下留白 12）
    const float personal_top = top + 116.0f;
    fill_round(ctx, rect_of(left, personal_top, right, personal_top + 220.0f), theme_radius(),
               ctx.elevated.Get());
    stroke_round(ctx, rect_of(left, personal_top, right, personal_top + 220.0f), theme_radius(),
                 ctx.border.Get());
    draw_text(ctx, L"个性化", rect_of(left + 16.0f, personal_top + 10.0f, right - 16.0f,
                                      personal_top + 38.0f),
              ctx.f16b, ctx.text.Get());
    const float row_top = personal_top + 44.0f;
    auto row_label = [&](int index, const std::wstring& label, const std::wstring& value) {
        const float y = row_top + static_cast<float>(index) * kOverviewRowPitch;
        draw_text(ctx, label, rect_of(left + 16.0f, y, left + 78.0f, y + 28.0f), ctx.f12,
                  ctx.secondary.Get());
        draw_text(ctx, value, rect_of(left + 82.0f, y, right - 108.0f, y + 28.0f),
                  ctx.f12, ctx.text.Get());
    };
    row_label(0, L"分类", category_text());
    row_label(1, L"名称", widen(m_version_id));
    const std::wstring description =
        m_summary.custom_info.empty() ? std::wstring(L"未设置") : widen(m_summary.custom_info);
    row_label(2, L"描述", description);
    row_label(3, L"图标", m_settings.logo.empty() ? std::wstring(L"默认色块")
                                                  : std::wstring(L"自定义图片"));
    row_label(4, L"隔离", m_summary.isolated ? std::wstring(L"开启（版本独立目录）")
                                             : std::wstring(L"关闭（共用 .minecraft）"));
    draw_button(ctx, m_category_rect, L"更改", false, true, "list");
    draw_button(ctx, m_rename_rect, L"重命名", false, true, "edit");
    draw_button(ctx, m_desc_rect, L"修改", false, true, "edit");
    draw_button(ctx, m_logo_rect, L"选择图片", false, true, "folder_open");
    draw_button(ctx, m_isolation_rect, L"切换", false, true, "settings");

    // 快捷方式：220 + 12 → 232 起，84 高
    const float shortcut_top = personal_top + 232.0f;
    fill_round(ctx, rect_of(left, shortcut_top, right, shortcut_top + 84.0f), theme_radius(),
               ctx.elevated.Get());
    stroke_round(ctx, rect_of(left, shortcut_top, right, shortcut_top + 84.0f), theme_radius(),
                 ctx.border.Get());
    draw_text(ctx, L"快捷方式", rect_of(left + 16.0f, shortcut_top + 10.0f, right - 16.0f,
                                        shortcut_top + 38.0f),
              ctx.f16b, ctx.text.Get());
    draw_text(ctx, L"打开版本文件夹；桌面快捷方式后续版本提供。",
              rect_of(left + 16.0f, shortcut_top + 38.0f, right - 180.0f, shortcut_top + 64.0f),
              ctx.f11, ctx.secondary.Get());
    draw_button(ctx, m_open_folder_rect, L"打开版本文件夹", false, true, "folder_open");

    // 删除：84 + 12 → 96 起，72 高（此前与上一张卡片 0 间距）
    const float danger_top = shortcut_top + 96.0f;
    fill_round(ctx, rect_of(left, danger_top, right, danger_top + 72.0f), theme_radius(),
               ctx.elevated.Get());
    stroke_round(ctx, rect_of(left, danger_top, right, danger_top + 72.0f), theme_radius(),
                 ctx.border.Get());
    draw_text(ctx, L"删除版本", rect_of(left + 16.0f, danger_top + 10.0f, right - 150.0f,
                                        danger_top + 38.0f),
              ctx.f16b, ctx.text.Get());
    draw_text(ctx, L"默认移动到回收站；按住 Shift 点击为永久删除。",
              rect_of(left + 16.0f, danger_top + 38.0f, right - 150.0f, danger_top + 62.0f),
              ctx.f11, ctx.secondary.Get());
    draw_button(ctx, m_delete_rect, L"删除", true, true, "delete");
    target->PopAxisAlignedClip();
}

void InstanceDetailPage::draw_settings(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {    Ctx ctx;
    if (!make_ctx(ctx, target, dwrite)) {
        return;
    }
    ctx.hover_rect = m_hover_rect;
    ctx.pressed_rect = m_pressed_rect;
    const float left = kContentLeft;
    const float right = m_width_dip - 24.0f;
    target->PushAxisAlignedClip(rect_of(left, kNavHeightDip + 80.0f, right, m_height_dip - 40.0f),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (const auto& hit : m_setting_hits) {
        if (hit.kind == SettingKind::kSection) {
            if (hit.row.bottom < kNavHeightDip + 80.0f || hit.row.top > m_height_dip - 40.0f) {
                continue;
            }
            draw_text(ctx, hit.text, rect_of(left + 4.0f, hit.row.top + 6.0f,
                                             right - 16.0f, hit.row.top + 30.0f),
                      ctx.f14b, ctx.secondary.Get());
            continue;
        }
        if (hit.row.bottom < kNavHeightDip + 80.0f || hit.row.top > m_height_dip - 40.0f) {
            continue;
        }
        fill_round(ctx, hit.row, 6.0f, ctx.elevated.Get());
        motion::draw_hot_overlay(ctx.target, hit.row, 6.0f, theme_hover_background(),
                                 theme_primary_container());
        stroke_round(ctx, hit.row, 6.0f, ctx.border.Get());
        draw_text(ctx, setting_label(hit.key),
                  rect_of(hit.row.left + 16.0f, hit.row.top + 6.0f, hit.control.left - 12.0f,
                          hit.row.top + 27.0f),
                  ctx.f12, ctx.text.Get());
        draw_text(ctx, setting_desc(hit.key),
                  rect_of(hit.row.left + 16.0f, hit.row.top + 27.0f, hit.control.left - 12.0f,
                          hit.row.top + 47.0f),
                  ctx.f11, ctx.muted.Get());

        if (hit.kind == SettingKind::kToggle) {
            const bool on = setting_toggle_value(hit.key);
            const auto anim = m_toggle_anim.find(hit.key);
            const float t = anim == m_toggle_anim.end() ? (on ? 1.0f : 0.0f) : anim->second;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> track;
            if (SUCCEEDED(ctx.target->CreateSolidColorBrush(
                    motion::blend(theme_border(), theme_primary(), t), track.GetAddressOf()))) {
                fill_round(ctx, hit.control, 12.0f, track.Get());
            }
            const float knob_x = hit.control.left + 13.0f +
                                 (hit.control.right - hit.control.left - 26.0f) * t;
            ctx.target->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(knob_x, (hit.control.top + hit.control.bottom) * 0.5f),
                              9.0f, 9.0f),
                ctx.elevated.Get());
        } else if (hit.kind == SettingKind::kNumber) {
            draw_button(ctx, hit.minus, L"-", false);
            draw_button(ctx, hit.plus, L"+", false);
            draw_text(ctx, setting_value(hit.key),
                      rect_of(hit.minus.right + 4.0f, hit.row.top + 14.0f, hit.plus.left - 4.0f,
                              hit.row.top + 38.0f),
                      ctx.f12, ctx.text.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else if (hit.kind == SettingKind::kAction) {
            draw_button(ctx, hit.control, L"初始化", false);
        } else {
            draw_text(ctx, setting_value(hit.key),
                      rect_of(hit.row.left + 190.0f, hit.row.top + 14.0f, hit.control.left - 8.0f,
                              hit.row.top + 38.0f),
                      ctx.f12, ctx.secondary.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
            draw_button(ctx, hit.control, hit.kind == SettingKind::kChoice ? L"更改" : L"修改",
                        false);
        }
    }
    target->PopAxisAlignedClip();
}

void InstanceDetailPage::draw_modify(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    Ctx ctx;
    if (!make_ctx(ctx, target, dwrite)) {
        return;
    }
    ctx.hover_rect = m_hover_rect;
    ctx.pressed_rect = m_pressed_rect;
    const float left = kContentLeft;
    const float right = m_width_dip - 24.0f;
    const float top = kNavHeightDip + 88.0f;
    // 卡片内边距 16、间距 12；按钮 28 高（与概览/设置一致）。
    fill_round(ctx, rect_of(left, top, right, top + 148.0f), theme_radius(), ctx.elevated.Get());
    stroke_round(ctx, rect_of(left, top, right, top + 148.0f), theme_radius(), ctx.border.Get());
    draw_text(ctx, L"修改版本", rect_of(left + 16.0f, top + 14.0f, right - 16.0f, top + 40.0f),
              ctx.f16b, ctx.text.Get());
    draw_text(ctx, L"补全缺失的版本文件、natives 与资源；不会改动存档/Mod。",
              rect_of(left + 16.0f, top + 44.0f, right - 16.0f, top + 68.0f), ctx.f11,
              ctx.secondary.Get());
    m_repair_rect = rect_of(left + 16.0f, top + 80.0f, left + 168.0f, top + 108.0f);
    m_download_page_rect = rect_of(left + 180.0f, top + 80.0f, left + 372.0f, top + 108.0f);
    draw_button(ctx, m_repair_rect, m_busy ? L"处理中…" : L"补全/修复文件", true, !m_busy,
                "build");
    draw_button(ctx, m_download_page_rect, L"前往下载页修改加载器", false, true, "download");
    if (!m_busy_text.empty()) {
        draw_text(ctx, m_busy_text,
                  rect_of(left + 16.0f, top + 114.0f, right - 16.0f, top + 132.0f), ctx.f10,
                  ctx.secondary.Get());
    }

    const float info_top = top + 160.0f;
    fill_round(ctx, rect_of(left, info_top, right, info_top + 76.0f), theme_radius(), ctx.elevated.Get());
    stroke_round(ctx, rect_of(left, info_top, right, info_top + 76.0f), theme_radius(),
                 ctx.border.Get());
    draw_text(ctx, L"当前版本", rect_of(left + 16.0f, info_top + 10.0f, right - 16.0f,
                                        info_top + 38.0f),
              ctx.f16b, ctx.text.Get());
    draw_text(ctx, widen(m_summary.version.id) + L"  ·  " + state_text(),
              rect_of(left + 16.0f, info_top + 40.0f, right - 16.0f, info_top + 62.0f), ctx.f11,
              ctx.secondary.Get());
}

void InstanceDetailPage::refresh_mods() {
    m_mods.clear();
    m_mod_error.clear();
    m_mods_dir.clear();
    if (m_service == nullptr) {
        m_mod_error = L"服务不可用";
        rebuild_mod_hits();
        return;
    }
    auto target = m_service->resolve_target(m_version_id);
    if (target.is_error()) {
        m_mod_error = widen(target.error().message);
        rebuild_mod_hits();
        return;
    }
    m_mods_dir = target.value().mods_dir;
    m_mods = sscl::core::list_local_mods(m_mods_dir);
    rebuild_mod_hits();
}

void InstanceDetailPage::rebuild_mod_hits() {
    const float left = kContentLeft;
    const float right = m_width_dip - 24.0f;
    const float toolbar_top = kNavHeightDip + 88.0f;
    // 工具栏按钮 28 高（与全站一致），列表与工具栏间距 12。
    m_import_mods_rect = rect_of(left, toolbar_top, left + 140.0f, toolbar_top + 28.0f);
    m_refresh_mods_rect = rect_of(left + 152.0f, toolbar_top, left + 252.0f, toolbar_top + 28.0f);
    const float list_top = toolbar_top + 40.0f;
    m_scroll.layout(list_top, m_height_dip - 40.0f,
                    static_cast<float>(m_mods.size()) * 54.0f, m_width_dip);
    const float offset = m_scroll.offset();
    m_mod_hits.clear();
    for (std::size_t i = 0; i < m_mods.size(); ++i) {
        const float y = list_top + static_cast<float>(i) * 54.0f - offset;
        ModHit hit;
        hit.file = m_mods[i].file_name;
        hit.name = m_mods[i].enabled_name;
        hit.enabled = m_mods[i].enabled;
        hit.row = rect_of(left, y, right, y + 50.0f);
        hit.toggle = rect_of(right - 150.0f, y + 11.0f, right - 82.0f, y + 39.0f);
        hit.del = rect_of(right - 74.0f, y + 11.0f, right - 16.0f, y + 39.0f);
        m_mod_hits.push_back(std::move(hit));
    }
}

void InstanceDetailPage::draw_mods(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    Ctx ctx;
    if (!make_ctx(ctx, target, dwrite)) {
        return;
    }
    ctx.hover_rect = m_hover_rect;
    ctx.pressed_rect = m_pressed_rect;
    const float left = kContentLeft;
    const float right = m_width_dip - 24.0f;
    draw_button(ctx, m_import_mods_rect, L"导入 Mod", false, true, "upload_file");
    draw_button(ctx, m_refresh_mods_rect, L"刷新", false, true, "refresh");
    draw_text(ctx, std::to_wstring(m_mods.size()) + L" 个文件",
              rect_of(right - 120.0f, kNavHeightDip + 88.0f, right, kNavHeightDip + 118.0f),
              ctx.f11, ctx.muted.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING);

    if (!m_summary.moddable) {
        draw_text(ctx, L"该版本没有安装可识别的 Mod 加载器。",
                  rect_of(left, kNavHeightDip + 170.0f, right, kNavHeightDip + 200.0f),
                  ctx.f12, ctx.secondary.Get());
        return;
    }
    if (!m_mod_error.empty()) {
        draw_text(ctx, m_mod_error,
                  rect_of(left, kNavHeightDip + 170.0f, right, kNavHeightDip + 200.0f),
                  ctx.f12, ctx.danger.Get());
        return;
    }
    if (m_mods.empty()) {
        draw_text(ctx, L"还没有 Mod。点击“导入 Mod”添加。",
                  rect_of(left, kNavHeightDip + 170.0f, right, kNavHeightDip + 200.0f),
                  ctx.f12, ctx.secondary.Get());
        return;
    }
    target->PushAxisAlignedClip(
        rect_of(left, kNavHeightDip + 128.0f, right, m_height_dip - 40.0f),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (const auto& hit : m_mod_hits) {
        if (hit.row.bottom < kNavHeightDip + 128.0f || hit.row.top > m_height_dip - 40.0f) {
            continue;
        }
        fill_round(ctx, hit.row, 6.0f, ctx.elevated.Get());
        motion::draw_hot_overlay(ctx.target, hit.row, 6.0f, theme_hover_background(),
                                 theme_primary_container());
        stroke_round(ctx, hit.row, 6.0f, ctx.border.Get());
        draw_text(ctx, widen(hit.name),
                  rect_of(hit.row.left + 16.0f, hit.row.top + 5.0f, hit.toggle.left - 12.0f,
                          hit.row.top + 27.0f),
                  ctx.f12, ctx.text.Get());
        const std::wstring status = hit.enabled ? L"已启用" : L"已禁用";
        draw_text(ctx, status + L"  ·  " + widen(hit.file),
                  rect_of(hit.row.left + 16.0f, hit.row.top + 27.0f, hit.toggle.left - 12.0f,
                          hit.row.top + 46.0f),
                  ctx.f10, hit.enabled ? ctx.secondary.Get() : ctx.muted.Get());
        draw_button(ctx, hit.toggle, hit.enabled ? L"禁用" : L"启用", false, true,
                    hit.enabled ? "close" : "check");
        draw_button(ctx, hit.del, L"删除", false, true, "delete");
    }
    target->PopAxisAlignedClip();
}

void InstanceDetailPage::toggle_mod(std::size_t index) {
    if (index >= m_mods.size()) {
        return;
    }
    const auto& mod = m_mods[index];
    auto result = sscl::core::set_mod_enabled(mod.full_path, !mod.enabled);
    if (result.is_error()) {
        set_message(L"操作失败：" + widen(result.error().message));
    } else {
        set_message(mod.enabled ? L"已禁用 Mod" : L"已启用 Mod");
    }
    refresh_mods();
}

void InstanceDetailPage::delete_mod(std::size_t index) {
    if (index >= m_mods.size()) {
        return;
    }
    m_overlay = Overlay::kModDelete;
    motion::modal().open();  // 弹出：淡入 + 0.96→1 + 上移 8px（D-081）
    m_mod_delete_path = m_mods[index].full_path;
    m_overlay_prompt = L"删除 Mod？";
    m_overlay_text = widen(m_mods[index].file_name) + L"\n将移动到回收站。";
    layout_overlay();
    if (request_repaint) {
        request_repaint();
    }
}

void InstanceDetailPage::import_mods() {
    if (m_mods_dir.empty()) {
        set_message(L"Mod 目录不可用");
        return;
    }
    std::vector<std::wstring> files;
    if (m_window != nullptr &&
        sscl::platform::open_file_dialog(
            m_window->hwnd(), L"导入 Mod",
            L"Mod 文件\0*.jar;*.zip;*.litemod\0所有文件\0*.*\0\0", &files) &&
        !files.empty()) {
        std::vector<std::filesystem::path> paths;
        paths.reserve(files.size());
        for (const std::wstring& file : files) {
            paths.emplace_back(file);
        }
        auto result = sscl::core::import_mod_files(m_mods_dir, paths);
        if (result.is_error()) {
            set_message(L"导入失败：" + widen(result.error().message));
        } else {
            set_message(L"导入了 " + std::to_wstring(result.value()) + L" 个 Mod");
        }
        refresh_mods();
    }
}

void InstanceDetailPage::repair_files() {
    if (m_busy || m_service == nullptr) {
        return;
    }
    m_busy = true;
    m_busy_text = L"正在补全版本文件…";
    if (request_repaint) {
        request_repaint();
    }
    auto service = m_service;
    auto id = m_version_id;
    AppWindow* window = m_window;
    std::thread([service, id, window, this]() {
        std::wstring message;
        auto loaded = service->load_version(id);
        if (loaded.is_error()) {
            message = L"读取版本失败：" + widen(loaded.error().message);
        } else {
            const auto requests = service->plan_version_install(loaded.value());
            auto version_files = service->install_version(loaded.value(), requests);
            if (version_files.is_error()) {
                message = L"补全失败：" + widen(version_files.error().message);
            } else {
                auto assets = service->install_assets(loaded.value());
                message = assets.is_ok() ? L"版本文件已补全"
                                         : L"资源补全失败：" + widen(assets.error().message);
            }
        }
        if (window != nullptr) {
            window->post_to_ui([this, message]() {
                m_busy = false;
                m_busy_text.clear();
                set_message(message);
            });
        }
    }).detach();
}

void InstanceDetailPage::draw_export(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    Ctx ctx;
    if (!make_ctx(ctx, target, dwrite)) {
        return;
    }
    ctx.hover_rect = m_hover_rect;
    ctx.pressed_rect = m_pressed_rect;
    const float left = kContentLeft;
    const float right = m_width_dip - 24.0f;
    const float top = kNavHeightDip + 88.0f;
    fill_round(ctx, rect_of(left, top, right, top + 164.0f), theme_radius(), ctx.elevated.Get());
    stroke_round(ctx, rect_of(left, top, right, top + 164.0f), theme_radius(), ctx.border.Get());
    draw_text(ctx, L"导出整合包", rect_of(left + 16.0f, top + 14.0f, right - 16.0f,
                                          top + 40.0f),
              ctx.f16b, ctx.text.Get());
    draw_text(ctx, L"导出为 ZIP：包含 versions/<id> 与隔离目录下的 mods。",
              rect_of(left + 16.0f, top + 44.0f, right - 16.0f, top + 68.0f), ctx.f11,
              ctx.secondary.Get());
    m_export_rect = rect_of(left + 16.0f, top + 80.0f, left + 160.0f, top + 108.0f);
    m_open_folder_export_rect = rect_of(left + 172.0f, top + 80.0f, left + 342.0f, top + 108.0f);
    draw_button(ctx, m_export_rect, m_busy ? L"导出中…" : L"导出 ZIP", true, !m_busy,
                "archive");
    draw_button(ctx, m_open_folder_export_rect, L"打开版本文件夹", false, true, "folder_open");
    if (!m_busy_text.empty()) {
        draw_text(ctx, m_busy_text, rect_of(left + 16.0f, top + 114.0f, right - 16.0f,
                                            top + 134.0f),
                  ctx.f10, ctx.secondary.Get());
    }
}

void InstanceDetailPage::export_modpack() {
    if (m_busy || m_service == nullptr || m_window == nullptr) {
        return;
    }
    std::wstring path;
    if (!sscl::platform::save_file_dialog(
            m_window->hwnd(), L"导出整合包", widen(m_version_id) + L".zip",
            m_service->layout().game_dir.wstring(), L"ZIP 文件\0*.zip\0所有文件\0*.*\0\0",
            L"zip", &path) ||
        path.empty()) {
        return;
    }
    m_busy = true;
    m_busy_text = L"正在导出整合包…";
    if (request_repaint) {
        request_repaint();
    }
    auto service = m_service;
    auto id = m_version_id;
    const std::filesystem::path archive(path);
    AppWindow* window = m_window;
    std::thread([service, id, archive, window, this]() {
        std::wstring message;
        std::vector<sscl::platform::ZipWriteEntry> entries;
        const std::filesystem::path version_dir = service->layout().versions_dir / id;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(version_dir, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) {
                continue;
            }
            const std::filesystem::path relative = it->path().lexically_relative(version_dir);
            entries.push_back(sscl::platform::ZipWriteEntry{
                "versions/" + id + "/" + relative.generic_string(), it->path()});
        }
        auto target = service->resolve_target(id);
        if (target.is_ok()) {
            const std::filesystem::path mods_dir = target.value().mods_dir;
            for (std::filesystem::recursive_directory_iterator it(mods_dir, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) {
                    continue;
                }
                const std::filesystem::path relative = it->path().lexically_relative(mods_dir);
                entries.push_back(sscl::platform::ZipWriteEntry{
                    "mods/" + relative.generic_string(), it->path()});
            }
        }
        auto zipped = sscl::platform::zip_write_files(archive, entries);
        message = zipped.is_ok() ? L"整合包已导出"
                                 : L"导出失败：" + widen(zipped.error().message);
        if (window != nullptr) {
            window->post_to_ui([this, message]() {
                m_busy = false;
                m_busy_text.clear();
                set_message(message);
            });
        }
    }).detach();
}

void InstanceDetailPage::draw_overlay(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    Ctx ctx;
    if (!make_ctx(ctx, target, dwrite)) {
        return;
    }
    ctx.hover_rect = m_hover_rect;
    ctx.pressed_rect = m_pressed_rect;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dim;
    if (SUCCEEDED(target->CreateSolidColorBrush(
            D2D1::ColorF(0.0f, 0.0f, 0.0f, motion::modal().dim()), dim.GetAddressOf()))) {
        target->FillRectangle(rect_of(0, 0, m_width_dip, m_height_dip), dim.Get());
    }
    const bool layered = motion::push_pop(target, motion::modal(), m_overlay_card);
    fill_round(ctx, m_overlay_card, theme_radius(), ctx.elevated.Get());
    stroke_round(ctx, m_overlay_card, theme_radius(), ctx.border.Get());
    draw_text(ctx, m_overlay_prompt,
              rect_of(m_overlay_card.left + 24.0f, m_overlay_card.top + 20.0f,
                      m_overlay_card.right - 24.0f, m_overlay_card.top + 52.0f),
              ctx.f16b, ctx.text.Get());
    if (m_overlay == Overlay::kInput && m_input_box) {
        m_input_box->draw(target, dwrite);
    } else {
        draw_text(ctx, m_overlay_text,
                  rect_of(m_overlay_card.left + 24.0f, m_overlay_card.top + 58.0f,
                          m_overlay_card.right - 24.0f, m_overlay_card.bottom - 64.0f),
                  ctx.f12, ctx.secondary.Get(), DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    draw_button(ctx, m_overlay_cancel, L"取消", false);
    draw_button(ctx, m_overlay_ok,
                (m_overlay == Overlay::kDelete || m_overlay == Overlay::kModDelete)
                    ? L"删除"
                    : (m_overlay == Overlay::kReset ? L"初始化" : L"确定"),
                m_overlay == Overlay::kDelete || m_overlay == Overlay::kModDelete,
                (m_overlay == Overlay::kDelete || m_overlay == Overlay::kModDelete)
                    ? "delete"
                    : "check");
    if (layered) {
        motion::pop_pop(target);
    }
}

void InstanceDetailPage::layout_overlay() {
    float width = 420.0f;
    float height = 180.0f;
    if (m_overlay == Overlay::kDelete) {
        width = 380.0f;
        height = 220.0f;
    } else if (m_overlay == Overlay::kIsolation) {
        width = 460.0f;
        height = 208.0f;  // 标题 16b + 正文 12 两行，原 250 留白过多
    }
    const float cx = m_width_dip * 0.5f;
    const float cy = m_height_dip * 0.5f;
    m_overlay_card = rect_of(cx - width * 0.5f, cy - height * 0.5f, cx + width * 0.5f,
                             cy + height * 0.5f);
    m_overlay_cancel = rect_of(m_overlay_card.right - 200.0f, m_overlay_card.bottom - 50.0f,
                               m_overlay_card.right - 116.0f, m_overlay_card.bottom - 18.0f);
    m_overlay_ok = rect_of(m_overlay_card.right - 104.0f, m_overlay_card.bottom - 50.0f,
                           m_overlay_card.right - 20.0f, m_overlay_card.bottom - 18.0f);
    if (m_overlay == Overlay::kInput && m_input_box) {
        m_input_box->bounds = rect_of(m_overlay_card.left + 24.0f, m_overlay_card.top + 66.0f,
                                      m_overlay_card.right - 24.0f, m_overlay_card.top + 102.0f);
    }
}

void InstanceDetailPage::open_input(std::wstring prompt, std::wstring initial,
                                    std::function<void(const std::wstring&)> accept) {
    m_overlay = Overlay::kInput;
    motion::modal().open();  // 弹出：淡入 + 0.96→1 + 上移 8px（D-081）
    m_overlay_prompt = std::move(prompt);
    m_overlay_text.clear();
    m_input_accept = std::move(accept);
    m_input_box = std::make_unique<TextBox>(std::move(initial), 12.0f);
    m_input_box->max_length = 512;
    m_input_box->set_focused(true);
    layout_overlay();
    if (request_repaint) {
        request_repaint();
    }
}

void InstanceDetailPage::open_delete() {
    m_overlay = Overlay::kDelete;
    motion::modal().open();  // 弹出：淡入 + 0.96→1 + 上移 8px（D-081）
    m_delete_permanent = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    m_overlay_prompt = L"确定删除版本 " + widen(m_version_id) + L" 吗？";
    m_overlay_text = m_delete_permanent
                         ? L"将永久删除，无法恢复；隔离版本的存档和 Mod 也会一并删除。"
                         : L"将移动到回收站；隔离版本的存档和 Mod 也会一并删除。";
    layout_overlay();
    if (request_repaint) {
        request_repaint();
    }
}

void InstanceDetailPage::open_isolation() {
    m_overlay = Overlay::kIsolation;
    motion::modal().open();  // 弹出：淡入 + 0.96→1 + 上移 8px（D-081）
    m_overlay_prompt = m_summary.isolated ? L"关闭版本隔离？" : L"开启版本隔离？";
    m_overlay_text =
        L"隔离会把该版本的存档、Mod、配置放到独立目录；切换不会自动搬迁已有文件，"
        L"可能导致存档/Mod 看似消失。请确认后再切换。";
    layout_overlay();
    if (request_repaint) {
        request_repaint();
    }
}

void InstanceDetailPage::open_reset() {
    m_overlay = Overlay::kReset;
    motion::modal().open();  // 弹出：淡入 + 0.96→1 + 上移 8px（D-081）
    m_overlay_prompt = L"初始化版本设置？";
    m_overlay_text = L"将重置窗口标题、Java、内存、服务器、高级参数；隔离开关保持不变。";
    layout_overlay();
    if (request_repaint) {
        request_repaint();
    }
}

void InstanceDetailPage::persist_settings() {
    if (m_service == nullptr) {
        return;
    }
    auto saved = m_service->save_instance_settings_for(m_version_id, m_settings);
    if (saved.is_error()) {
        set_message(L"设置保存失败：" + widen(saved.error().message));
    }
}

void InstanceDetailPage::open_choice(int key) {
    choice_items(key, &m_choice_values, &m_choice_labels);
    if (m_choice_values.empty()) {
        return;
    }
    m_choice_key = key;
    m_choice_open = true;
    m_choice_hover = -1;
    D2D1_RECT_F control{};
    for (const auto& hit : m_setting_hits) {
        if (hit.key == key) {
            control = hit.control;
            break;
        }
    }
    const float height = static_cast<float>(m_choice_labels.size()) * 28.0f + 8.0f;
    float top = control.bottom + 2.0f;
    if (top + height > m_height_dip - 12.0f) {
        top = control.top - height - 2.0f;
    }
    float left = control.left - 60.0f;
    if (left + 220.0f > m_width_dip - 12.0f) {
        left = m_width_dip - 232.0f;
    }
    m_choice_rect = rect_of(left, top, left + 220.0f, top + height);
    if (request_repaint) {
        request_repaint();
    }
}

void InstanceDetailPage::apply_choice(int key, int value) {
    switch (key) {
        case kJavaMode:
            m_settings.java_mode = value;
            break;
        case kRamType:
            m_settings.ram_type = value;
            break;
        case kRamOptimize:
            m_settings.ram_optimize = value;
            break;
        case kServerLogin:
            m_settings.server_login = value;
            break;
        case kGcMode:
            m_settings.gc_mode = value;
            break;
        default:
            return;
    }
    persist_settings();
    rebuild_hits();
}

void InstanceDetailPage::open_setting_text(int key) {
    std::wstring initial;
    switch (key) {
        case kWindowTitle:
            initial = widen(m_settings.window_title);
            break;
        case kVersionInfo:
            initial = widen(m_settings.version_info);
            break;
        case kJavaRange:
            initial = widen(m_settings.java_range);
            break;
        case kJavaPath:
            initial = widen(m_settings.java_path);
            break;
        case kServerNide:
            initial = widen(m_settings.server_nide);
            break;
        case kServerAuthUrl:
            initial = widen(m_settings.server_auth_url);
            break;
        case kServerAuthRegister:
            initial = widen(m_settings.server_auth_register);
            break;
        case kServerAuthName:
            initial = widen(m_settings.server_auth_name);
            break;
        case kServerEnter:
            initial = widen(m_settings.server_enter);
            break;
        case kJvmArgs:
            initial = widen(m_settings.jvm_args);
            break;
        case kGameArgs:
            initial = widen(m_settings.game_args);
            break;
        case kPreLaunch:
            initial = widen(m_settings.pre_launch);
            break;
        default:
            return;
    }
    open_input(L"修改 " + setting_label(key), initial, [this, key](const std::wstring& value) {
        const std::string text = sscl::util::wide_to_utf8(value);
        switch (key) {
            case kWindowTitle:
                m_settings.window_title = text;
                break;
            case kVersionInfo:
                m_settings.version_info = text;
                break;
            case kJavaRange:
                m_settings.java_range = text;
                break;
            case kJavaPath:
                m_settings.java_path = text;
                break;
            case kServerNide:
                m_settings.server_nide = text;
                break;
            case kServerAuthUrl:
                m_settings.server_auth_url = text;
                break;
            case kServerAuthRegister:
                m_settings.server_auth_register = text;
                break;
            case kServerAuthName:
                m_settings.server_auth_name = text;
                break;
            case kServerEnter:
                m_settings.server_enter = text;
                break;
            case kJvmArgs:
                m_settings.jvm_args = text;
                break;
            case kGameArgs:
                m_settings.game_args = text;
                break;
            case kPreLaunch:
                m_settings.pre_launch = text;
                break;
            default:
                return;
        }
        persist_settings();
        rebuild_hits();
    });
}

void InstanceDetailPage::toggle_setting(int key) {
    switch (key) {
        case kPreLaunchWait:
            m_settings.pre_launch_wait = !m_settings.pre_launch_wait;
            break;
        case kDisableModUpdate:
            m_settings.disable_mod_update = !m_settings.disable_mod_update;
            break;
        case kDisableFileCheck:
            m_settings.disable_file_check = !m_settings.disable_file_check;
            break;
        case kDisableJlw:
            m_settings.disable_jlw = !m_settings.disable_jlw;
            break;
        case kDisableLua:
            m_settings.disable_lua = !m_settings.disable_lua;
            break;
        default:
            return;
    }
    persist_settings();
    rebuild_hits();
}

void InstanceDetailPage::close_overlay() {
    m_overlay = Overlay::kNone;
    motion::modal().close();
    // 内容保留到淡出结束（下次 open 覆盖）。
    m_delete_permanent = false;
    m_mod_delete_path.clear();
    if (request_repaint) {
        request_repaint();
    }
}

std::vector<D2D1_RECT_F> InstanceDetailPage::hot_rects() const {
    std::vector<D2D1_RECT_F> rects;
    rects.reserve(16);
    rects.push_back(m_back_rect);
    for (const TabHit& tab : m_tabs) {
        rects.push_back(tab.rect);
    }
    switch (m_tab) {
        case Tab::kOverview:
            rects.push_back(m_star_rect);
            rects.push_back(m_category_rect);
            rects.push_back(m_rename_rect);
            rects.push_back(m_desc_rect);
            rects.push_back(m_logo_rect);
            rects.push_back(m_isolation_rect);
            rects.push_back(m_open_folder_rect);
            rects.push_back(m_delete_rect);
            break;
        case Tab::kSettings:
            for (const SettingHit& hit : m_setting_hits) {
                rects.push_back(hit.control);
                rects.push_back(hit.minus);
                rects.push_back(hit.plus);
            }
            break;
        case Tab::kModify:
            rects.push_back(m_repair_rect);
            rects.push_back(m_download_page_rect);
            break;
        case Tab::kMods:
            rects.push_back(m_import_mods_rect);
            rects.push_back(m_refresh_mods_rect);
            for (const ModHit& hit : m_mod_hits) {
                rects.push_back(hit.toggle);
                rects.push_back(hit.del);
            }
            break;
        case Tab::kExport:
            rects.push_back(m_export_rect);
            rects.push_back(m_open_folder_export_rect);
            break;
    }
    return rects;
}

bool InstanceDetailPage::mouse_move(float x, float y) {
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;
    if (m_overlay != Overlay::kNone) {
            // 弹层是模态的：清掉底层页面的悬停/按下，避免视觉残留。
        if (rect_valid(m_hover_rect) || rect_valid(m_pressed_rect)) {
            m_hover_rect = {};
            m_pressed_rect = {};
            if (request_repaint) {
                request_repaint();
            }
            return true;
        }
        return false;
    }
    if (m_choice_open) {
            int hover = -1;
        if (contains(m_choice_rect, x_dip, y_dip)) {
            const int index = static_cast<int>((y_dip - (m_choice_rect.top + 4.0f)) / 28.0f);
            if (index >= 0 && index < static_cast<int>(m_choice_labels.size())) {
                hover = index;
            }
        }
        if (hover != m_choice_hover) {
            m_choice_hover = hover;
            if (request_repaint) {
                request_repaint();
            }
            return true;
        }
        return false;
    }
    if (m_category_menu_open) {
            int hover = -1;
        if (contains(m_category_menu_rect, x_dip, y_dip)) {
            const int index = static_cast<int>((y_dip - (m_category_menu_rect.top + 4.0f)) / 28.0f);
            if (index >= 0 && index < 7) {
                hover = index;
            }
        }
        if (hover != m_category_hover) {
            m_category_hover = hover;
            if (request_repaint) {
                request_repaint();
            }
            return true;
        }
        return false;
    }
    bool changed = false;
    // 概览/设置/Mod 三页共用同一个滚动条（概览在最小窗口下需要滚动）。
    const bool scrollable = m_tab == Tab::kOverview || m_tab == Tab::kSettings ||
                            m_tab == Tab::kMods;
    if (scrollable && m_scroll.on_mouse_move(x_dip, y_dip)) {
        changed = true;
    }
    D2D1_RECT_F hover{};
    for (const D2D1_RECT_F& rect : hot_rects()) {
        if (rect_valid(rect) && contains(rect, x_dip, y_dip)) {
            hover = rect;
            break;
        }
    }
    // 概览可滚动：滚出内容视口（导航条/标题区）的控件不参与悬停。
    if (m_tab == Tab::kOverview && y_dip < kNavHeightDip + 80.0f) {
        hover = {};
    }
    if (!same_rect(hover, m_hover_rect)) {
        m_hover_rect = hover;
        changed = true;
    }
    // 按下后移出该控件：取消按下态（与常规按钮一致）。
    if (rect_valid(m_pressed_rect) && !same_rect(hover, m_pressed_rect)) {
        m_pressed_rect = {};
        changed = true;
    }
    if (changed && request_repaint) {
        request_repaint();
    }
    return changed;
}

bool InstanceDetailPage::mouse_down(float x, float y) {
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;
    if ((m_tab == Tab::kOverview || m_tab == Tab::kSettings || m_tab == Tab::kMods) &&
        m_scroll.on_mouse_down(x_dip, y_dip)) {
        m_pressed_rect = {};
        return true;
    }
    if (rect_valid(m_hover_rect)) {
        m_pressed_rect = m_hover_rect;
        if (request_repaint) {
            request_repaint();
        }
    }
    return true;
}

bool InstanceDetailPage::mouse_up(float x, float y) {
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;

    if (rect_valid(m_pressed_rect)) {
        m_pressed_rect = {};
        if (request_repaint) {
            request_repaint();
        }
    }

    if (m_overlay != Overlay::kNone) {
        if (!contains(m_overlay_card, x_dip, y_dip)) {
            return true;
        }
        if (contains(m_overlay_cancel, x_dip, y_dip)) {
            close_overlay();
            return true;
        }
        if (!contains(m_overlay_ok, x_dip, y_dip)) {
            return true;
        }
        if (m_overlay == Overlay::kInput && m_input_box) {
            const std::wstring value = m_input_box->text;
            auto accept = m_input_accept;
            close_overlay();
            if (accept) {
                accept(value);
            }
        } else if (m_overlay == Overlay::kDelete) {
            const std::string id = m_version_id;
            const bool permanent = m_delete_permanent;
            close_overlay();
            if (m_service != nullptr && m_service->delete_instance(id, permanent).is_ok()) {
                if (on_deleted) {
                    on_deleted(id);
                }
                if (on_back) {
                    on_back();
                }
            } else {
                set_message(L"删除失败");
            }
        } else if (m_overlay == Overlay::kIsolation) {
            const bool target = !m_summary.isolated;
            close_overlay();
            if (m_service != nullptr && m_service->set_instance_isolation(m_version_id, target).is_ok()) {
                reload();
                if (on_changed) {
                    on_changed();
                }
                set_message(target ? L"已开启版本隔离" : L"已关闭版本隔离");
            } else {
                set_message(L"切换隔离失败");
            }
        } else if (m_overlay == Overlay::kReset) {
            const bool isolated = m_settings.isolated;
            const bool decided = m_settings.decided;
            const bool starred = m_settings.starred;
            const int category = m_settings.category;
            const std::string custom_info = m_settings.custom_info;
            const std::string logo = m_settings.logo;
            m_settings = sscl::core::InstanceSettings{};
            m_settings.isolated = isolated;
            m_settings.decided = decided;
            m_settings.starred = starred;
            m_settings.category = category;
            m_settings.custom_info = custom_info;
            m_settings.logo = logo;
            close_overlay();
            persist_settings();
            reload();
            set_message(L"版本设置已初始化");
        } else if (m_overlay == Overlay::kModDelete) {
            const std::filesystem::path path = m_mod_delete_path;
            close_overlay();
            if (sscl::platform::recycle_file(path)) {
                refresh_mods();
                set_message(L"Mod 已删除");
            } else {
                set_message(L"删除 Mod 失败");
            }
        }
        return true;
    }

    if (m_choice_open) {
        if (contains(m_choice_rect, x_dip, y_dip)) {
            const int index = static_cast<int>((y_dip - (m_choice_rect.top + 4.0f)) / 28.0f);
            if (index >= 0 && index < static_cast<int>(m_choice_values.size())) {
                apply_choice(m_choice_key, m_choice_values[static_cast<std::size_t>(index)]);
            }
        }
        m_choice_open = false;
        m_choice_hover = -1;
        if (request_repaint) {
            request_repaint();
        }
        return true;
    }

    if (m_category_menu_open) {
        if (contains(m_category_menu_rect, x_dip, y_dip)) {
            const int index = static_cast<int>((y_dip - (m_category_menu_rect.top + 4.0f)) / 28.0f);
            m_category_menu_open = false;
            if (index >= 0 && index < 7 && m_service != nullptr) {
                const auto category = static_cast<sscl::core::InstanceCategory>(index);
                if (m_service->set_instance_category(m_version_id, category).is_ok()) {
                    reload();
                    if (on_changed) {
                        on_changed();
                    }
                }
            }
        } else {
            m_category_menu_open = false;
        }
        if (request_repaint) {
            request_repaint();
        }
        return true;
    }

    if (contains(m_back_rect, x_dip, y_dip)) {
        if (on_back) {
            on_back();
        }
        return true;
    }
    for (const auto& tab : m_tabs) {
        if (!contains(tab.rect, x_dip, y_dip)) {
            continue;
        }
        if (!tab.enabled) {
            set_message(L"该页将在后续版本提供");
            return true;
        }
        if (tab.tab != m_tab) {
            m_tab_fade = 0.0f;  // 只淡入内容区（D-093），tab 栏不跟着闪。
        }
        m_tab = tab.tab;
        m_scroll.jump_to(0.0f);
        if (m_tab == Tab::kMods) {
            refresh_mods();
        } else {
            rebuild_hits();
        }
        if (request_repaint) {
            request_repaint();
        }
        return true;
    }

    if (m_tab == Tab::kModify) {
        if (contains(m_repair_rect, x_dip, y_dip)) {
            repair_files();
        } else if (contains(m_download_page_rect, x_dip, y_dip)) {
            if (on_open_downloads) {
                on_open_downloads();
            }
        }
        return true;
    }
    if (m_tab == Tab::kMods) {
        if (m_scroll.on_mouse_up(x_dip, y_dip)) {
            rebuild_mod_hits();
            return true;
        }
        if (contains(m_import_mods_rect, x_dip, y_dip)) {
            import_mods();
            return true;
        }
        if (contains(m_refresh_mods_rect, x_dip, y_dip)) {
            refresh_mods();
            return true;
        }
        for (std::size_t i = 0; i < m_mod_hits.size(); ++i) {
            const auto& hit = m_mod_hits[i];
            if (!contains(hit.row, x_dip, y_dip)) {
                continue;
            }
            if (contains(hit.toggle, x_dip, y_dip)) {
                toggle_mod(i);
            } else if (contains(hit.del, x_dip, y_dip)) {
                delete_mod(i);
            }
            return true;
        }
        return true;
    }
    if (m_tab == Tab::kExport) {
        if (contains(m_export_rect, x_dip, y_dip)) {
            export_modpack();
        } else if (contains(m_open_folder_export_rect, x_dip, y_dip) && m_service != nullptr) {
            sscl::platform::open_folder(m_service->layout().versions_dir / m_version_id);
        }
        return true;
    }

    if (m_tab == Tab::kSettings) {
        if (m_scroll.on_mouse_up(x_dip, y_dip)) {
            rebuild_hits();
            return true;
        }
        for (const auto& hit : m_setting_hits) {
            if (hit.kind == SettingKind::kSection || !contains(hit.row, x_dip, y_dip)) {
                continue;
            }
            if (hit.kind == SettingKind::kNumber) {
                int delta = 0;
                if (contains(hit.minus, x_dip, y_dip)) {
                    delta = -1;
                } else if (contains(hit.plus, x_dip, y_dip)) {
                    delta = 1;
                }
                if (delta == 0) {
                    return true;
                }
                m_settings.ram_custom_gb =
                    (std::max)(1, (std::min)(64, m_settings.ram_custom_gb + delta));
                persist_settings();
                rebuild_hits();
                return true;
            }
            if (!contains(hit.control, x_dip, y_dip)) {
                return true;
            }
            if (hit.key == kIsolation) {
                open_isolation();
            } else if (hit.kind == SettingKind::kToggle) {
                toggle_setting(hit.key);
            } else if (hit.kind == SettingKind::kText) {
                open_setting_text(hit.key);
            } else if (hit.kind == SettingKind::kChoice) {
                open_choice(hit.key);
            } else if (hit.kind == SettingKind::kAction) {
                open_reset();
            }
            return true;
        }
        return true;
    }
    if (m_tab == Tab::kOverview && m_scroll.on_mouse_up(x_dip, y_dip)) {
        // 概览的滚动条拖拽释放：命中后不再触发卡片按钮。
        rebuild_hits();
        return true;
    }
    if (m_tab != Tab::kOverview || m_service == nullptr) {
        return true;
    }
    if (y_dip < kNavHeightDip + 80.0f) {
        return true;  // 被裁剪到视口外的卡片按钮不响应
    }
    if (contains(m_star_rect, x_dip, y_dip)) {
        if (m_service->set_instance_star(m_version_id, !m_summary.starred).is_ok()) {
            reload();
            if (on_changed) {
                on_changed();
            }
        }
        return true;
    }
    if (contains(m_category_rect, x_dip, y_dip)) {
        m_category_menu_open = true;
        m_category_hover = -1;
        rebuild_hits();
        if (request_repaint) {
            request_repaint();
        }
        return true;
    }
    if (contains(m_rename_rect, x_dip, y_dip)) {
        open_input(L"重命名版本", widen(m_version_id), [this](const std::wstring& value) {
            const std::string old_id = m_version_id;
            const std::string new_id = sscl::util::wide_to_utf8(value);
            auto result = m_service->rename_instance(old_id, new_id);
            if (result.is_error()) {
                set_message(L"重命名失败：" + widen(result.error().detail));
                return;
            }
            m_version_id = new_id;
            reload();
            if (on_renamed) {
                on_renamed(old_id, new_id);
            }
            if (on_changed) {
                on_changed();
            }
            set_message(L"已重命名");
        });
        return true;
    }
    if (contains(m_desc_rect, x_dip, y_dip)) {
        open_input(L"版本描述", widen(m_summary.custom_info), [this](const std::wstring& value) {
            if (m_service->set_instance_description(m_version_id,
                                                    sscl::util::wide_to_utf8(value))
                    .is_ok()) {
                reload();
                if (on_changed) {
                    on_changed();
                }
                set_message(L"描述已更新");
            } else {
                set_message(L"描述保存失败");
            }
        });
        return true;
    }
    if (contains(m_logo_rect, x_dip, y_dip)) {
        std::vector<std::wstring> files;
        if (m_window != nullptr &&
            sscl::platform::open_file_dialog(m_window->hwnd(), L"选择版本图标",
                                             L"图片文件\0*.png;*.jpg;*.jpeg\0所有文件\0*.*\0\0",
                                             &files) &&
            !files.empty()) {
            if (m_service->set_instance_logo(m_version_id,
                                             sscl::util::wide_to_utf8(files.front()))
                    .is_ok()) {
                reload();
                if (on_changed) {
                    on_changed();
                }
                set_message(L"图标已更新");
            } else {
                set_message(L"图标保存失败");
            }
        }
        return true;
    }
    if (contains(m_isolation_rect, x_dip, y_dip)) {
        open_isolation();
        return true;
    }
    if (contains(m_open_folder_rect, x_dip, y_dip)) {
        if (m_service != nullptr) {
            sscl::platform::open_folder(m_service->layout().versions_dir / m_version_id);
        }
        return true;
    }
    if (contains(m_delete_rect, x_dip, y_dip)) {
        open_delete();
        return true;
    }
    return false;
}

bool InstanceDetailPage::mouse_leave() {
    bool changed = m_scroll.on_mouse_leave();
    if (rect_valid(m_hover_rect) || rect_valid(m_pressed_rect)) {
        m_hover_rect = {};
        m_pressed_rect = {};
        changed = true;
    }
    return changed;
}

bool InstanceDetailPage::mouse_wheel(float x, float y, float delta) {
    if (m_tab != Tab::kOverview && m_tab != Tab::kSettings && m_tab != Tab::kMods) {
        return false;
    }
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;
    if (x_dip <= kContentLeft || y_dip < kNavHeightDip + 80.0f) {
        return false;
    }
    if (m_scroll.on_wheel(delta, 120.0f)) {
        if (m_tab == Tab::kMods) {
            rebuild_mod_hits();
        } else {
            rebuild_hits();
        }
        if (request_repaint) {
            request_repaint();
        }
        return true;
    }
    return false;
}

bool InstanceDetailPage::handle_char(wchar_t ch) {
    if (m_overlay != Overlay::kInput || !m_input_box) {
        return false;
    }
    if (m_input_box->handle_char(ch)) {
        if (request_repaint) {
            request_repaint();
        }
        return true;
    }
    return false;
}

bool InstanceDetailPage::handle_key_down(unsigned long key) {
    constexpr unsigned long kEscape = 0x1B;
    constexpr unsigned long kReturn = 0x0D;
    constexpr unsigned long kBack = 0x08;
    if (m_overlay == Overlay::kInput && m_input_box) {
        if (key == kEscape) {
            close_overlay();
            return true;
        }
        if (key == kReturn) {
            const std::wstring value = m_input_box->text;
            auto accept = m_input_accept;
            close_overlay();
            if (accept) {
                accept(value);
            }
            return true;
        }
        if (key == kBack) {
            if (m_input_box->handle_key_down(key) && request_repaint) {
                request_repaint();
            }
            return true;
        }
        // D-120：退格之外的编辑键（Ctrl+V/C/X）也必须到输入框，
        // 否则弹层输入框无法粘贴（此前只转发退格，其余按键在这里被吞掉）。
        if (m_input_box->handle_key_down(key)) {
            if (request_repaint) {
                request_repaint();
            }
            return true;
        }
        return false;
    }
    if (m_overlay != Overlay::kNone) {
        if (key == kEscape) {
            close_overlay();
            return true;
        }
        return false;
    }
    if (m_choice_open) {
        if (key == kEscape) {
            m_choice_open = false;
            return true;
        }
        return false;
    }
    if (m_category_menu_open && key == kEscape) {
        m_category_menu_open = false;
        return true;
    }
    if (key == kEscape) {
        if (on_back) {
            on_back();
        }
        return true;
    }
    return false;
}

bool InstanceDetailPage::is_busy() const {
    return m_busy;
}

std::wstring InstanceDetailPage::busy_text() const {
    return m_busy_text.empty() ? L"正在处理…" : m_busy_text;
}

bool InstanceDetailPage::tick(float dt_ms) {
    bool active = false;
    if ((m_tab == Tab::kOverview || m_tab == Tab::kSettings || m_tab == Tab::kMods) &&
        m_scroll.tick(dt_ms)) {
        if (m_tab == Tab::kMods) {
            rebuild_mod_hits();
        } else {
            rebuild_hits();
        }
        active = true;
    }
    // D-093：tab 内容区淡入（120ms，随动效强度缩放；关闭档瞬时到位）。
    if (motion::advance(m_tab_fade, 1.0f, dt_ms, motion::Kind::kFade)) {
        active = true;
    }
    // D-093：开关滑块/底色过渡（60~120ms 手感，随动效强度缩放）。
    for (int key : kToggleKeys) {
        float& anim = m_toggle_anim[key];
        const float target = setting_toggle_value(key) ? 1.0f : 0.0f;
        if (motion::tween(anim, target, dt_ms, motion::Kind::kFade)) {
            active = true;
        }
    }
    return active;
}

}  // namespace sscl::ui
