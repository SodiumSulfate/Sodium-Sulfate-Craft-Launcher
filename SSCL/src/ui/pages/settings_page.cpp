#include "ui/pages/settings_page.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "core/config/build_config.hpp"
#include "core/config/secret_store.hpp"
#include "core/java/java_manager.hpp"
#include "platform/http_client.hpp"
#include "platform/path_utils.hpp"
#include "platform/system_monitor.hpp"
#include "platform/shell_utils.hpp"
#include "ui/framework/icon_manager.hpp"
#include "ui/framework/motion.hpp"
#include "ui/framework/window.hpp"
#include "ui/shell/shell_layout.hpp"
#include "ui/theme/theme.hpp"
#include "util/string_utils.hpp"

namespace sscl::ui {

namespace {

// D-113：诊断面板尺寸。必须定义在所有使用者之前 —— C++ 单遍解析，无隐式前向声明，
// draw_runs()（文件前部）会用到 kProbeLineHeightDip，放到尺寸常量区（后部）会 C2065。
constexpr float kProbeLogHeightDip = 150.0f;  // 结果面板高度（纯底色文本框）
constexpr float kProbeLineHeightDip = 22.0f;   // 面板内单行高度
constexpr float kJavaRowHeightDip = 44.0f;     // D-115：Java / 扫描位置行高

D2D1_RECT_F rect_of(float left, float top, float right, float bottom) {
    return D2D1::RectF(left, top, right, bottom);
}

std::wstring widen(const std::string& text) {
    return sscl::util::utf8_to_wide(text);
}

// D-115：Windows 路径比较（大小写不敏感），用于扫描位置与 Java 列表去重。
bool same_java_path(const std::string& left, const std::string& right) {
    if (left.empty() || right.empty()) {
        return false;
    }
    const std::wstring a = sscl::util::utf8_to_wide(left);
    const std::wstring b = sscl::util::utf8_to_wide(right);
    return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring format_gb(double value) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(1) << value << L" GB";
    return out.str();
}

std::string narrow(const std::wstring& text) {
    return sscl::util::wide_to_utf8(text);
}

bool contains(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

bool rect_valid(const D2D1_RECT_F& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

struct Ctx {
    ID2D1RenderTarget* target = nullptr;
    IDWriteFactory* dwrite = nullptr;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> primary;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> primary_container;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> secondary;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> muted;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> surface;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> elevated;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hover;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> danger;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f9;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f10;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f11;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f12;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f13b;
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
        FAILED(target->CreateSolidColorBrush(theme_primary_container(),
                                             ctx.primary_container.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_text_primary(), ctx.text.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_text_secondary(),
                                             ctx.secondary.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_text_muted(), ctx.muted.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_border(), ctx.border.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_surface(), ctx.surface.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_elevated_surface(),
                                             ctx.elevated.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_hover_background(),
                                             ctx.hover.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_danger(), ctx.danger.GetAddressOf()))) {
        return false;
    }
    ctx.f9 = make_format(dwrite, 9.0f, DWRITE_FONT_WEIGHT_NORMAL);
    ctx.f10 = make_format(dwrite, 10.0f, DWRITE_FONT_WEIGHT_NORMAL);
    ctx.f11 = make_format(dwrite, 11.0f, DWRITE_FONT_WEIGHT_NORMAL);
    ctx.f12 = make_format(dwrite, 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
    ctx.f13b = make_format(dwrite, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ctx.f16b = make_format(dwrite, 16.0f, DWRITE_FONT_WEIGHT_BOLD);
    ctx.f20b = make_format(dwrite, 20.0f, DWRITE_FONT_WEIGHT_BOLD);
    return ctx.f9 && ctx.f10 && ctx.f11 && ctx.f12 && ctx.f13b && ctx.f16b && ctx.f20b;
}

void draw_text(Ctx& ctx, const std::wstring& text, const D2D1_RECT_F& rect,
               const Microsoft::WRL::ComPtr<IDWriteTextFormat>& format, ID2D1Brush* brush,
               DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING) {
    if (format.Get() == nullptr || brush == nullptr || text.empty() || !rect_valid(rect)) {
        return;
    }
    format->SetTextAlignment(alignment);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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
    if (format.Get() == nullptr || text.empty()) {
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

float measure_wrapped_height(IDWriteFactory* dwrite, IDWriteTextFormat* format,
                             const std::wstring& text, float width) {
    if (dwrite == nullptr || format == nullptr || text.empty() || width <= 0.0f) {
        return 0.0f;
    }
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                        format, width, 100000.0f,
                                        layout.GetAddressOf())) ||
        layout == nullptr) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) {
        return 0.0f;
    }
    return metrics.height;
}

std::wstring third_party_notice_text() {
    return L"SSCL 使用以下第三方组件。完整许可证文本见项目仓库的 "
           L"THIRD_PARTY_NOTICES.md 与 assets/icons/LICENSE-material-symbols.txt。\n\n"
           L"Material Symbols（Apache License 2.0）\n"
           L"Copyright © Google LLC\n"
           L"https://github.com/google/material-design-icons\n"
           L"用于界面图标；SVG 以 RCDATA 形式内嵌，运行时解析 path 并按主题色填充。\n\n"
           L"nlohmann/json（MIT License）\n"
           L"Copyright © 2013-2022 Niels Lohmann\n"
           L"https://github.com/nlohmann/json\n"
           L"用于配置、版本清单与平台 API 的 JSON 解析。\n\n"
           L"miniz（MIT License）\n"
           L"Copyright © 2013-2014 RAD Game Tools and Valve Software\n"
           L"Copyright © 2010-2014 Rich Geldreich and Tenacious Software LLC\n"
           L"https://github.com/richgel999/miniz\n"
           L"用于 ZIP/JAR 读取、整合包导入与导出。\n\n"
           L"doctest（MIT License）\n"
           L"Copyright © 2016-2023 Viktor Kirilov\n"
           L"https://github.com/doctest/doctest\n"
           L"仅用于开发期单元测试，不参与发布 EXE。\n\n"
           L"Windows 系统组件（Microsoft）\n"
           L"Direct2D、DirectWrite、Direct3D 11、DXGI、WIC、DWM、WinHTTP、"
           L"Crypt32、BCrypt 等为 Windows 系统组件，随 Windows 提供。\n\n"
           L"上述组件分别适用其自身许可证；SSCL 不修改其源码，仅在许可范围内"
           L"链接或调用。";
}

std::wstring privacy_policy_text() {
    return L"SSCL 隐私政策（适用于 0.1.0 Beta）\n"
           L"最后更新：2026-11-14\n\n"
           L"一、本地保存的数据\n"
           L"• config.json：启动器设置、窗口位置、版本隔离、Java 顺序等。\n"
           L"• accounts.dat：账号信息与登录令牌，使用 Windows DPAPI 加密，"
           L"仅当前 Windows 用户可解密。\n"
           L"• secrets.dat：你填写的 CurseForge API Key，使用 Windows DPAPI 加密。\n"
           L"• logs/：运行日志，用于排查下载、启动与登录问题；日志会对 "
           L"access_token、refresh_token、API Key 等字段脱敏。\n"
           L"• cache/、runtime/、minecraft/：图标/皮肤缓存、自动下载的 Java、"
           L"游戏文件与实例数据。\n\n"
           L"二、不会收集或上传的内容\n"
           L"SSCL 不包含遥测、广告、统计分析或自动崩溃上报；不会把本地日志、"
           L"游戏目录或账号数据上传到自建服务器。\n\n"
           L"三、网络请求\n"
           L"为完成登录、下载与资源查询，SSCL 会访问：\n"
           L"• Microsoft、Xbox Live、Minecraft Services（微软正版登录与档案）；\n"
           L"• Mojang 版本清单与资源（含 BMCLAPI 镜像）；\n"
           L"• Modrinth、CurseForge / MCIM（Mod 搜索、详情与下载）；\n"
           L"• authlib-injector 元数据、你填写的第三方认证服务器（外置登录）。\n"
           L"请求中可能包含账号令牌、应用 Client ID、User-Agent 与 IP 地址；"
           L"这些信息由对应服务按其隐私政策处理。\n\n"
           L"四、凭据与密码\n"
           L"微软 access/refresh token、第三方认证令牌与 CurseForge Key 仅以 "
           L"DPAPI 加密形式保存在本机，不写入日志。第三方登录密码只在登录请求中"
           L"使用，不会保存。\n\n"
           L"五、数据删除\n"
           L"删除数据目录中的 accounts.dat 与 secrets.dat 可清除本机账号与 Key；"
           L"删除整个 SSCL 数据目录可清除全部本地数据。删除 SSCL.exe 不会自动"
           L"删除数据目录。\n\n"
           L"六、第三方服务\n"
           L"SSCL 与 Microsoft、Mojang、Modrinth、CurseForge、BMCLAPI、"
           L"authlib-injector 及你指定的认证服务器无隶属关系；这些服务的数据处理"
           L"遵循各自隐私政策。\n\n"
           L"七、反馈\n"
           L"如对隐私处理有疑问，请通过项目发布页或负责人提供的联系方式反馈。";
}

// 单行省略：超宽时逐字回退并补 "…"（长路径必需，NO_WRAP + CLIP 会硬切）。
std::wstring elide(Ctx& ctx, const std::wstring& text,
                   const Microsoft::WRL::ComPtr<IDWriteTextFormat>& format, float max_width) {
    if (text.empty() || max_width <= 12.0f) {
        return text;
    }
    if (measure_text(ctx, text, format) <= max_width) {
        return text;
    }
    std::wstring result = text;
    while (result.size() > 1 && measure_text(ctx, result + L"…", format) > max_width) {
        result.pop_back();
    }
    return result + L"…";
}

// D-113：一段带颜色的文本。延迟数值按阈值分色，不能整行一个色。
struct TextRun {
    std::wstring text;
    ID2D1Brush* brush = nullptr;
};

// 依次水平排布若干段，超出 max_right 即停止（面板是定宽文本区，不做换行）。
void draw_runs(Ctx& ctx, const std::vector<TextRun>& runs, float x, float y, float max_right) {
    for (const TextRun& run : runs) {
        if (run.text.empty() || run.brush == nullptr) {
            continue;
        }
        if (x + 8.0f >= max_right) {
            break;
        }
        const float width = measure_text(ctx, run.text, ctx.f10);
        draw_text(ctx, run.text,
                  rect_of(x, y, (std::min)(x + width + 2.0f, max_right), y + kProbeLineHeightDip),
                  ctx.f10, run.brush);
        x += width;
    }
}

// D-115：Java 行右侧的四个操作符号（↑ ↓ − 文件夹），自绘以避免依赖图标资源。
void draw_glyph(Ctx& ctx, int kind, const D2D1_RECT_F& rect, ID2D1Brush* brush) {
    if (brush == nullptr || !rect_valid(rect)) {
        return;
    }
    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    constexpr float kArm = 4.5f;
    constexpr float kStroke = 1.5f;
    switch (kind) {
        case 0:  // 上移
            ctx.target->DrawLine(D2D1::Point2F(cx, cy - kArm), D2D1::Point2F(cx, cy + kArm),
                                 brush, kStroke);
            ctx.target->DrawLine(D2D1::Point2F(cx - kArm, cy - kArm * 0.2f),
                                 D2D1::Point2F(cx, cy - kArm), brush, kStroke);
            ctx.target->DrawLine(D2D1::Point2F(cx + kArm, cy - kArm * 0.2f),
                                 D2D1::Point2F(cx, cy - kArm), brush, kStroke);
            break;
        case 1:  // 下移
            ctx.target->DrawLine(D2D1::Point2F(cx, cy - kArm), D2D1::Point2F(cx, cy + kArm),
                                 brush, kStroke);
            ctx.target->DrawLine(D2D1::Point2F(cx - kArm, cy + kArm * 0.2f),
                                 D2D1::Point2F(cx, cy + kArm), brush, kStroke);
            ctx.target->DrawLine(D2D1::Point2F(cx + kArm, cy + kArm * 0.2f),
                                 D2D1::Point2F(cx, cy + kArm), brush, kStroke);
            break;
        case 2:  // 删除（减号）
            ctx.target->DrawLine(D2D1::Point2F(cx - kArm, cy), D2D1::Point2F(cx + kArm, cy),
                                 brush, kStroke);
            break;
        case 3:  // 打开所在文件夹
            ctx.target->DrawRectangle(
                D2D1::RectF(cx - kArm, cy - kArm * 0.55f, cx + kArm, cy + kArm * 0.85f), brush,
                kStroke);
            ctx.target->DrawLine(D2D1::Point2F(cx - kArm * 0.6f, cy - kArm * 0.55f),
                                 D2D1::Point2F(cx - kArm * 0.6f, cy - kArm * 0.95f), brush,
                                kStroke);
            break;
        default:
            break;
    }
}

// 延迟着色：<150ms 绿、>=150ms 橙、超时（total<0）红。
ID2D1Brush* latency_brush(long long ms, ID2D1Brush* good, ID2D1Brush* warn, ID2D1Brush* bad) {
    if (ms < 0) {
        return bad;
    }
    return ms < 150 ? good : warn;
}

void draw_button(Ctx& ctx, const D2D1_RECT_F& rect, const std::wstring& label, bool primary,
                 bool enabled = true, const char* icon = nullptr, bool hot = false) {
    (void)hot;
    // D-083：统一渐变三态
    const motion::StateColors state = motion::button_colors(primary, enabled, rect);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> stroke_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_brush;
    if (FAILED(ctx.target->CreateSolidColorBrush(state.fill, fill_brush.GetAddressOf())) ||
        FAILED(ctx.target->CreateSolidColorBrush(state.border, stroke_brush.GetAddressOf())) ||
        FAILED(ctx.target->CreateSolidColorBrush(state.text, text_brush.GetAddressOf()))) {
        return;
    }
    fill_round(ctx, rect, theme_radius(), fill_brush.Get());
    if (!primary) {
        stroke_round(ctx, rect, theme_radius(), stroke_brush.Get());
    }
    D2D1_RECT_F text_rect = rect;
    if (icon != nullptr && *icon != '\0') {
        const float icon_size = 15.0f;
        const float gap = 5.0f;
        const float text_width = measure_text(ctx, label, ctx.f11);
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
    draw_text(ctx, label, text_rect, ctx.f11, text_brush.Get(),
              (icon != nullptr && *icon != '\0') ? DWRITE_TEXT_ALIGNMENT_LEADING
                                                 : DWRITE_TEXT_ALIGNMENT_CENTER);
}

enum SettingKey {
    kIsolation = 1,
    kGameFolder,
    // Java（D-115）
    kJavaRefresh,
    kJavaRuntime,
    kScanDirAdd,
    kScanDirEntry,
    kTheme,
    kMotion,
    kVersion,
    kDataDir,
    kOpenData,
    kOpenLogs,
    kThirdParty,
    kPrivacy,
    // 启动选项（D-104）
    kWindowTitle,
    kCustomInfo,
    kLauncherVisibility,
    kProcessPriority,
    kWindowSize,
    kCurseForgeKey,
    // D-107 设置页「其他」
    kDownloadSource,
    kCommunitySource,
    kMemoryMode,
    kMemoryBar,
    kAutoChinese,
    // D-109 网络代理
    kProxyMode,
    kProxyHost,
    kProxyPort,
    kProxyTest,
    // D-111/D-113 网络诊断
    kNetworkDiagnose,
    kProbeLog,
};

constexpr float kTabTopDip = kNavHeightDip + 96.0f;
constexpr float kTabHeightDip = 40.0f;
constexpr float kTabGapDip = 2.0f;
constexpr float kRowHeightDip = 56.0f;
constexpr float kRowGapDip = 8.0f;
constexpr float kSectionHeightDip = 36.0f;
constexpr float kMemoryRowHeightDip = 132.0f;  // D-107 内存条专用行高

std::wstring trim_wide(std::wstring text) {
    const std::size_t begin = text.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(L" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// D-109：端口必须是 1-65535 的纯数字。
bool parse_proxy_port(const std::wstring& text, int* port) {
    if (port == nullptr) {
        return false;
    }
    const std::wstring value = trim_wide(text);
    if (value.empty()) {
        return false;
    }
    int result = 0;
    for (const wchar_t ch : value) {
        if (ch < L'0' || ch > L'9') {
            return false;
        }
        result = result * 10 + (ch - L'0');
        if (result > 65535) {
            return false;
        }
    }
    if (result < 1) {
        return false;
    }
    *port = result;
    return true;
}

// 去掉协议前缀 / 尾部斜杠 / 空白；空值回退 127.0.0.1。
std::string sanitize_proxy_host(const std::wstring& text) {
    std::string host = narrow(trim_wide(text));
    if (host.rfind("http://", 0) == 0) {
        host = host.substr(7);
    } else if (host.rfind("https://", 0) == 0) {
        host = host.substr(8);
    }
    while (!host.empty() && host.back() == '/') {
        host.pop_back();
    }
    return host.empty() ? "127.0.0.1" : host;
}

// D-113：网络诊断目标表。顺序即结果面板的显示顺序与后台回写下标。
struct ProbeTarget {
    const wchar_t* group;
    const wchar_t* name;
    const char* url;
};

constexpr ProbeTarget kProbeTargets[] = {
    {L"官方源", L"Mojang", "https://piston-meta.mojang.com/mc/game/version_manifest.json"},
    {L"官方源", L"Modrinth", "https://api.modrinth.com/"},
    {L"官方源", L"CurseForge", "https://api.curseforge.com/"},
    {L"镜像源", L"BMCLAPI", "https://bmclapi2.bangbang93.com/"},
    {L"镜像源", L"MCIM", "https://mod.mcimirror.top/"},
    {L"常见网站", L"百度", "https://www.baidu.com/"},
    {L"常见网站", L"腾讯", "https://www.qq.com/"},
    {L"常见网站", L"GitHub", "https://github.com/"},
    {L"常见网站", L"Google", "https://www.google.com/"},
};
constexpr std::size_t kProbeTargetCount =
    sizeof(kProbeTargets) / sizeof(kProbeTargets[0]);

// D-113：spinner 四帧循环，tick 每 120ms 前进一格。
constexpr wchar_t kProbeSpinner[] = L"|/-\\";
constexpr float kProbeSpinnerStepMs = 120.0f;

// D-112：分阶段延迟样本。connect = DNS + TCP（与 curl time_connect 同口径），
// total = 到响应头（含 TLS 握手与一次 HTTP 往返）。失败时两者都是 -1。
struct LatencySample {
    long long connect_ms = -1;
    long long total_ms = -1;
};

LatencySample measure_latency(const std::string& url, int timeout_ms) {
    const sscl::platform::HttpProbeTiming timing =
        sscl::platform::probe_http_timing(url, timeout_ms);
    LatencySample sample;
    if (!timing.error.empty() || timing.total_ms < 0) {
        return sample;
    }
    sample.connect_ms = timing.connect_ms;
    sample.total_ms = timing.total_ms;
    return sample;
}

// 经当前代理访问 IP 查询服务；两个候选，取第一个 200 响应。
std::wstring fetch_exit_ip() {
    for (const char* url : {"https://api.ipify.org", "https://ifconfig.me/ip"}) {
        sscl::platform::HttpRequest request;
        request.url = url;
        request.resolve_timeout_ms = 4000;
        request.connect_timeout_ms = 4000;
        request.receive_timeout_ms = 5000;
        const auto response = sscl::platform::http_request(request);
        if (response.is_error() || response.value().status_code != 200) {
            continue;
        }
        std::string body = response.value().body;
        std::size_t begin = 0;
        std::size_t end = body.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(body[begin])) != 0) {
            ++begin;
        }
        while (end > begin && std::isspace(static_cast<unsigned char>(body[end - 1])) != 0) {
            --end;
        }
        body = body.substr(begin, end - begin);
        if (!body.empty() && body.size() <= 64) {
            return widen(body);
        }
    }
    return L"获取失败";
}

// PCL 约定：自定义信息禁止引号字符（会破坏 version JSON / 命令行拼接）。
std::string sanitize_custom_info(const std::wstring& text) {
    std::wstring clean;
    for (const wchar_t ch : text) {
        if (ch != L'"' && ch != L'\'' && ch != L'`') {
            clean.push_back(ch);
        }
    }
    return narrow(trim_wide(clean));
}

// "1280×720" / "1280x720" / "1280*720" / "1280,720" / 空格分隔均可。
bool parse_window_size(const std::wstring& text, int* width, int* height) {
    if (width == nullptr || height == nullptr) {
        return false;
    }
    std::vector<int> numbers;
    int current = 0;
    bool has_digit = false;
    for (const wchar_t ch : text) {
        if (ch >= L'0' && ch <= L'9') {
            current = current * 10 + (ch - L'0');
            if (current > 100000) {
                return false;
            }
            has_digit = true;
        } else if (has_digit) {
            numbers.push_back(current);
            current = 0;
            has_digit = false;
            if (numbers.size() > 2) {
                return false;
            }
        }
    }
    if (has_digit) {
        numbers.push_back(current);
    }
    if (numbers.size() != 2) {
        return false;
    }
    if (numbers[0] < 320 || numbers[0] > 7680 || numbers[1] < 240 || numbers[1] > 4320) {
        return false;
    }
    *width = numbers[0];
    *height = numbers[1];
    return true;
}

// D-104：只有两项设置需要鼠标悬浮提示（其余说明已写在行内第二行）。
std::wstring row_tooltip(int key) {
    if (key == kIsolation) {
        return L"当安装新版本时，据此自动设置新版本的隔离选项。\n"
               L"若想调整已有版本的隔离策略，请前往它的版本设置。";
    }
    if (key == kWindowTitle) {
        return L"自定义游戏窗口的标题，若留空则不进行更改。";
    }
    return {};
}

}  // namespace

SettingsPage::SettingsPage(AppWindow* window, std::shared_ptr<sscl::core::GameService> service,
                           sscl::core::LauncherConfig config,
                           std::function<void(const sscl::core::LauncherConfig&)> on_config_changed)
    : m_window(window),
      m_service(std::move(service)),
      m_config(std::move(config)),
      m_on_config_changed(std::move(on_config_changed)) {
    if (m_service != nullptr) {
        m_game_dir = m_service->layout().game_dir;
    }
    // 数据目录固定为 EXE 同目录 SSCL\（§4.3），与游戏文件夹无关。
    m_data_dir = sscl::platform::data_dir();
    if (m_data_dir.empty()) {
        m_data_dir = std::filesystem::path(L"SSCL");
    }
    if (m_game_dir.empty()) {
        m_game_dir = m_data_dir / L"minecraft";
    }
    rebuild_tabs();
    rebuild_rows();
}

void SettingsPage::set_config(const sscl::core::LauncherConfig& config) {
    m_config = config;
    m_tooltip.clear();
    // D-115：首次拿到配置时后台探测一次 Java 版本（不阻塞构造/渲染）。
    if (!m_java_probe_started && !m_config.java_candidates.empty()) {
        m_java_probe_started = true;
        probe_java_candidates();
    }
    if (m_service != nullptr) {
        // 版本选择页可能切换过 MC 文件夹：游戏目录展示随之刷新。
        m_game_dir = m_service->layout().game_dir;
    }
    rebuild_rows();
}

void SettingsPage::set_message(std::wstring message) {
    m_message = std::move(message);
    if (request_repaint) {
        request_repaint();
    }
}

void SettingsPage::layout(UINT width, UINT height, UINT dpi) {
    m_to_dip = 96.0f / static_cast<float>(dpi == 0 ? 96 : dpi);
    m_width_dip = static_cast<float>(width) * m_to_dip;
    m_height_dip = static_cast<float>(height) * m_to_dip;

    m_content_left = kSidebarWidthDip + 24.0f;
    m_content_right = m_width_dip - 40.0f;
    if (m_content_right < m_content_left + 220.0f) {
        m_content_right = m_content_left + 220.0f;
    }
    m_view_top = kNavHeightDip + 62.0f;
    m_view_bottom = m_height_dip - 40.0f;
    if (m_view_bottom < m_view_top + 80.0f) {
        m_view_bottom = m_view_top + 80.0f;
    }

    rebuild_tabs();
    rebuild_rows();
    layout_overlay();
    layout_doc_overlay();
}

int SettingsPage::memory_slider_max_mb() const {
    const double total_mb =
        static_cast<double>(m_load.memory_total_bytes) / (1024.0 * 1024.0);
    int max_mb = total_mb > 2048.0 ? static_cast<int>(total_mb - 1024.0) : 1024;
    if (max_mb > 16384) {
        max_mb = 16384;
    }
    if (max_mb < 1024) {
        max_mb = 1024;
    }
    return (max_mb / 256) * 256;
}

int SettingsPage::effective_memory_mb() const {
    if (m_config.memory_mode == 1) {
        return (std::max)(512, (std::min)(16384, m_config.memory_custom_mb));
    }
    return sscl::platform::recommended_game_memory_mb();
}

void SettingsPage::set_memory_from_slider(float x_dip) {
    const float left = m_memory_slider_rect.left;
    const float right = m_memory_slider_rect.right;
    if (right <= left) {
        return;
    }
    const float ratio = (std::max)(0.0f, (std::min)(1.0f, (x_dip - left) / (right - left)));
    const int max_mb = memory_slider_max_mb();
    const int raw = 512 + static_cast<int>(ratio * static_cast<float>(max_mb - 512));
    const int value = (std::max)(512, (std::min)(max_mb, (raw / 256) * 256));
    if (value == m_config.memory_custom_mb) {
        return;
    }
    m_config.memory_custom_mb = value;
    if (request_repaint) {
        request_repaint();
    }
}

void SettingsPage::rebuild_tabs() {
    m_tabs.clear();
    const float left = kSidebarMarginDip;
    const float right = kSidebarWidthDip - kSidebarMarginDip;
    const Category categories[] = {Category::kGame,     Category::kJava,
                                   Category::kAppearance, Category::kNetwork,
                                   Category::kOther,    Category::kAbout};
    for (int i = 0; i < 6; ++i) {
        TabHit hit;
        hit.category = categories[i];
        const float top = kTabTopDip + static_cast<float>(i) * (kTabHeightDip + kTabGapDip);
        hit.rect = rect_of(left, top, right, top + kTabHeightDip);
        m_tabs.push_back(hit);
    }
}

void SettingsPage::rebuild_rows() {
    if (m_category != Category::kOther) {
        m_memory_slider_rect = {};
        m_memory_dragging = false;
    }
    struct Def {
        RowKind kind;
        int key;
        std::wstring text;
        float height = kRowHeightDip;  // D-107：内存条行更高
        int index = -1;                // D-115：列表项下标
    };
    std::vector<Def> defs;
    auto section = [&defs](const wchar_t* text) {
        defs.push_back(Def{RowKind::kSection, 0, text, kSectionHeightDip});
    };
    auto row = [&defs](RowKind kind, int key, float height = kRowHeightDip, int index = -1) {
        defs.push_back(Def{kind, key, {}, height, index});
    };

    switch (m_category) {
        case Category::kGame:
            section(L"启动选项");
            row(RowKind::kChoice, kIsolation);
            row(RowKind::kText, kWindowTitle);
            row(RowKind::kText, kCustomInfo);
            row(RowKind::kChoice, kLauncherVisibility);
            row(RowKind::kChoice, kProcessPriority);
            row(RowKind::kChoice, kWindowSize);
            section(L"版本与目录");
            row(RowKind::kAction, kGameFolder);
            break;
        case Category::kNetwork:
            section(L"网络代理");
            row(RowKind::kChoice, kProxyMode);
            row(RowKind::kText, kProxyHost);
            row(RowKind::kText, kProxyPort);
            row(RowKind::kAction, kProxyTest);
            section(L"下载及列表网络源");
            row(RowKind::kChoice, kDownloadSource);
            section(L"社区资源网络源");
            row(RowKind::kChoice, kCommunitySource);
            section(L"连接测试");
            row(RowKind::kAction, kNetworkDiagnose);
            row(RowKind::kProbeLog, kProbeLog, kProbeLogHeightDip);
            break;
        case Category::kOther:
            section(L"资源与凭据");
            row(RowKind::kText, kCurseForgeKey);
            section(L"内存分配");
            row(RowKind::kChoice, kMemoryMode);
            row(RowKind::kMemory, kMemoryBar, kMemoryRowHeightDip);
            section(L"辅助功能");
            row(RowKind::kToggle, kAutoChinese);
            break;
        case Category::kJava:
            section(L"运行时");
            row(RowKind::kAction, kJavaRefresh);
            for (std::size_t i = 0; i < m_config.java_candidates.size(); ++i) {
                row(RowKind::kJavaRuntime, kJavaRuntime, kJavaRowHeightDip,
                    static_cast<int>(i));
            }
            section(L"扫描位置");
            row(RowKind::kDualAction, kScanDirAdd);
            for (std::size_t i = 0; i < m_config.java_scan_dirs.size(); ++i) {
                row(RowKind::kScanDirEntry, kScanDirEntry, kJavaRowHeightDip,
                    static_cast<int>(i));
            }
            break;
        case Category::kAppearance:
            section(L"主题");
            row(RowKind::kChoice, kTheme);
            section(L"动效");
            row(RowKind::kChoice, kMotion);
            break;
        case Category::kAbout:
            section(L"程序");
            row(RowKind::kInfo, kVersion);
            row(RowKind::kInfo, kDataDir);
            row(RowKind::kAction, kOpenData);
            row(RowKind::kAction, kOpenLogs);
            section(L"说明与隐私");
            row(RowKind::kAction, kThirdParty);
            row(RowKind::kAction, kPrivacy);
            break;
    }

    float content = 0.0f;
    for (const Def& def : defs) {
        content += def.kind == RowKind::kSection ? kSectionHeightDip
                                                 : def.height + kRowGapDip;
    }
    m_scroll.layout(m_view_top, m_view_bottom, content, m_width_dip - 6.0f);
    const float offset = m_scroll.offset();

    const float left = m_content_left;
    const float right = m_content_right;

    m_rows.clear();
    float y = m_view_top + 4.0f - offset;
    for (const Def& def : defs) {
        RowHit hit;
        hit.kind = def.kind;
        hit.key = def.key;
        hit.index = def.index;
        hit.text = def.text;
        if (def.kind == RowKind::kSection) {
            hit.row = rect_of(left, y, right, y + kSectionHeightDip);
            y += kSectionHeightDip;
            m_rows.push_back(std::move(hit));
            continue;
        }
        hit.row = rect_of(left, y, right, y + def.height);
        if (def.kind == RowKind::kAction) {
            hit.control = rect_of(right - 100.0f, y + 14.0f, right - 18.0f, y + 42.0f);
        } else if (def.kind == RowKind::kChoice || def.kind == RowKind::kText) {
            hit.control = rect_of(right - 92.0f, y + 15.0f, right - 18.0f, y + 41.0f);
        } else if (def.kind == RowKind::kToggle) {
            hit.control = rect_of(right - 40.0f, y + 18.0f, right - 18.0f, y + 40.0f);
        } else if (def.kind == RowKind::kJavaRuntime) {
            // D-115：四个 26×26 图标按钮，右对齐、间距 4。
            constexpr float kButton = 26.0f;
            constexpr float kGap = 4.0f;
            float bx = right - 12.0f;
            const float by = y + (def.height - kButton) * 0.5f;
            for (int b = 3; b >= 0; --b) {
                hit.buttons[b] = rect_of(bx - kButton, by, bx, by + kButton);
                bx -= kButton + kGap;
            }
        } else if (def.kind == RowKind::kScanDirEntry) {
            hit.control = rect_of(right - 44.0f, y + (def.height - 26.0f) * 0.5f,
                                  right - 18.0f, y + (def.height - 26.0f) * 0.5f + 26.0f);
        } else if (def.kind == RowKind::kDualAction) {
            // D-115：[添加] [自动扫描] 两个 72 宽按钮并排右对齐。
            constexpr float kWidth = 72.0f;
            constexpr float kHeight = 28.0f;
            const float top = y + (def.height - kHeight) * 0.5f;
            hit.control2 = rect_of(right - 18.0f - kWidth, top, right - 18.0f, top + kHeight);
            hit.control = rect_of(hit.control2.left - 8.0f - kWidth, top,
                                  hit.control2.left - 8.0f, top + kHeight);
        }
        y += def.height + kRowGapDip;
        m_rows.push_back(std::move(hit));
    }
}

void SettingsPage::draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    Ctx ctx;
    if (!make_ctx(ctx, target, dwrite)) {
        return;
    }
    rebuild_tabs();
    rebuild_rows();

    // 页面底色与左侧栏必须与首页取同一语义色（Background / Secondary Surface）。
    // 此前用 Surface(#08060A) + Elevated(#17101A)，比首页的 #000000 + #100A12 整体偏亮，
    // 左侧栏在视觉上像是与首页不同的第二种侧栏；Elevated 按规范只用于弹窗/浮层。
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> page_bg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> sidebar_bg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> divider;
    if (FAILED(target->CreateSolidColorBrush(theme_background(), page_bg.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_sidebar_background(),
                                             sidebar_bg.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_border(), divider.GetAddressOf()))) {
        return;
    }
    divider->SetOpacity(0.6f);  // 与 home_page / task_page / version_page 保持一致
    ctx.target->FillRectangle(rect_of(0.0f, 0.0f, m_width_dip, m_height_dip), page_bg.Get());
    ctx.target->FillRectangle(rect_of(0.0f, kNavHeightDip, kSidebarWidthDip, m_height_dip),
                              sidebar_bg.Get());
    ctx.target->DrawLine(D2D1::Point2F(kSidebarWidthDip, kNavHeightDip),
                         D2D1::Point2F(kSidebarWidthDip, m_height_dip), divider.Get(), 1.0f);

    draw_text(ctx, L"设置", rect_of(kSidebarMarginDip, kNavHeightDip + 18.0f,
                                    kSidebarWidthDip - kSidebarMarginDip,
                                    kNavHeightDip + 54.0f),
              ctx.f20b, ctx.text.Get());
    draw_text(ctx, L"PCL 式分类设置 · 更改立即生效并写入 config.json",
              rect_of(kSidebarMarginDip, kNavHeightDip + 56.0f,
                      kSidebarWidthDip - kSidebarMarginDip, kNavHeightDip + 78.0f),
              ctx.f10, ctx.muted.Get());

    draw_text(ctx, category_label(m_category),
              rect_of(m_content_left, kNavHeightDip + 20.0f, m_content_right,
                      kNavHeightDip + 52.0f),
              ctx.f16b, ctx.text.Get());
    draw_text(ctx, L"全部更改自动保存",
              rect_of(m_content_left, kNavHeightDip + 20.0f, m_content_right,
                      kNavHeightDip + 52.0f),
              ctx.f10, ctx.muted.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING);

    draw_tabs(target, dwrite);
    draw_rows(target, dwrite);
    m_scroll.draw(target);

    if (!m_message.empty()) {
        draw_text(ctx, m_message,
                  rect_of(m_content_left, m_height_dip - 30.0f, m_content_right,
                          m_height_dip - 10.0f),
                  ctx.f10, ctx.muted.Get());
    }
    motion::menu().set_open(m_choice_open, 1);
    if (m_choice_open || motion::menu().visible_for(1)) {
        const bool menu_layer = motion::push_fade(target, motion::menu().opacity(),
                                                  motion::menu().offset_y());
        draw_choice(target, dwrite);
        if (menu_layer) {
            motion::pop_layer(target);
        }
    }
    if (m_doc_mode && !m_doc_open && !motion::modal().visible()) {
        m_doc_mode = false;  // 动效关闭时无淡出，立即恢复正常绘制
    }
    // 关闭淡出期间仍绘制卡片（内容保留至动画收敛），命中已由 m_input_open 放行。
    if (m_input_open || m_doc_mode || motion::modal().visible()) {
        if (m_doc_mode) {
            draw_doc_overlay(target, dwrite);
        } else {
            draw_overlay(target, dwrite);
        }
    }
    // D-104：悬浮提示画在最上层（内容 / 下拉 / 弹层之后）。
    m_tooltip.draw(target, dwrite, m_width_dip, m_height_dip);
}

void SettingsPage::draw_tabs(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    Ctx ctx;
    if (!make_ctx(ctx, target, dwrite)) {
        return;
    }
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
        const TabHit& tab = m_tabs[i];
        const bool selected = tab.category == m_category;
        if (selected) {
            // 选中态 = PrimaryContainer + Primary 边框（不只靠颜色，§三）。
            fill_round(ctx, tab.rect, 8.0f, ctx.primary_container.Get());
            stroke_round(ctx, tab.rect, 8.0f, ctx.primary.Get());
        }
        motion::draw_hot_overlay(ctx.target, tab.rect, 8.0f, theme_hover_background(),
                             theme_primary_container());
        draw_text(ctx, category_label(tab.category),
                  rect_of(tab.rect.left + 14.0f, tab.rect.top, tab.rect.right - 12.0f,
                          tab.rect.bottom),
                  selected ? ctx.f13b : ctx.f12,
                  selected ? ctx.text.Get() : ctx.secondary.Get());
    }
}

void SettingsPage::draw_rows(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    Ctx ctx;
    if (!make_ctx(ctx, target, dwrite)) {
        return;
    }
    target->PushAxisAlignedClip(
        rect_of(m_content_left - 6.0f, m_view_top, m_width_dip, m_view_bottom),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        const RowHit& hit = m_rows[i];
        if (hit.row.bottom < m_view_top || hit.row.top > m_view_bottom) {
            continue;
        }
        if (hit.kind == RowKind::kSection) {
            draw_text(ctx, hit.text,
                      rect_of(hit.row.left + 4.0f, hit.row.top + 4.0f, hit.row.right,
                              hit.row.top + 30.0f),
                      ctx.f13b, ctx.secondary.Get());
            continue;
        }
        if (hit.kind == RowKind::kJavaRuntime) {
            // D-115：Java 运行时行 —— 主版本 + 详细版本 + bin 完整路径 + 四个操作按钮。
            const bool row_hover = static_cast<int>(i) == m_hover_row;
            fill_round(ctx, hit.row, 8.0f, row_hover ? ctx.hover.Get() : ctx.elevated.Get());

            const bool has_index =
                hit.index >= 0 &&
                static_cast<std::size_t>(hit.index) < m_config.java_candidates.size();
            const std::size_t index =
                has_index ? static_cast<std::size_t>(hit.index) : std::size_t(0);
            std::wstring path;
            if (has_index) {
                path = widen(m_config.java_candidates[index]);
            }
            if (path.empty()) {
                path = L"—";
            } else if (path.back() != L'\\') {
                path += L'\\';
            }

            int major = 0;
            std::wstring detail;
            if (has_index && index < m_java_entries.size()) {
                major = m_java_entries[index].major;
                detail = widen(m_java_entries[index].version);
            }
            std::wstring text;
            if (major > 0) {
                text = L"Java " + std::to_wstring(major);
                if (!detail.empty()) {
                    text += L" (" + detail + L")";
                }
                text += L": " + path;
            } else if (m_java_scanning) {
                text = L"Java（检测中…）: " + path;
            } else {
                text = L"Java（未探测）: " + path;
            }

            const float text_right = hit.buttons[0].left - 10.0f;
            draw_text(ctx, elide(ctx, text, ctx.f11, text_right - (hit.row.left + 18.0f)),
                      rect_of(hit.row.left + 18.0f, hit.row.top, text_right, hit.row.bottom),
                      ctx.f11, ctx.text.Get());
            for (int b = 0; b < 4; ++b) {
                const bool hot = row_hover && m_hover_button == b + 3;
                if (hot) {
                    fill_round(ctx, hit.buttons[b], 5.0f, ctx.primary_container.Get());
                }
                draw_glyph(ctx, b, hit.buttons[b],
                           hot ? ctx.primary.Get() : ctx.secondary.Get());
            }
            continue;
        }
        if (hit.kind == RowKind::kScanDirEntry) {
            // D-115：扫描位置行 —— 路径 + 来源标签 + 删除。
            const bool row_hover = static_cast<int>(i) == m_hover_row;
            fill_round(ctx, hit.row, 8.0f, row_hover ? ctx.hover.Get() : ctx.elevated.Get());

            const bool has_index =
                hit.index >= 0 &&
                static_cast<std::size_t>(hit.index) < m_config.java_scan_dirs.size();
            const std::size_t index =
                has_index ? static_cast<std::size_t>(hit.index) : std::size_t(0);
            const std::wstring path =
                has_index ? widen(m_config.java_scan_dirs[index]) : std::wstring();
            bool auto_added = false;
            if (has_index) {
                auto_added =
                    std::find(m_config.java_scan_dirs_auto.begin(),
                              m_config.java_scan_dirs_auto.end(),
                              m_config.java_scan_dirs[index]) !=
                    m_config.java_scan_dirs_auto.end();
            }

            const float tag_right = hit.control.left - 10.0f;
            const float tag_left = tag_right - 64.0f;
            const float text_right = tag_left - 8.0f;
            draw_text(ctx, elide(ctx, path, ctx.f11, text_right - (hit.row.left + 18.0f)),
                      rect_of(hit.row.left + 18.0f, hit.row.top, text_right, hit.row.bottom),
                      ctx.f11, ctx.text.Get());
            draw_text(ctx, auto_added ? L"程序添加" : L"手动添加",
                      rect_of(tag_left, hit.row.top, tag_right, hit.row.bottom), ctx.f10,
                      ctx.muted.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
            const bool hot = row_hover && m_hover_button == 1;
            if (hot) {
                fill_round(ctx, hit.control, 5.0f, ctx.primary_container.Get());
            }
            draw_glyph(ctx, 2, hit.control, hot ? ctx.primary.Get() : ctx.secondary.Get());
            continue;
        }
        if (hit.kind == RowKind::kProbeLog) {
            // D-113：纯底色文本框。未测试=占位文案；测试中=spinner + 逐项填充；
            // 完成=按组列出，延迟数值按 150ms 阈值分色（绿/橙/红）。
            const D2D1_RECT_F box = rect_of(hit.row.left + 18.0f, hit.row.top,
                                            hit.row.right - 18.0f, hit.row.bottom - 6.0f);
            fill_round(ctx, box, 8.0f, ctx.surface.Get());

            const float pad = 14.0f;
            const float left = box.left + pad;
            const float max_right = box.right - pad;
            const float group_width = 68.0f;  // 组名列宽，保证各行的值竖向对齐
            float y = box.top + pad;

            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_good;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_warn;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_bad;
            target->CreateSolidColorBrush(theme_success(), brush_good.GetAddressOf());
            target->CreateSolidColorBrush(theme_warning(), brush_warn.GetAddressOf());
            target->CreateSolidColorBrush(theme_danger(), brush_bad.GetAddressOf());

            if (!m_net.started) {
                draw_text(ctx, L"未进行测试",
                          rect_of(left, y, max_right, y + kProbeLineHeightDip), ctx.f10,
                          ctx.muted.Get());
                continue;
            }

            if (m_net.running) {
                const std::wstring frame(1, kProbeSpinner[m_probe_spinner % 4]);
                const float frame_width = measure_text(ctx, frame, ctx.f10);
                draw_text(ctx, frame,
                          rect_of(left, y, left + frame_width + 2.0f, y + kProbeLineHeightDip),
                          ctx.f10, ctx.primary.Get());
                draw_text(ctx, L" 测试中",
                          rect_of(left + frame_width + 2.0f, y, max_right,
                                  y + kProbeLineHeightDip),
                          ctx.f10, ctx.text.Get());
                y += kProbeLineHeightDip;
            }

            // 按组输出；测试中只显示已有结果的组，未完成的项留待后续帧填充。
            for (std::size_t i = 0; i < kProbeTargetCount;) {
                const std::wstring group = kProbeTargets[i].group;
                std::size_t group_end = i;
                while (group_end < kProbeTargetCount &&
                       std::wstring(kProbeTargets[group_end].group) == group) {
                    ++group_end;
                }
                bool any_done = false;
                for (std::size_t j = i; j < group_end; ++j) {
                    if (j < m_net.entries.size() && m_net.entries[j].done) {
                        any_done = true;
                    }
                }
                if (m_net.running && !any_done) {
                    i = group_end;
                    continue;
                }

                draw_text(ctx, group,
                          rect_of(left, y, left + group_width - 8.0f, y + kProbeLineHeightDip),
                          ctx.f10, ctx.muted.Get());

                std::vector<TextRun> runs;
                for (std::size_t j = i; j < group_end; ++j) {
                    const bool done = j < m_net.entries.size() && m_net.entries[j].done;
                    if (m_net.running && !done) {
                        continue;
                    }
                    const long long connect = done ? m_net.entries[j].connect_ms : -1;
                    const long long total = done ? m_net.entries[j].total_ms : -1;
                    if (!runs.empty()) {
                        runs.push_back({L"  ·  ", ctx.muted.Get()});
                    }
                    runs.push_back({std::wstring(kProbeTargets[j].name) + L" ",
                                    ctx.secondary.Get()});
                    if (total < 0) {
                        runs.push_back({L"超时", brush_bad.Get()});
                        continue;
                    }
                    // 连接段与首字节各自按 150ms 判定；没有连接样本时退回首字节。
                    const long long connect_value = connect >= 0 ? connect : total;
                    runs.push_back({std::to_wstring(connect_value),
                                    latency_brush(connect_value, brush_good.Get(),
                                                  brush_warn.Get(), brush_bad.Get())});
                    runs.push_back({L"/", ctx.muted.Get()});
                    runs.push_back({std::to_wstring(total) + L" ms",
                                    latency_brush(total, brush_good.Get(), brush_warn.Get(),
                                                  brush_bad.Get())});
                }
                draw_runs(ctx, runs, left + group_width, y, max_right);
                y += kProbeLineHeightDip;
                i = group_end;
            }

            if (!m_net.running && m_net.exit_ip_done) {
                draw_text(ctx, L"出口 IP",
                          rect_of(left, y, left + group_width - 8.0f, y + kProbeLineHeightDip),
                          ctx.f10, ctx.muted.Get());
                const bool ok = !m_net.exit_ip.empty() && m_net.exit_ip != L"获取失败";
                draw_text(ctx, ok ? m_net.exit_ip : L"获取失败",
                          rect_of(left + group_width, y, max_right, y + kProbeLineHeightDip),
                          ctx.f10, ok ? ctx.text.Get() : brush_bad.Get());
            }
            continue;
        }
        if (hit.kind == RowKind::kMemory) {
            // D-107：每秒刷新的分段内存条（深蓝=系统已用，浅蓝=游戏分配）。
            constexpr double kGb = 1024.0 * 1024.0 * 1024.0;
            const double total_gb = static_cast<double>(m_load.memory_total_bytes) / kGb;
            const double used_gb = static_cast<double>(m_load.memory_used_bytes) / kGb;
            const double alloc_gb = static_cast<double>(effective_memory_mb()) / 1024.0;
            const float left = hit.row.left + 18.0f;
            const float right = hit.row.right - 18.0f;
            const float mid = left + (right - left) * 0.48f;
            const float top = hit.row.top;
            draw_text(ctx, L"已使用内存",
                      rect_of(left, top + 4.0f, mid - 10.0f, top + 24.0f), ctx.f10,
                      ctx.muted.Get());
            draw_text(ctx, L"游戏分配",
                      rect_of(mid, top + 4.0f, right, top + 24.0f), ctx.f10, ctx.muted.Get());
            draw_text(ctx, format_gb(used_gb) + L" / " + format_gb(total_gb),
                      rect_of(left, top + 24.0f, mid - 10.0f, top + 50.0f), ctx.f16b,
                      ctx.text.Get());
            draw_text(ctx, format_gb(alloc_gb),
                      rect_of(mid, top + 24.0f, right, top + 50.0f), ctx.f16b, ctx.text.Get());

            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> used_brush;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> alloc_brush;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> track_brush;
            D2D1_COLOR_F primary = theme_primary();
            D2D1_COLOR_F soft = primary;
            soft.a = 0.42f;
            D2D1_COLOR_F track_color = theme_border();
            track_color.a = 0.6f;
            target->CreateSolidColorBrush(track_color, track_brush.GetAddressOf());
            target->CreateSolidColorBrush(primary, used_brush.GetAddressOf());
            target->CreateSolidColorBrush(soft, alloc_brush.GetAddressOf());
            const D2D1_RECT_F bar = rect_of(left, top + 56.0f, right, top + 66.0f);
            fill_round(ctx, bar, 5.0f, track_brush.Get());
            const float width = bar.right - bar.left;
            const float used_ratio = total_gb > 0.0
                                         ? (std::max)(0.0f, (std::min)(1.0f,
                                               static_cast<float>(used_gb / total_gb)))
                                         : 0.0f;
            const float alloc_ratio = total_gb > 0.0
                                          ? (std::max)(0.0f, (std::min)(1.0f,
                                                static_cast<float>(alloc_gb / total_gb)))
                                          : 0.0f;
            const float used_end = bar.left + width * used_ratio;
            const float alloc_end = (std::min)(bar.right, used_end + width * alloc_ratio);
            if (used_end > bar.left) {
                fill_round(ctx, rect_of(bar.left, bar.top, used_end, bar.bottom), 5.0f,
                           used_brush.Get());
            }
            if (alloc_end > used_end) {
                fill_round(ctx, rect_of(used_end, bar.top, alloc_end, bar.bottom), 5.0f,
                           alloc_brush.Get());
            }

            if (m_config.memory_mode == 1) {
                m_memory_slider_rect = rect_of(left, top + 88.0f, right, top + 106.0f);
                const float cy =
                    (m_memory_slider_rect.top + m_memory_slider_rect.bottom) * 0.5f;
                fill_round(ctx, rect_of(left, cy - 3.0f, right, cy + 3.0f), 3.0f,
                           track_brush.Get());
                const int max_mb = memory_slider_max_mb();
                const int value =
                    (std::max)(512, (std::min)(max_mb, effective_memory_mb()));
                const float ratio =
                    max_mb > 512
                        ? static_cast<float>(value - 512) / static_cast<float>(max_mb - 512)
                        : 0.0f;
                const float cx = left + (std::max)(0.0f, (std::min)(1.0f, ratio)) *
                                           (right - left);
                fill_round(ctx, rect_of(left, cy - 3.0f, cx, cy + 3.0f), 3.0f,
                           used_brush.Get());
                if (used_brush.Get() != nullptr) {
                    const D2D1_ELLIPSE knob =
                        D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.0f, 8.0f);
                    target->FillEllipse(&knob, used_brush.Get());
                }
                draw_text(ctx, std::to_wstring(value) + L" MB",
                          rect_of(left, top + 108.0f, right, top + 128.0f), ctx.f10,
                          ctx.secondary.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
            } else {
                m_memory_slider_rect = {};
                draw_text(ctx, L"自动配置：按可用物理内存动态分配（" + format_gb(alloc_gb) +
                                   L"）",
                          rect_of(left, top + 88.0f, right, top + 112.0f), ctx.f10,
                          ctx.muted.Get());
            }
            continue;
        }
        // 列表行：默认 Elevated；悬停 = Hover Background（§三 交互状态）。
        const bool row_hover = static_cast<int>(i) == m_hover_row;
        fill_round(ctx, hit.row, 6.0f, ctx.elevated.Get());
        motion::draw_hot_overlay(ctx.target, hit.row, 6.0f, theme_hover_background(),
                             theme_primary_container());
        // 同上：浅色 Elevated=#FFFFFF，无描边会与页面底色糊在一起。
        stroke_round(ctx, hit.row, 6.0f, ctx.border.Get());
        // kInfo 行没有控件：文字区右边距用行右边界，否则会退化成无效矩形。
        const float text_right =
            hit.kind == RowKind::kInfo ? hit.row.right - 18.0f : hit.control.left - 12.0f;
        draw_text(ctx, row_label(hit.key),
                  rect_of(hit.row.left + 18.0f, hit.row.top + 8.0f, text_right,
                          hit.row.top + 30.0f),
                  ctx.f12, ctx.text.Get());
        draw_text(ctx, row_desc(hit.key),
                  rect_of(hit.row.left + 18.0f, hit.row.top + 28.0f, text_right,
                          hit.row.top + 48.0f),
                  ctx.f10, ctx.muted.Get());

        const float value_left = hit.row.left + 190.0f;
        const float value_right = hit.control.left - 10.0f;

        if (hit.kind == RowKind::kChoice || hit.kind == RowKind::kText) {
            const std::wstring value =
                elide(ctx, row_value(hit.key), ctx.f11, value_right - value_left);
            draw_text(ctx, value,
                      rect_of(value_left, hit.row.top + 15.0f, value_right, hit.row.top + 41.0f),
                      ctx.f11, ctx.secondary.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
            draw_button(ctx, hit.control, hit.kind == RowKind::kChoice ? L"更改" : L"修改",
                        false, true, nullptr, row_hover && m_hover_button == 1);
        } else if (hit.kind == RowKind::kAction) {
            draw_button(ctx, hit.control, row_action(hit.key), false, true, nullptr,
                        row_hover && m_hover_button == 1);
        } else if (hit.kind == RowKind::kDualAction) {
            // D-115：[添加] 手动选目录；[自动扫描] 按常见位置自动补充。
            draw_button(ctx, hit.control, L"添加", false, true, nullptr,
                        row_hover && m_hover_button == 1);
            draw_button(ctx, hit.control2, L"自动扫描", false, true, nullptr,
                        row_hover && m_hover_button == 2);
        } else if (hit.kind == RowKind::kInfo) {
            const std::wstring value = elide(ctx, row_value(hit.key), ctx.f11,
                                             hit.row.right - 18.0f - value_left);
            // 只读值放标题行（与 label 同带），说明文字独占下一行，避免长路径压住说明。
            draw_text(ctx, value,
                      rect_of(value_left, hit.row.top + 8.0f, hit.row.right - 18.0f,
                              hit.row.top + 30.0f),
                      ctx.f11, ctx.secondary.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else if (hit.kind == RowKind::kToggle) {
            // D-107：勾选框；默认勾选「自动设置游戏为中文（简体）」。
            const bool on = m_config.auto_chinese_language;
            fill_round(ctx, hit.control, 4.0f, on ? ctx.primary.Get() : ctx.surface.Get());
            stroke_round(ctx, hit.control, 4.0f, ctx.border.Get());
            if (on) {
                IconManager::instance().draw(
                    target, "check",
                    rect_of(hit.control.left + 3.0f, hit.control.top + 3.0f,
                            hit.control.right - 3.0f, hit.control.bottom - 3.0f),
                    theme_text_on_primary());
            }
        }
    }
    target->PopAxisAlignedClip();
}

void SettingsPage::draw_choice(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    Ctx ctx;
    if (!make_ctx(ctx, target, dwrite)) {
        return;
    }
    fill_round(ctx, m_choice_rect, 8.0f, ctx.elevated.Get());
    stroke_round(ctx, m_choice_rect, 8.0f, ctx.border.Get());
    for (std::size_t i = 0; i < m_choice_labels.size(); ++i) {
        const D2D1_RECT_F item =
            rect_of(m_choice_rect.left + 4.0f, m_choice_rect.top + 4.0f + static_cast<float>(i) * 28.0f,
                    m_choice_rect.right - 4.0f,
                    m_choice_rect.top + 4.0f + static_cast<float>(i + 1) * 28.0f);
        motion::draw_hot_overlay(ctx.target, item, 5.0f, theme_hover_background(),
                             theme_primary_container());
        const bool current =
            i < m_choice_values.size() && m_choice_values[i] == row_value_int(m_choice_key);
        draw_text(ctx, m_choice_labels[i],
                  rect_of(item.left + 10.0f, item.top, item.right - 26.0f, item.bottom),
                  ctx.f11, current ? ctx.text.Get() : ctx.secondary.Get());
        if (current) {
            const float center_y = (item.top + item.bottom) * 0.5f;
            IconManager::instance().draw(
                target, "check",
                rect_of(item.right - 24.0f, center_y - 7.0f, item.right - 10.0f, center_y + 7.0f),
                theme_primary());
        }
    }
}

void SettingsPage::draw_overlay(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    Ctx ctx;
    if (!make_ctx(ctx, target, dwrite)) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dim;
    if (SUCCEEDED(target->CreateSolidColorBrush(
            D2D1::ColorF(0.0f, 0.0f, 0.0f, motion::modal().dim()), dim.GetAddressOf()))) {
        target->FillRectangle(rect_of(0, 0, m_width_dip, m_height_dip), dim.Get());
    }
    const bool layered = motion::push_pop(target, motion::modal(), m_overlay_card);
    fill_round(ctx, m_overlay_card, 8.0f, ctx.elevated.Get());
    stroke_round(ctx, m_overlay_card, 8.0f, ctx.border.Get());
    draw_text(ctx, m_overlay_prompt,
              rect_of(m_overlay_card.left + 20.0f, m_overlay_card.top + 14.0f,
                      m_overlay_card.right - 20.0f, m_overlay_card.top + 52.0f),
              ctx.f13b, ctx.text.Get());
    if (m_input_box) {
        m_input_box->draw(target, dwrite);
    }
    draw_button(ctx, m_overlay_cancel, L"取消", false);
    draw_button(ctx, m_overlay_ok, L"确定", true, true, "check");
    if (layered) {
        motion::pop_pop(target);
    }
}

void SettingsPage::layout_overlay() {
    const float width = 460.0f;
    const float height = 190.0f;
    const float cx = m_width_dip * 0.5f;
    const float cy = m_height_dip * 0.5f;
    m_overlay_card = rect_of(cx - width * 0.5f, cy - height * 0.5f, cx + width * 0.5f,
                             cy + height * 0.5f);
    m_overlay_cancel = rect_of(m_overlay_card.right - 200.0f, m_overlay_card.bottom - 50.0f,
                               m_overlay_card.right - 116.0f, m_overlay_card.bottom - 18.0f);
    m_overlay_ok = rect_of(m_overlay_card.right - 100.0f, m_overlay_card.bottom - 50.0f,
                           m_overlay_card.right - 20.0f, m_overlay_card.bottom - 18.0f);
    if (m_input_box) {
        m_input_box->bounds = rect_of(m_overlay_card.left + 20.0f, m_overlay_card.top + 66.0f,
                                      m_overlay_card.right - 20.0f, m_overlay_card.top + 104.0f);
    }
}

void SettingsPage::open_doc(int key) {
    if (key == kThirdParty) {
        m_doc_title = L"第三方说明";
        m_doc_text = third_party_notice_text();
    } else {
        m_doc_title = L"隐私政策";
        m_doc_text = privacy_policy_text();
    }
    m_input_open = false;
    m_choice_open = false;
    m_doc_mode = true;
    m_doc_open = true;
    m_doc_content_height = 0.0f;
    m_doc_scroll.jump_to(0.0f);
    layout_doc_overlay();
    motion::modal().open();
    if (request_repaint) {
        request_repaint();
    }
}

void SettingsPage::close_doc_overlay() {
    m_doc_open = false;
    motion::modal().close();
    if (request_repaint) {
        request_repaint();
    }
}

void SettingsPage::layout_doc_overlay() {
    // 弹层属于设置页内容区：应在顶部导航条以下、侧栏右侧居中。
    // 若按整个窗口居中，560 高的卡片会顶到导航条（视觉上“向上超出”），
    // 在宽窗口里也会相对内容区偏左。
    const float area_left = m_content_left > 0.0f ? m_content_left : 24.0f;
    const float area_right = m_content_right > area_left ? m_content_right : m_width_dip - 24.0f;
    const float area_top = kNavHeightDip + 8.0f;
    const float area_bottom = (std::max)(area_top + 240.0f, m_height_dip - 16.0f);
    const float area_w = area_right - area_left;
    const float area_h = area_bottom - area_top;
    const float width = (std::min)(720.0f, (std::max)(360.0f, area_w - 32.0f));
    const float height = (std::min)(560.0f, (std::max)(320.0f, area_h - 24.0f));
    const float cx = (area_left + area_right) * 0.5f;
    const float cy = (area_top + area_bottom) * 0.5f;
    m_overlay_card = rect_of(cx - width * 0.5f, cy - height * 0.5f,
                             cx + width * 0.5f, cy + height * 0.5f);
    m_doc_view = rect_of(m_overlay_card.left + 24.0f, m_overlay_card.top + 64.0f,
                         m_overlay_card.right - 24.0f, m_overlay_card.bottom - 64.0f);
    m_doc_close = rect_of(m_overlay_card.right - 120.0f, m_overlay_card.bottom - 48.0f,
                          m_overlay_card.right - 24.0f, m_overlay_card.bottom - 16.0f);
}

void SettingsPage::draw_doc_overlay(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    Ctx ctx;
    if (!make_ctx(ctx, target, dwrite)) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dim;
    if (SUCCEEDED(target->CreateSolidColorBrush(
            D2D1::ColorF(0.0f, 0.0f, 0.0f, motion::modal().dim()), dim.GetAddressOf()))) {
        target->FillRectangle(rect_of(0, 0, m_width_dip, m_height_dip), dim.Get());
    }
    const bool layered = motion::push_pop(target, motion::modal(), m_overlay_card);
    fill_round(ctx, m_overlay_card, 8.0f, ctx.elevated.Get());
    stroke_round(ctx, m_overlay_card, 8.0f, ctx.border.Get());
    draw_text(ctx, m_doc_title,
              rect_of(m_overlay_card.left + 24.0f, m_overlay_card.top + 14.0f,
                      m_overlay_card.right - 24.0f, m_overlay_card.top + 54.0f),
              ctx.f16b, ctx.text.Get());
    if (ctx.border) {
        target->DrawLine(
            D2D1::Point2F(m_overlay_card.left + 24.0f, m_overlay_card.top + 58.0f),
            D2D1::Point2F(m_overlay_card.right - 24.0f, m_overlay_card.top + 58.0f),
            ctx.border.Get(), 1.0f);
    }

    Microsoft::WRL::ComPtr<IDWriteTextFormat> doc_format =
        make_format(ctx.dwrite, 11.0f, DWRITE_FONT_WEIGHT_NORMAL);
    if (doc_format && rect_valid(m_doc_view)) {
        const float text_width =
            (std::max)(80.0f, m_doc_view.right - m_doc_view.left - 16.0f);
        m_doc_content_height =
            measure_wrapped_height(ctx.dwrite, doc_format.Get(), m_doc_text, text_width);
        m_doc_scroll.layout(m_doc_view.top, m_doc_view.bottom, m_doc_content_height,
                            m_doc_view.right - 4.0f);
        const float offset = m_doc_scroll.offset();
        doc_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        doc_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        doc_format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        target->PushAxisAlignedClip(m_doc_view, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const D2D1_RECT_F text_rect =
            rect_of(m_doc_view.left, m_doc_view.top - offset,
                    m_doc_view.left + text_width,
                    m_doc_view.top - offset + m_doc_content_height + 24.0f);
        target->DrawTextW(m_doc_text.c_str(), static_cast<UINT32>(m_doc_text.size()),
                          doc_format.Get(), text_rect, ctx.secondary.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        target->PopAxisAlignedClip();
        m_doc_scroll.draw(target);
    }
    draw_button(ctx, m_doc_close, L"关闭", true, true, "close");
    if (layered) {
        motion::pop_pop(target);
    }
}

void SettingsPage::open_input(std::wstring prompt, std::wstring initial,
                              std::function<void(const std::wstring&)> accept) {
    m_doc_mode = false;
    m_doc_open = false;
    m_input_open = true;
    m_overlay_prompt = std::move(prompt);
    m_input_accept = std::move(accept);
    m_input_box = std::make_unique<TextBox>(std::move(initial), 12.0f);
    m_input_box->max_length = 512;
    m_input_box->set_focused(true);
    layout_overlay();
    motion::modal().open();  // 弹出：淡入 + 0.96→1 + 上移 8px（D-081）
    if (request_repaint) {
        request_repaint();
    }
}

void SettingsPage::close_overlay() {
    m_input_open = false;
    motion::modal().close();
    // 内容保留到淡出结束（下次 open 时覆盖），避免出现空白卡片。
    if (request_repaint) {
        request_repaint();
    }
}

void SettingsPage::open_choice(int key) {
    const std::vector<std::pair<int, std::wstring>> items = choice_items(key);
    if (items.empty()) {
        return;
    }
    m_choice_key = key;
    m_choice_open = true;
    // D-105：动画状态必须在事件里就启动。只靠 draw() 里的 set_open 时，
    // WM_PAINT「先 tick 再 draw」会让第一帧的 update_all 看不到菜单动画，
    // 帧循环立刻收敛 → 下拉停在 opacity≈0，必须移动鼠标才出现。
    motion::menu().set_open(true, 1);
    m_choice_hover = -1;
    m_choice_values.clear();
    m_choice_labels.clear();
    for (const auto& item : items) {
        m_choice_values.push_back(item.first);
        m_choice_labels.push_back(item.second);
    }

    D2D1_RECT_F control{};
    for (const RowHit& hit : m_rows) {
        if (hit.key == key) {
            control = hit.control;
            break;
        }
    }
    const float height = static_cast<float>(m_choice_labels.size()) * 28.0f + 8.0f;
    float top = control.bottom + 4.0f;
    if (top + height > m_height_dip - 12.0f) {
        top = control.top - height - 4.0f;
    }
    // D-104：长文案下拉自动加宽（可见性 / 优先级说明最长）。
    float menu_width = 220.0f;
    if (key == kLauncherVisibility || key == kProcessPriority) {
        menu_width = 340.0f;
    } else if (key == kProxyMode) {
        menu_width = 320.0f;  // D-109：「不使用任何代理（全部直连）」较长
    } else if (key == kDownloadSource || key == kCommunitySource) {
        menu_width = 380.0f;  // D-107：网络源文案最长
    } else if (key == kMemoryMode) {
        menu_width = 240.0f;
    } else if (key == kIsolation || key == kWindowSize) {
        menu_width = 300.0f;
    }
    float left = control.right - menu_width;
    if (left < m_content_left) {
        left = m_content_left;
    }
    if (left + menu_width > m_content_right) {
        left = m_content_right - menu_width;
    }
    m_choice_rect = rect_of(left, top, left + menu_width, top + height);
    if (request_repaint) {
        request_repaint();
    }
}

std::vector<std::pair<int, std::wstring>> SettingsPage::choice_items(int key) const {
    std::vector<std::pair<int, std::wstring>> items;
    if (key == kIsolation) {
        items = {{0, L"关闭"},
                 {1, L"隔离可安装 Mod 的版本"},
                 {2, L"隔离非正式版"},
                 {3, L"隔离可安装 Mod 的版本与非正式版"},
                 {4, L"隔离所有版本"}};
    } else if (key == kLauncherVisibility) {
        items = {{0, L"游戏启动后仍保持不变"},
                 {1, L"游戏启动后立即关闭"},
                 {2, L"隐藏，游戏退出后自动关闭"},
                 {3, L"隐藏，游戏退出后重新打开"},
                 {4, L"游戏启动后最小化"}};
    } else if (key == kProcessPriority) {
        items = {{1, L"中（平衡）"},
                 {2, L"高（优先保证游戏运行）"},
                 {0, L"低（适合挂机时使用）"}};
    } else if (key == kWindowSize) {
        items = {{0, L"默认"},
                 {1, L"全屏"},
                 {2, L"与启动器尺寸一致"},
                 {3, L"自定义尺寸"},
                 {4, L"最大化"}};
    } else if (key == kProxyMode) {
        items = {{0, L"使用系统代理"},
                 {1, L"使用 HTTP 代理"},
                 {2, L"不使用任何代理（全部直连）"}};
    } else if (key == kDownloadSource) {
        items = {{0, L"尽量使用镜像源"},
                 {1, L"优先使用官方源，在加载缓慢时换用镜像源"},
                 {2, L"尽量使用官方源"}};
    } else if (key == kCommunitySource) {
        items = {{0, L"尽量使用镜像源（可能缺少刚刚更新的版本）"},
                 {1, L"仅在官方源加载缓慢时改用镜像源"},
                 {2, L"尽量使用官方源"}};
    } else if (key == kMemoryMode) {
        items = {{0, L"自动配置"}, {1, L"自定义"}};
    } else if (key == kTheme) {
        items = {{0, L"跟随系统"}, {1, L"浅色"}, {2, L"深色"}};
    } else if (key == kMotion) {
        items = {{0, L"完整"},
                 {1, L"柔和"},
                 {2, L"精简"},
                 {3, L"关闭"}};
    }
    return items;
}

int SettingsPage::row_value_int(int key) const {
    if (key == kIsolation) {
        return m_config.version_isolation;
    }
    if (key == kLauncherVisibility) {
        return m_config.launcher_visibility;
    }
    if (key == kProcessPriority) {
        return m_config.process_priority;
    }
    if (key == kWindowSize) {
        return m_config.window_size_mode;
    }
    if (key == kProxyMode) {
        return m_config.proxy_mode;
    }
    if (key == kDownloadSource) {
        return m_config.download_source;
    }
    if (key == kCommunitySource) {
        return m_config.community_source;
    }
    if (key == kMemoryMode) {
        return m_config.memory_mode;
    }
    if (key == kTheme) {
        if (m_config.theme_mode == "light") {
            return 1;
        }
        if (m_config.theme_mode == "dark") {
            return 2;
        }
        return 0;
    }
    if (key == kMotion) {
        return static_cast<int>(sscl::ui::motion::level_from_key(m_config.motion_level));
    }
    return 0;
}

void SettingsPage::apply_choice(int key, int value) {
    if (key == kIsolation) {
        if (value < 0 || value > 4) {
            return;
        }
        m_config.version_isolation = value;
    } else if (key == kLauncherVisibility) {
        if (value < 0 || value > 4) {
            return;
        }
        m_config.launcher_visibility = value;
    } else if (key == kProcessPriority) {
        if (value < 0 || value > 2) {
            return;
        }
        m_config.process_priority = value;
    } else if (key == kWindowSize) {
        if (value < 0 || value > 4) {
            return;
        }
        m_config.window_size_mode = value;
    } else if (key == kProxyMode) {
        if (value < 0 || value > 2) {
            return;
        }
        m_config.proxy_mode = value;
    } else if (key == kDownloadSource) {
        if (value < 0 || value > 2) {
            return;
        }
        m_config.download_source = value;
    } else if (key == kCommunitySource) {
        if (value < 0 || value > 2) {
            return;
        }
        m_config.community_source = value;
    } else if (key == kMemoryMode) {
        if (value != 0 && value != 1) {
            return;
        }
        m_config.memory_mode = value;
    } else if (key == kTheme) {
        const char* keys[] = {"system", "light", "dark"};
        if (value < 0 || value > 2) {
            return;
        }
        m_config.theme_mode = keys[value];
        sscl::ui::apply_theme_preference(m_config.theme_mode);
    } else if (key == kMotion) {
        if (value < 0 || value > 3) {
            return;
        }
        const sscl::ui::motion::Level level = static_cast<sscl::ui::motion::Level>(value);
        m_config.motion_level = sscl::ui::motion::level_key(level);
        sscl::ui::motion::set_level(level);  // 立即生效（下一个动画帧即按新强度）
    } else {
        return;
    }
    commit();
    // D-109：切到「使用 HTTP 代理」后提示用户补全地址与端口。
    if (key == kProxyMode && value == 1) {
        set_message(L"已启用强制 HTTP 代理：代理不可用时请求会失败，不会改走直连");
    }
    // 选「自定义尺寸」后立刻弹出输入框，避免用户找不到尺寸入口。
    if (key == kWindowSize && value == 3) {
        const std::wstring initial = std::to_wstring(m_config.window_custom_width) + L"×" +
                                     std::to_wstring(m_config.window_custom_height);
        open_input(L"自定义窗口尺寸（宽×高，如 1280×720）", initial,
                   [this](const std::wstring& text) {
                       int width = 0;
                       int height = 0;
                       if (!parse_window_size(text, &width, &height)) {
                           set_message(L"尺寸无效：请输入宽×高（320~7680 × 240~4320）");
                           return;
                       }
                       m_config.window_custom_width = width;
                       m_config.window_custom_height = height;
                       commit();
                   });
    }
}

void SettingsPage::test_proxy() {
    if (m_proxy_testing) {
        return;
    }
    if (m_config.proxy_mode != 1) {
        set_message(L"当前不是「使用 HTTP 代理」模式；系统代理/直连无需测试");
        if (request_repaint) {
            request_repaint();
        }
        return;
    }
    m_proxy_testing = true;
    set_message(L"正在通过代理访问测试地址…");
    if (request_repaint) {
        request_repaint();
    }
    const std::weak_ptr<SettingsPage> weak = weak_from_this();
    std::thread([weak]() {
        sscl::platform::HttpRequest request;
        request.url = "https://api.modrinth.com/";
        request.resolve_timeout_ms = 5000;
        request.connect_timeout_ms = 5000;
        request.receive_timeout_ms = 8000;
        const util::Result<sscl::platform::HttpResponse> result =
            sscl::platform::http_request(request);
        const bool ok = result.is_ok();
        const std::string detail =
            result.is_error() ? result.error().message : std::string();
        std::shared_ptr<SettingsPage> page = weak.lock();
        if (!page || page->m_window == nullptr) {
            return;
        }
        page->m_window->post_to_ui([weak, ok, detail]() {
            std::shared_ptr<SettingsPage> alive = weak.lock();
            if (!alive) {
                return;
            }
            alive->m_proxy_testing = false;
            std::wstring message =
                ok ? std::wstring(L"代理可用：测试请求已成功")
                   : std::wstring(L"代理不可用：") + widen(detail);
            alive->set_message(std::move(message));
            if (alive->request_repaint) {
                alive->request_repaint();
            }
        });
    }).detach();
}

void SettingsPage::run_network_diagnostics() {
    if (m_net.running) {
        return;
    }
    m_net.entries.assign(kProbeTargetCount, ProbeResult{});
    m_net.exit_ip.clear();
    m_net.exit_ip_done = false;
    m_net.started = true;
    m_net.running = true;
    m_probe_spinner = 0;
    m_probe_spinner_ms = 0.0f;
    rebuild_rows();
    if (request_repaint) {
        request_repaint();
    }

    const std::weak_ptr<SettingsPage> weak = weak_from_this();
    std::thread([weak]() {
        constexpr int kTimeoutMs = 3000;  // 全超时约 30s，可达目标通常几秒
        // D-113：逐项回写，让文本框在测试过程中逐行填充，而不是最后一次性出现。
        for (std::size_t i = 0; i < kProbeTargetCount; ++i) {
            const LatencySample sample = measure_latency(kProbeTargets[i].url, kTimeoutMs);
            std::shared_ptr<SettingsPage> page = weak.lock();
            if (!page || page->m_window == nullptr) {
                return;
            }
            page->m_window->post_to_ui([weak, i, sample]() {
                std::shared_ptr<SettingsPage> alive = weak.lock();
                if (!alive || i >= alive->m_net.entries.size()) {
                    return;
                }
                alive->m_net.entries[i].connect_ms = sample.connect_ms;
                alive->m_net.entries[i].total_ms = sample.total_ms;
                alive->m_net.entries[i].done = true;
                if (alive->request_repaint) {
                    alive->request_repaint();
                }
            });
        }

        const std::wstring exit_ip = fetch_exit_ip();
        std::shared_ptr<SettingsPage> page = weak.lock();
        if (!page || page->m_window == nullptr) {
            return;
        }
        page->m_window->post_to_ui([weak, exit_ip]() {
            std::shared_ptr<SettingsPage> alive = weak.lock();
            if (!alive) {
                return;
            }
            alive->m_net.exit_ip = exit_ip;
            alive->m_net.exit_ip_done = true;
            alive->m_net.running = false;
            alive->rebuild_rows();
            alive->set_message(L"网络诊断完成");
            if (alive->request_repaint) {
                alive->request_repaint();
            }
        });
    }).detach();
}

void SettingsPage::activate_row(const RowHit& hit) {
    switch (hit.key) {
        case kIsolation:
        case kTheme:
        case kMotion:
        case kLauncherVisibility:
        case kProcessPriority:
        case kWindowSize:
        case kDownloadSource:
        case kCommunitySource:
        case kMemoryMode:
        case kProxyMode:
            open_choice(hit.key);
            break;
        case kWindowTitle:
            open_input(L"游戏窗口标题（留空则不进行更改）",
                       widen(m_config.game_window_title),
                       [this](const std::wstring& value) {
                           m_config.game_window_title = narrow(trim_wide(value));
                           commit();
                       });
            break;
        case kCustomInfo:
            open_input(L"自定义信息（留空则使用版本自带信息）",
                       widen(m_config.custom_info),
                       [this](const std::wstring& value) {
                           m_config.custom_info = sanitize_custom_info(value);
                           commit();
                       });
            break;
        case kProxyTest:
            test_proxy();
            break;
        case kNetworkDiagnose:
            run_network_diagnostics();
            break;
        case kProxyHost:
            open_input(L"HTTP 代理地址（默认 127.0.0.1）", widen(m_config.proxy_host),
                       [this](const std::wstring& value) {
                           m_config.proxy_host = sanitize_proxy_host(value);
                           commit();
                       });
            break;
        case kProxyPort:
            open_input(L"HTTP 代理端口（1-65535，默认 10800）",
                       std::to_wstring(m_config.proxy_port),
                       [this](const std::wstring& value) {
                           int port = 0;
                           if (!parse_proxy_port(value, &port)) {
                               set_message(L"端口无效：请输入 1-65535 的数字");
                               return;
                           }
                           m_config.proxy_port = port;
                           commit();
                       });
            break;
        case kCurseForgeKey:
            // 初始文本故意留空：不把已保存的 Key 回显到屏幕上。
            open_input(L"自定义 CurseForge API Key（输入新值；留空并确定 = 清除）", L"",
                       [this](const std::wstring& value) {
                           const std::string key = narrow(trim_wide(value));
                           const util::Result<void> saved = sscl::core::store_secret_value(
                               m_data_dir / L"secrets.dat", "curseforge_api_key", key);
                           if (saved.is_error()) {
                               set_message(L"保存失败：" + widen(saved.error().message));
                               return;
                           }
                           set_message(key.empty()
                                           ? L"已清除自定义 Key（回退到构建期 Key / 镜像源）"
                                           : L"已保存（DPAPI 本机加密，切换附加页生效）");
                           rebuild_rows();
                           if (request_repaint) {
                               request_repaint();
                           }
                       });
            break;
        case kJavaRefresh:
            refresh_java();
            break;
        case kScanDirAdd:
            // 双按钮行由 mouse_up 直接分发；这里只兜底处理主按钮。
            add_scan_dir();
            break;
        case kGameFolder:
            if (!sscl::platform::open_folder(m_game_dir)) {
                set_message(L"打开游戏文件夹失败");
            }
            break;
        case kOpenData:
            if (!sscl::platform::open_folder(m_data_dir)) {
                set_message(L"打开数据目录失败");
            }
            break;
        case kOpenLogs:
            if (!sscl::platform::open_folder(m_data_dir / L"logs")) {
                set_message(L"打开日志目录失败");
            }
            break;
        case kThirdParty:
        case kPrivacy:
            open_doc(hit.key);
            break;
        default:
            break;
    }
}

void SettingsPage::commit() {
    if (m_on_config_changed) {
        m_on_config_changed(m_config);
    }
    set_message(L"已保存");
    rebuild_rows();
    if (request_repaint) {
        request_repaint();
    }
}

void SettingsPage::probe_java_candidates() {
    if (m_java_scanning || m_config.java_candidates.empty()) {
        return;
    }
    m_java_scanning = true;
    const std::vector<std::string> candidates = m_config.java_candidates;
    const std::weak_ptr<SettingsPage> weak = weak_from_this();
    std::thread([weak, candidates]() {
        std::vector<JavaEntry> entries;
        entries.reserve(candidates.size());
        for (const std::string& path : candidates) {
            JavaEntry entry;
            entry.path = path;
            const util::Result<sscl::core::JavaRuntime> runtime =
                sscl::core::validate_java_path(
                    std::filesystem::path(sscl::util::utf8_to_wide(path)));
            if (runtime.is_ok()) {
                entry.major = runtime.value().version.major_version;
                entry.version = runtime.value().version.version_string;
            }
            entries.push_back(std::move(entry));
        }
        std::shared_ptr<SettingsPage> page = weak.lock();
        if (!page || page->m_window == nullptr) {
            return;
        }
        page->m_window->post_to_ui([weak, entries]() {
            std::shared_ptr<SettingsPage> alive = weak.lock();
            if (!alive) {
                return;
            }
            alive->m_java_entries = entries;
            alive->m_java_scanning = false;
            if (alive->request_repaint) {
                alive->request_repaint();
            }
        });
    }).detach();
}

void SettingsPage::refresh_java() {
    if (m_java_scanning) {
        return;
    }
    m_java_scanning = true;
    set_message(L"正在扫描 Java…");
    rebuild_rows();
    if (request_repaint) {
        request_repaint();
    }

    const std::vector<std::string> dirs = m_config.java_scan_dirs;
    const std::vector<std::string> previous = m_config.java_candidates;
    const std::weak_ptr<SettingsPage> weak = weak_from_this();
    std::thread([weak, dirs, previous]() {
        std::vector<std::filesystem::path> roots;
        roots.reserve(dirs.size());
        for (const std::string& dir : dirs) {
            if (!dir.empty()) {
                roots.push_back(std::filesystem::path(sscl::util::utf8_to_wide(dir)));
            }
        }
        const std::vector<sscl::core::JavaRuntime> runtimes =
            sscl::core::detect_java_in_dirs(roots, 4000);

        std::vector<JavaEntry> found;
        found.reserve(runtimes.size());
        for (const sscl::core::JavaRuntime& runtime : runtimes) {
            JavaEntry entry;
            entry.path = sscl::util::wide_to_utf8(runtime.executable.parent_path().wstring());
            entry.major = runtime.version.major_version;
            entry.version = runtime.version.version_string;
            found.push_back(std::move(entry));
        }

        // 保留用户已有顺序（仍然存在的项），新发现的追加到末尾 —— 排序即优先级。
        std::vector<JavaEntry> ordered;
        std::vector<bool> used(found.size(), false);
        for (const std::string& old_path : previous) {
            for (std::size_t i = 0; i < found.size(); ++i) {
                if (used[i] || !same_java_path(old_path, found[i].path)) {
                    continue;
                }
                ordered.push_back(found[i]);
                used[i] = true;
                break;
            }
        }
        for (std::size_t i = 0; i < found.size(); ++i) {
            if (!used[i]) {
                ordered.push_back(found[i]);
            }
        }

        std::shared_ptr<SettingsPage> page = weak.lock();
        if (!page || page->m_window == nullptr) {
            return;
        }
        page->m_window->post_to_ui([weak, ordered]() {
            std::shared_ptr<SettingsPage> alive = weak.lock();
            if (!alive) {
                return;
            }
            alive->m_java_entries = ordered;
            alive->m_config.java_candidates.clear();
            for (const JavaEntry& entry : ordered) {
                alive->m_config.java_candidates.push_back(entry.path);
            }
            alive->m_java_scanning = false;
            alive->commit();
            alive->set_message(ordered.empty()
                                   ? L"未找到 Java，请检查下方扫描位置"
                                   : L"已找到 " + std::to_wstring(ordered.size()) +
                                         L" 个 Java 运行时");
        });
    }).detach();
}

void SettingsPage::move_java(int index, int delta) {
    const int count = static_cast<int>(m_config.java_candidates.size());
    const int target = index + delta;
    if (index < 0 || index >= count || target < 0 || target >= count) {
        return;
    }
    std::swap(m_config.java_candidates[static_cast<std::size_t>(index)],
              m_config.java_candidates[static_cast<std::size_t>(target)]);
    if (static_cast<std::size_t>(index) < m_java_entries.size() &&
        static_cast<std::size_t>(target) < m_java_entries.size()) {
        std::swap(m_java_entries[static_cast<std::size_t>(index)],
                  m_java_entries[static_cast<std::size_t>(target)]);
    }
    commit();
}

void SettingsPage::remove_java(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= m_config.java_candidates.size()) {
        return;
    }
    m_config.java_candidates.erase(m_config.java_candidates.begin() + index);
    if (static_cast<std::size_t>(index) < m_java_entries.size()) {
        m_java_entries.erase(m_java_entries.begin() + index);
    }
    commit();
}

void SettingsPage::open_java_folder(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= m_config.java_candidates.size()) {
        return;
    }
    const std::filesystem::path path(
        sscl::util::utf8_to_wide(m_config.java_candidates[static_cast<std::size_t>(index)]));
    if (!sscl::platform::open_folder(path)) {
        set_message(L"打开 Java 目录失败");
    }
}

void SettingsPage::add_scan_dir() {
    if (m_window == nullptr) {
        return;
    }
    std::wstring selected;
    if (!sscl::platform::select_folder_dialog(
            m_window->hwnd(), L"选择 Java 扫描位置（bin 目录或 JDK 根目录）", &selected) ||
        selected.empty()) {
        return;
    }
    const std::string dir = narrow(selected);
    for (const std::string& existing : m_config.java_scan_dirs) {
        if (same_java_path(existing, dir)) {
            set_message(L"该目录已在扫描位置中");
            return;
        }
    }
    m_config.java_scan_dirs.push_back(dir);
    // 手动添加不写入 java_scan_dirs_auto → 列表里显示为「手动添加」。
    commit();
    set_message(L"已添加扫描位置");
}

void SettingsPage::auto_scan_dirs() {
    const std::vector<std::filesystem::path> defaults =
        sscl::core::default_java_scan_dirs();
    std::size_t added = 0;
    for (const std::filesystem::path& path : defaults) {
        const std::string dir = narrow(path.wstring());
        if (dir.empty()) {
            continue;
        }
        bool exists = false;
        for (const std::string& existing : m_config.java_scan_dirs) {
            if (same_java_path(existing, dir)) {
                exists = true;
                break;
            }
        }
        if (exists) {
            continue;
        }
        m_config.java_scan_dirs.push_back(dir);
        m_config.java_scan_dirs_auto.push_back(dir);
        ++added;
    }
    commit();
    set_message(added == 0 ? L"扫描位置已包含全部常见目录"
                           : L"已自动添加 " + std::to_wstring(added) + L" 个扫描位置");
}

void SettingsPage::remove_scan_dir(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= m_config.java_scan_dirs.size()) {
        return;
    }
    const std::string dir = m_config.java_scan_dirs[static_cast<std::size_t>(index)];
    m_config.java_scan_dirs.erase(m_config.java_scan_dirs.begin() + index);
    for (auto it = m_config.java_scan_dirs_auto.begin();
         it != m_config.java_scan_dirs_auto.end();) {
        if (same_java_path(*it, dir)) {
            it = m_config.java_scan_dirs_auto.erase(it);
        } else {
            ++it;
        }
    }
    commit();
}

std::wstring SettingsPage::category_label(Category category) const {
    switch (category) {
        case Category::kGame:
            return L"游戏";
        case Category::kJava:
            return L"Java";
        case Category::kAppearance:
            return L"外观";
        case Category::kNetwork:
            return L"网络";
        case Category::kOther:
            return L"其他";
        case Category::kAbout:
            return L"关于";
    }
    return L"游戏";
}

std::wstring SettingsPage::row_label(int key) const {
    switch (key) {
        case kIsolation:
            return L"默认版本隔离";
        case kWindowTitle:
            return L"游戏窗口标题";
        case kCustomInfo:
            return L"自定义信息";
        case kLauncherVisibility:
            return L"启动器可见性";
        case kProcessPriority:
            return L"进程优先级";
        case kWindowSize:
            return L"窗口大小";
        case kCurseForgeKey:
            return L"CurseForge API Key";
        case kProxyMode:
            return L"网络代理";
        case kProxyTest:
            return L"测试代理连通性";
        case kNetworkDiagnose:
            return L"网络连接测试";
        case kProbeLog:
            return L"测试结果";
        case kProxyHost:
            return L"代理地址";
        case kProxyPort:
            return L"代理端口";
        case kDownloadSource:
            return L"下载及列表网络源";
        case kCommunitySource:
            return L"社区资源网络源";
        case kMemoryMode:
            return L"内存分配";
        case kMemoryBar:
            return L"当前内存分配";
        case kAutoChinese:
            return L"自动设置游戏为中文（简体）";
        case kGameFolder:
            return L"游戏文件夹";
        case kJavaRefresh:
            return L"Java 版本";
        case kScanDirAdd:
            return L"添加扫描目录";
        case kTheme:
            return L"主题模式";
        case kMotion:
            return L"动效强度";
        case kVersion:
            return L"程序版本";
        case kDataDir:
            return L"数据目录";
        case kOpenData:
            return L"打开数据目录";
        case kOpenLogs:
            return L"打开日志目录";
        case kThirdParty:
            return L"第三方说明";
        case kPrivacy:
            return L"隐私政策";
        default:
            return {};
    }
}

std::wstring SettingsPage::row_desc(int key) const {
    switch (key) {
        case kIsolation:
            return L"安装新版本时自动套用；已有版本请在版本设置中调整";
        case kWindowTitle:
            return L"留空则不更改；版本设置中单独指定时以版本为准";
        case kCustomInfo:
            return L"留空使用版本自带信息；填写后显示在主界面左下 / F3";
        case kLauncherVisibility:
            return L"控制游戏启动后启动器窗口的显示行为";
        case kProcessPriority:
            return L"高：优先游戏性能，其他程序可能卡顿；低：适合挂机";
        case kWindowSize:
            return L"全屏 / 跟随启动器 / 自定义宽高 / 最大化";
        case kCurseForgeKey:
            return L"留空=使用构建期 Key 或镜像；填写后仅存本机（DPAPI），优先级最高";
        case kProxyMode:
            return L"系统代理 = Windows 设置；HTTP 代理为强制代理，连不上会直接报错";
        case kProxyHost:
            return L"仅「使用 HTTP 代理」时生效；默认 127.0.0.1";
        case kProxyPort:
            return L"仅「使用 HTTP 代理」时生效；默认 10800";
        case kProxyTest:
            return L"用当前代理访问测试地址；失败会显示具体原因";
        case kNetworkDiagnose:
            return m_net.running ? L"正在依次测试，请稍候…"
                                 : L"连接/首字节（ms）；绿 <150、橙 ≥150、红 = 超时";
        case kProbeLog:
            return {};
        case kDownloadSource:
            return L"控制游戏文件、版本清单与加载器清单的源顺序";
        case kCommunitySource:
            return L"控制 Modrinth / CurseForge 搜索与详情请求的源顺序";
        case kMemoryMode:
            return L"自动按可用物理内存推算；自定义可拖动下方滑块";
        case kMemoryBar:
            return L"每秒刷新；深蓝为系统已用，浅蓝为游戏分配";
        case kAutoChinese:
            return L"启动前把 options.txt 的语言设为 zh_cn，不影响其它设置";
        case kGameFolder:
            return L"当前 .minecraft 位置；可在版本选择页切换文件夹";
        case kJavaRefresh:
            return L"按下面的顺序优先使用；点「刷新」重新扫描并探测版本";
        case kScanDirAdd:
            return L"「添加」手动选 bin 目录；「自动扫描」按常见安装位置补充";
        case kTheme:
            return L"切换立即生效；跟随系统时随 Windows 应用模式变化";
        case kMotion:
            return L"影响悬停、按下、弹层与页面切换；关闭后一律瞬时完成";
        case kVersion:
            return L"SSCL · 单文件 EXE · Win32 + Direct2D 自绘";
        case kDataDir:
            return L"配置、日志、账号与游戏文件所在位置";
        case kOpenData:
            return L"在资源管理器中打开数据目录";
        case kOpenLogs:
            return L"排查启动/下载问题时查看运行日志";
        case kThirdParty:
            return L"第三方组件与许可证信息";
        case kPrivacy:
            return L"本地数据、网络请求与凭据说明";
        default:
            return {};
    }
}

std::wstring SettingsPage::row_value(int key) const {
    if (key == kCurseForgeKey) {
        // D-106：机密绝不回显，只展示状态。
        const util::Result<std::string> secret = sscl::core::load_secret_value(
            m_data_dir / L"secrets.dat", "curseforge_api_key");
        if (secret.is_ok() && !secret.value().empty()) {
            return L"已设置（本机加密保存）";
        }
        return sscl::core::curseforge_configured() ? L"使用构建期 Key"
                                                   : L"未设置（仅镜像源可用）";
    }
    if (key == kLauncherVisibility || key == kProcessPriority || key == kWindowSize ||
        key == kIsolation || key == kDownloadSource || key == kCommunitySource ||
        key == kMemoryMode || key == kProxyMode) {
        const std::vector<std::pair<int, std::wstring>> items = choice_items(key);
        for (const auto& item : items) {
            if (item.first != row_value_int(key)) {
                continue;
            }
            if (key == kWindowSize && item.first == 3) {
                return item.second + L" " + std::to_wstring(m_config.window_custom_width) +
                       L"×" + std::to_wstring(m_config.window_custom_height);
            }
            return item.second;
        }
        return key == kIsolation ? L"隔离所有版本" : L"默认";
    }
    if (key == kWindowTitle) {
        return m_config.game_window_title.empty() ? L"不更改"
                                                  : widen(m_config.game_window_title);
    }
    if (key == kCustomInfo) {
        return m_config.custom_info.empty() ? L"版本自带" : widen(m_config.custom_info);
    }
    switch (key) {
        case kGameFolder:
            return m_game_dir.wstring();
        case kTheme:
            if (m_config.theme_mode == "light") {
                return L"浅色";
            }
            if (m_config.theme_mode == "dark") {
                return L"深色";
            }
            return L"跟随系统";
        case kMotion:
            return sscl::ui::motion::level_label(
                sscl::ui::motion::level_from_key(m_config.motion_level));
        case kVersion:
            return L"0.1.0.0";
        case kProxyHost:
            return m_config.proxy_host.empty() ? L"127.0.0.1" : widen(m_config.proxy_host);
        case kProxyPort:
            return std::to_wstring(m_config.proxy_port);
        case kDataDir:
            return m_data_dir.wstring();
        default:
            return {};
    }
}

std::wstring SettingsPage::row_action(int key) const {
    switch (key) {
        case kGameFolder:
        case kOpenData:
        case kOpenLogs:
            return L"打开";
        case kThirdParty:
        case kPrivacy:
            return L"查看";
        case kJavaRefresh:
            return m_java_scanning ? L"扫描中" : L"刷新";
        case kProxyTest:
            return L"测试";
        case kNetworkDiagnose:
            return m_net.running ? L"测试中" : L"开始测试";
        default:
            return L"打开";
    }
}

int SettingsPage::hit_tab(float x, float y) const {
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
        if (contains(m_tabs[i].rect, x, y)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int SettingsPage::hit_row(float x, float y) const {
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].kind != RowKind::kSection && contains(m_rows[i].row, x, y)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool SettingsPage::mouse_move(float x, float y) {
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;
    bool changed = false;
    if (m_doc_open) {
        m_tooltip.clear();
        if (m_doc_scroll.on_mouse_move(x_dip, y_dip)) {
            if (request_repaint) {
                request_repaint();
            }
            return true;
        }
        return false;
    }
    if (m_input_open || m_choice_open) {
        m_tooltip.clear();
    }

    // 滚动条拖拽/悬停（D-022）：拖拽会直接改偏移，行位置需重建。
    if (m_scroll.on_mouse_move(x_dip, y_dip)) {
        rebuild_rows();
        changed = true;
    }
    if (m_memory_dragging) {
        set_memory_from_slider(x_dip);
        return true;
    }

    if (m_choice_open) {
        int hover = -1;
        if (contains(m_choice_rect, x_dip, y_dip)) {
            const int index =
                static_cast<int>((y_dip - (m_choice_rect.top + 4.0f)) / 28.0f);
            if (index >= 0 && index < static_cast<int>(m_choice_labels.size())) {
                hover = index;
            }
        }
        if (hover != m_choice_hover) {
            m_choice_hover = hover;
            changed = true;
        }
        return changed;
    }

    const int tab = hit_tab(x_dip, y_dip);
    if (tab != m_hover_tab) {
        m_hover_tab = tab;
        changed = true;
    }
    const int row = hit_row(x_dip, y_dip);
    int button = 0;
    if (row >= 0) {
        const RowHit& row_hit = m_rows[static_cast<std::size_t>(row)];
        if (row_hit.kind == RowKind::kJavaRuntime) {
            for (int b = 0; b < 4; ++b) {
                if (contains(row_hit.buttons[b], x_dip, y_dip)) {
                    button = b + 3;  // 3..6 = ↑ ↓ − 打开
                    break;
                }
            }
        } else if (contains(row_hit.control, x_dip, y_dip)) {
            button = 1;
        } else if (rect_valid(row_hit.control2) && contains(row_hit.control2, x_dip, y_dip)) {
            button = 2;
        }
    }
    if (row != m_hover_row || button != m_hover_button) {
        m_hover_row = row;
        m_hover_button = button;
        changed = true;
    }
    if (!m_input_open && !m_choice_open) {
        std::wstring tip;
        D2D1_RECT_F anchor{};
        if (row >= 0) {
            tip = row_tooltip(m_rows[static_cast<std::size_t>(row)].key);
            anchor = m_rows[static_cast<std::size_t>(row)].row;
        }
        if (m_tooltip.update(std::move(tip), anchor, x_dip, y_dip)) {
            changed = true;
        }
    }
    return changed;
}

bool SettingsPage::mouse_down(float x, float y) {
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;
    if (m_doc_open) {
        if (contains(m_doc_close, x_dip, y_dip)) {
            return true;
        }
        if (m_doc_scroll.on_mouse_down(x_dip, y_dip) && request_repaint) {
            request_repaint();
        }
        return true;  // 模态：吞掉所有按下
    }
    if (m_input_open || m_choice_open) {
        return false;
    }
    if (m_scroll.on_mouse_down(x * m_to_dip, y * m_to_dip)) {
        rebuild_rows();
        if (request_repaint) {
            request_repaint();
        }
        return true;
    }
    // D-107：自定义内存滑块（扩大命中范围便于抓取）。
    if (m_config.memory_mode == 1 && rect_valid(m_memory_slider_rect)) {
        const D2D1_RECT_F grab =
            rect_of(m_memory_slider_rect.left - 8.0f, m_memory_slider_rect.top - 12.0f,
                    m_memory_slider_rect.right + 8.0f, m_memory_slider_rect.bottom + 12.0f);
        if (contains(grab, x * m_to_dip, y * m_to_dip)) {
            m_memory_dragging = true;
            set_memory_from_slider(x * m_to_dip);
            return true;
        }
    }
    return false;
}

bool SettingsPage::mouse_up(float x, float y) {
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;

    if (m_doc_open) {
        if (m_doc_scroll.on_mouse_up(x_dip, y_dip)) {
            if (request_repaint) {
                request_repaint();
            }
            return true;
        }
        if (contains(m_doc_close, x_dip, y_dip)) {
            close_doc_overlay();
        }
        return true;
    }

    // 结束滚动条拖拽（返回 true 表示本次抬起属于拖拽，不再当作点击）。
    if (m_scroll.on_mouse_up(x_dip, y_dip)) {
        if (request_repaint) {
            request_repaint();
        }
        return true;
    }
    if (m_memory_dragging) {
        m_memory_dragging = false;
        commit();  // 拖拽结束才落盘，避免每帧写 config.json
        return true;
    }

    if (m_input_open) {
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
        const std::wstring value = m_input_box != nullptr ? m_input_box->text : std::wstring();
        auto accept = m_input_accept;
        close_overlay();
        if (accept) {
            accept(value);
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
        motion::menu().set_open(false, 1);  // D-105：淡出在事件里启动
        m_choice_hover = -1;
        if (request_repaint) {
            request_repaint();
        }
        return true;
    }

    const int tab = hit_tab(x_dip, y_dip);
    if (tab >= 0) {
        const Category category = m_tabs[static_cast<std::size_t>(tab)].category;
        if (category != m_category) {
            m_category = category;
            motion::content().jump();  // D-108：页内切换瞬时；全局 content 已被整页转场占用
            m_scroll.jump_to(0.0f);
            m_hover_row = -1;
            rebuild_rows();
            if (request_repaint) {
                request_repaint();
            }
        }
        return true;
    }

    const int row = hit_row(x_dip, y_dip);
    if (row >= 0) {
        // 按下标拷贝一份：activate_row/commit 内可能重建 m_rows（引用会失效）。
        const RowHit hit = m_rows[static_cast<std::size_t>(row)];
        if (hit.kind == RowKind::kJavaRuntime) {
            for (int b = 0; b < 4; ++b) {
                if (!contains(hit.buttons[b], x_dip, y_dip)) {
                    continue;
                }
                switch (b) {
                    case 0:
                        move_java(hit.index, -1);
                        break;
                    case 1:
                        move_java(hit.index, 1);
                        break;
                    case 2:
                        remove_java(hit.index);
                        break;
                    default:
                        open_java_folder(hit.index);
                        break;
                }
                return true;
            }
            return false;
        }
        if (hit.kind == RowKind::kScanDirEntry) {
            if (contains(hit.control, x_dip, y_dip)) {
                remove_scan_dir(hit.index);
                return true;
            }
            return false;
        }
        if (hit.kind == RowKind::kDualAction) {
            if (contains(hit.control, x_dip, y_dip)) {
                add_scan_dir();
                return true;
            }
            if (contains(hit.control2, x_dip, y_dip)) {
                auto_scan_dirs();
                return true;
            }
            return false;
        }
        if (hit.kind == RowKind::kAction || hit.kind == RowKind::kChoice ||
            hit.kind == RowKind::kText) {
            if (contains(hit.control, x_dip, y_dip)) {
                activate_row(hit);
                return true;
            }
        }
        if (hit.kind == RowKind::kToggle) {
            // 整行可点，便于快速切换（与 PCL 复选框行一致）。
            m_config.auto_chinese_language = !m_config.auto_chinese_language;
            commit();
            return true;
        }
        return false;
    }
    return false;
}

bool SettingsPage::mouse_leave() {
    const bool had_tooltip = m_tooltip.visible();
    m_tooltip.clear();
    bool changed = had_tooltip || m_hover_tab != -1 || m_hover_row != -1 ||
                   m_choice_hover != -1;
    if (m_doc_open && m_doc_scroll.on_mouse_leave()) {
        changed = true;
    }
    if (m_memory_dragging) {
        m_memory_dragging = false;
        commit();
        changed = true;
    }
    m_hover_tab = -1;
    m_hover_row = -1;
    m_hover_button = 0;
    m_choice_hover = -1;
    if (m_scroll.on_mouse_leave()) {
        changed = true;
    }
    return changed;
}

bool SettingsPage::mouse_wheel(float x, float y, float delta) {
    if (m_doc_open) {
        const float x_dip = x * m_to_dip;
        const float y_dip = y * m_to_dip;
        if (contains(m_doc_view, x_dip, y_dip) || contains(m_overlay_card, x_dip, y_dip)) {
            if (m_doc_scroll.on_wheel(delta, 60.0f) && request_repaint) {
                request_repaint();
            }
        }
        return true;
    }
    if (m_choice_open || m_input_open) {
        return false;
    }
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;
    if (x_dip < m_content_left - 6.0f || y_dip < m_view_top || y_dip > m_view_bottom) {
        return false;
    }
    if (m_scroll.on_wheel(delta, 120.0f)) {
        rebuild_rows();
        if (request_repaint) {
            request_repaint();
        }
        return true;
    }
    return false;
}

bool SettingsPage::handle_char(wchar_t ch) {
    if (!m_input_open || !m_input_box) {
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

bool SettingsPage::handle_key_down(unsigned long key) {
    constexpr unsigned long kEscape = 0x1B;
    constexpr unsigned long kReturn = 0x0D;
    constexpr unsigned long kBack = 0x08;
    if (m_doc_open) {
        if (key == kEscape || key == kReturn || key == 0x20 /* VK_SPACE */) {
            close_doc_overlay();
        }
        return true;  // 文档弹层独占键盘
    }
    if (m_input_open && m_input_box) {
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
    if (m_choice_open) {
        if (key == kEscape) {
            m_choice_open = false;
            motion::menu().set_open(false, 1);  // D-105：淡出在事件里启动
            m_choice_hover = -1;
            if (request_repaint) {
                request_repaint();
            }
            return true;
        }
        return false;
    }
    return false;
}

bool SettingsPage::tick(float dt_ms) {
    bool active = m_scroll.tick(dt_ms);
    if (m_doc_mode) {
        if (m_doc_scroll.tick(dt_ms)) {
            active = true;
        }
        if (!m_doc_open && !motion::modal().visible()) {
            m_doc_mode = false;  // 淡出结束，恢复正常页面绘制
        }
    }
    // D-104：Tooltip 有 320ms 延迟，期间必须保持帧循环才能显示出来。
    if (m_tooltip.tick(dt_ms)) {
        active = true;
    }
    if (m_category == Category::kOther) {
        // D-107：内存条每秒采样一次；WM_PAINT 的 more_frames 来自本函数返回值，
        // 因此这里必须持续返回 true 保活帧循环，否则下一秒无人唤醒。
        if (m_load.memory_total_bytes == 0) {
            m_load = m_monitor.sample();  // 首次进入页面立即有数值
        }
        m_sample_accum_ms += dt_ms;
        if (m_sample_accum_ms >= 1000.0f) {
            m_sample_accum_ms = 0.0f;
            m_load = m_monitor.sample();
        }
        active = true;
    }
    if (m_net.running) {
        // D-113：spinner 每 120ms 前进一格；测试期间必须保活帧循环才能持续刷新。
        m_probe_spinner_ms += dt_ms;
        while (m_probe_spinner_ms >= kProbeSpinnerStepMs) {
            m_probe_spinner_ms -= kProbeSpinnerStepMs;
            m_probe_spinner = (m_probe_spinner + 1) % 4;
        }
        active = true;
    }
    return active;
}

}  // namespace sscl::ui
