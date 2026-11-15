#include "ui/pages/addons_page.hpp"

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <thread>
#include <utility>

#include "core/config/build_config.hpp"
#include "core/config/secret_store.hpp"
#include "core/game/pcl_version.hpp"
#include "core/mod/mod_installer.hpp"
#include "core/mod/mod_scanner.hpp"
#include "platform/path_utils.hpp"
#include "platform/shell_utils.hpp"
#include "ui/framework/icon_manager.hpp"
#include "ui/framework/remote_image.hpp"
#include "ui/framework/window.hpp"
#include "ui/shell/shell_layout.hpp"
#include "ui/theme/theme.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

namespace sscl::ui {

namespace {

constexpr float kVersionRowHeight = 34.0f;
constexpr float kPageMargin = 25.0f;  // PCL PageResource.xaml StackPanel Margin="25"
constexpr float kFilterCardHeight = 196.0f;
// 滚动条距窗口右边缘：与设置页 `m_width_dip - 6` 对齐（D-097）。
constexpr float kScrollbarInset = 6.0f;
// 依赖勾选弹层的行节距（D-098）。
constexpr float kDepRowPitch = 44.0f;

D2D1_RECT_F rect_of(float left, float top, float right, float bottom) {
    return D2D1::RectF(left, top, right, bottom);
}

bool rect_valid(const D2D1_RECT_F& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool in_rect(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

std::wstring widen(const std::string& text) {
    return sscl::util::utf8_to_wide(text);
}

// D-116：实例解析给出的「原版号」可能只是加载器 id（neoforge-21.11.48）。
// 这类值送进平台的 versions/gameVersion 筛选会让结果恒为 0，必须丢弃。
std::string safe_mc_version(const std::string& value) {
    return sscl::core::is_plausible_mc_version(value) ? value : std::string();
}

// PCL ResourceProject：>1 亿用「亿」，>10 万「万」，否则原值。
std::wstring format_downloads(std::uint64_t downloads) {
    if (downloads > 100000000) {
        std::wostringstream out;
        out << std::fixed << std::setprecision(2)
            << static_cast<double>(downloads) / 100000000.0 << L" 亿";
        return out.str();
    }
    if (downloads > 100000) {
        return std::to_wstring(downloads / 10000) + L" 万";
    }
    return std::to_wstring(downloads);
}

std::wstring format_size(std::uint64_t bytes) {
    if (bytes >= 1024 * 1024) {
        std::wostringstream out;
        out << std::fixed << std::setprecision(1)
            << static_cast<double>(bytes) / (1024.0 * 1024.0) << L" MB";
        return out.str();
    }
    if (bytes >= 1024) {
        return std::to_wstring(bytes / 1024) + L" KB";
    }
    return std::to_wstring(bytes) + L" B";
}

std::wstring format_date(const std::string& iso) {
    if (iso.size() < 10) {
        return L"—";
    }
    return widen(iso.substr(0, 10));
}

std::wstring channel_text(sscl::core::ModChannel channel) {
    switch (channel) {
        case sscl::core::ModChannel::kRelease:
            return L"正式";
        case sscl::core::ModChannel::kBeta:
            return L"测试";
        case sscl::core::ModChannel::kAlpha:
            return L"预览";
        default:
            return L"—";
    }
}

std::wstring initial_of(const std::wstring& title) {
    for (const wchar_t ch : title) {
        if (ch != L' ' && ch != L'\t') {
            return std::wstring(1, ch);
        }
    }
    return L"M";
}

// 统一绘制入口：文字、圆角填充、圆角描边（全部语义色）。
void draw_label(ID2D1RenderTarget* target, IDWriteFactory* dwrite, const std::wstring& text,
                const D2D1_RECT_F& rect, float size, DWRITE_FONT_WEIGHT weight,
                const D2D1_COLOR_F& color,
                DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING) {
    if (target == nullptr || dwrite == nullptr || text.empty() || !rect_valid(rect)) {
        return;
    }
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (FAILED(dwrite->CreateTextFormat(theme_font_family(), nullptr, weight,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        size, L"zh-cn", format.GetAddressOf()))) {
        return;
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetTextAlignment(alignment);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
        return;
    }
    target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), rect,
                      brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
                      DWRITE_MEASURING_MODE_NATURAL);
}

float measure_label(IDWriteFactory* dwrite, const std::wstring& text, float size) {
    if (dwrite == nullptr || text.empty()) {
        return 0.0f;
    }
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (FAILED(dwrite->CreateTextFormat(theme_font_family(), nullptr,
                                        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn",
                                        format.GetAddressOf()))) {
        return 0.0f;
    }
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                        format.Get(), 10000.0f, 40.0f,
                                        layout.GetAddressOf()))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) {
        return 0.0f;
    }
    return metrics.widthIncludingTrailingWhitespace;
}

void fill_round(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float radius,
                const D2D1_COLOR_F& color) {
    if (target == nullptr || !rect_valid(rect)) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(target->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
        target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
    }
}

void stroke_round(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float radius,
                  const D2D1_COLOR_F& color) {
    if (target == nullptr || !rect_valid(rect)) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(target->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
        target->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), 1.0f);
    }
}

void draw_icon(ID2D1RenderTarget* target, IDWriteFactory* dwrite, const std::string& url,
               const std::wstring& title, const D2D1_RECT_F& rect) {
    if (target == nullptr || !rect_valid(rect)) {
        return;
    }
    auto image = RemoteImage::instance().bitmap(target, url);
    if (image) {
        target->DrawBitmap(image.Get(), rect, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
        return;
    }
    fill_round(target, rect, 6.0f, theme_primary_container());
    draw_label(target, dwrite, initial_of(title), rect, 16.0f,
               DWRITE_FONT_WEIGHT_SEMI_BOLD, theme_text_primary(),
               DWRITE_TEXT_ALIGNMENT_CENTER);
}

sscl::core::ModLoader loader_of(const sscl::core::InstanceTarget& target) {
    return sscl::core::mod_loader_from_key(target.loader);
}

bool version_matches(const sscl::core::ModVersion& version, const std::string& mc,
                     sscl::core::ModLoader loader) {
    const bool mc_ok = mc.empty() ||
                       std::find(version.game_versions.begin(), version.game_versions.end(), mc) !=
                           version.game_versions.end();
    const bool loader_ok =
        loader == sscl::core::ModLoader::kUnknown || loader == sscl::core::ModLoader::kVanilla ||
        std::find(version.loaders.begin(), version.loaders.end(), loader) != version.loaders.end();
    return mc_ok && loader_ok;
}

std::wstring describe_version(const sscl::core::ModVersion& version) {
    std::wstring text = L"版本：" + widen(version.version_number) + L"（" +
                        widen(version.name) + L"）";
    std::wstring dependencies;
    for (const auto& dependency : version.dependencies) {
        if (dependency.kind != sscl::core::DependencyKind::kRequired ||
            dependency.project_id.empty()) {
            continue;
        }
        if (!dependencies.empty()) {
            dependencies += L"、";
        }
        dependencies += widen(dependency.project_id);
    }
    text += dependencies.empty() ? L"；无 required 前置"
                                 : L"；前置：" + dependencies + L"（仅展示，不自动安装）";
    return text;
}

// —— PCL 分类（PageDownloadMod.xaml）：Tag = "CurseForgeId/ModrinthSlug" ——
struct CategoryDef {
    const wchar_t* label;
    const char* tag;
};

const CategoryDef kCategories[] = {
    {L"全部", ""},
    {L"世界元素", "406/worldgen"},
    {L"生物群系", "407/"},
    {L"维度", "410/"},
    {L"矿物与资源", "408/"},
    {L"天然结构", "409/"},
    {L"科技", "412/technology"},
    {L"管道与物流", "415/"},
    {L"自动化", "4843/"},
    {L"能源", "417/"},
    {L"红石", "4558/"},
    {L"食物与烹饪", "436/food"},
    {L"农业", "416/"},
    {L"游戏机制", "/game-mechanics"},
    {L"运输", "414/transportation"},
    {L"仓储", "420/storage"},
    {L"魔法", "419/magic"},
    {L"冒险", "422/adventure"},
    {L"装饰", "424/decoration"},
    {L"生物", "411/mobs"},
    {L"实用", "5191/utility"},
    {L"装备与工具", "434/equipment"},
    {L"创造模式", "9026/"},
    {L"性能优化", "6814/optimization"},
    {L"信息显示", "423/"},
    {L"服务器", "435/social"},
    {L"支持库", "421/library"},
};

const wchar_t* kVersionOptions[] = {L"全部 (也可自行输入)", L"1.21.11", L"1.21.8", L"1.21.4",
                                    L"1.21.1",           L"1.20.1",  L"1.19.2", L"1.18.2",
                                    L"1.16.5",           L"1.12.2",  L"1.7.10"};
// 来源只允许单平台：不再提供「全部」（负责人 2026-11-06 要求）。
// 索引 0=Modrinth、1=CurseForge，与 m_platform 一一对应。
const wchar_t* kSourceOptions[] = {L"Modrinth", L"CurseForge"};
constexpr std::size_t kSourceCount = sizeof(kSourceOptions) / sizeof(kSourceOptions[0]);
const wchar_t* kLoaderOptions[] = {L"任意 Mod 加载器", L"Forge", L"NeoForge", L"Fabric"};

// PCL ResourceProject.vb：CF categoryId / Modrinth categories → 中文标签。
std::wstring translate_tag(const std::string& raw) {
    std::string key = raw;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
        return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    });
    static const std::vector<std::pair<const char*, const wchar_t*>> table = {
        {"406", L"世界元素"},     {"407", L"生物群系"},   {"410", L"维度"},
        {"408", L"矿物/资源"},    {"409", L"天然结构"},   {"412", L"科技"},
        {"415", L"管道/物流"},    {"4843", L"自动化"},    {"417", L"能源"},
        {"4558", L"红石"},       {"436", L"食物/烹饪"},  {"416", L"农业"},
        {"414", L"运输"},        {"420", L"仓储"},       {"419", L"魔法"},
        {"422", L"冒险"},        {"424", L"装饰"},       {"411", L"生物"},
        {"434", L"装备"},        {"6814", L"性能优化"},  {"9026", L"创造模式"},
        {"423", L"信息显示"},    {"435", L"服务器"},     {"5191", L"改良"},
        {"421", L"支持库"},      {"worldgen", L"世界元素"}, {"technology", L"科技"},
        {"magic", L"魔法"},      {"adventure", L"冒险"}, {"utility", L"实用"},
        {"optimization", L"性能优化"}, {"food", L"食物/烹饪"},
        {"game-mechanics", L"游戏机制"}, {"transportation", L"运输"},
        {"storage", L"仓储"},    {"decoration", L"装饰"}, {"mobs", L"生物"},
        {"equipment", L"装备"},  {"social", L"服务器"},  {"library", L"支持库"},
        {"vanilla-like", L"原版风"}, {"realistic", L"写实风"},
        {"datapack", L"数据包"}, {"forge", L"Forge"},   {"fabric", L"Fabric"},
        {"neoforge", L"NeoForge"}, {"quilt", L"Quilt"},
    };
    for (const auto& item : table) {
        if (key == item.first) {
            return item.second;
        }
    }
    // 镜像源可能给英文显示名（如 "Optimization"）：做一次小写包含兜底。
    if (key.find("optimization") != std::string::npos) return L"性能优化";
    if (key.find("technology") != std::string::npos) return L"科技";
    if (key.find("magic") != std::string::npos) return L"魔法";
    if (key.find("adventure") != std::string::npos) return L"冒险";
    if (key.find("utility") != std::string::npos) return L"实用";
    if (key.find("library") != std::string::npos) return L"支持库";
    if (key.find("world") != std::string::npos) return L"世界元素";
    return {};
}

std::vector<std::wstring> project_tags(const sscl::core::ModProject& project,
                                       std::size_t limit = 3) {
    std::vector<std::wstring> tags;
    for (const auto& category : project.categories) {
        const std::wstring text = translate_tag(category);
        if (text.empty() || text == L"Forge" || text == L"Fabric" || text == L"NeoForge" ||
            text == L"Quilt") {
            continue;  // 加载器放到底部信息行，PCL 也不把它当 Tag
        }
        if (std::find(tags.begin(), tags.end(), text) != tags.end()) {
            continue;
        }
        tags.push_back(text);
        if (tags.size() >= limit) {
            break;
        }
    }
    return tags;
}

std::wstring loader_desc(const sscl::core::ModProject& project) {
    std::vector<std::wstring> loaders;
    for (const auto& category : project.categories) {
        const std::wstring text = translate_tag(category);
        if (text == L"Forge" || text == L"Fabric" || text == L"NeoForge" || text == L"Quilt") {
            if (std::find(loaders.begin(), loaders.end(), text) == loaders.end()) {
                loaders.push_back(text);
            }
        }
    }
    if (loaders.empty()) {
        return L"—";
    }
    std::wstring out;
    for (std::size_t i = 0; i < loaders.size(); ++i) {
        if (i != 0) out += L" / ";
        out += loaders[i];
    }
    return out;
}

// ISO8601 → 相对时间（PCL StringUtils.FormatTimeSpan："21 天前"）。
int days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const int shifted_month = static_cast<int>(m) + (m > 2 ? -3 : 9);
    const unsigned doy = (153 * shifted_month + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

// ISO8601 日期前 10 位固定为 YYYY-MM-DD。手写定宽解析，避免 /sdl（SDLCheck=true）
// 把 std::sscanf 的 C4996 提升为编译错误；与 core/account/account.cpp 的 hex 解析同一惯例。
bool parse_iso_date(const std::string& text, int& year, unsigned& month, unsigned& day) {
    if (text.size() < 10 || text[4] != '-' || text[7] != '-') {
        return false;
    }
    auto digits = [&text](std::size_t first, std::size_t count) -> int {
        int value = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const char c = text[first + i];
            if (c < '0' || c > '9') {
                return -1;
            }
            value = value * 10 + (c - '0');
        }
        return value;
    };
    const int y = digits(0, 4);
    const int m = digits(5, 2);
    const int d = digits(8, 2);
    if (y < 0 || m < 1 || m > 12 || d < 1 || d > 31) {
        return false;
    }
    year = y;
    month = static_cast<unsigned>(m);
    day = static_cast<unsigned>(d);
    return true;
}

std::wstring relative_time(const std::string& iso) {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (!parse_iso_date(iso, year, month, day)) {
        return format_date(iso);
    }
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    const int delta = days_from_civil(now.wYear, now.wMonth, now.wDay) -
                      days_from_civil(year, month, day);
    if (delta <= 0) return L"今天";
    if (delta < 30) return std::to_wstring(delta) + L" 天前";
    if (delta < 365) return std::to_wstring(delta / 30) + L" 个月前";
    return std::to_wstring(delta / 365) + L" 年前";
}

}  // namespace

AddonsPage::AddonsPage(AppWindow* window, std::shared_ptr<sscl::core::GameService> service,
                       sscl::core::LauncherConfig config)
    : m_window(window), m_service(std::move(service)), m_config(std::move(config)) {
    m_target_version_id = m_config.selected_version_id;
    if (m_target_version_id.empty() && m_service != nullptr) {
        const auto installed = m_service->installed_versions();
        if (!installed.empty()) {
            m_target_version_id = installed.front().id;
        }
    }
    m_platform = m_config.mod_platform == "curseforge" ? sscl::core::ModPlatform::kCurseForge
                                                       : sscl::core::ModPlatform::kModrinth;
    // 下拉已无「全部」，初始高亮必须与配置里的平台一致。
    m_source_index = m_platform == sscl::core::ModPlatform::kCurseForge ? 1 : 0;
    m_source_text = kSourceOptions[static_cast<std::size_t>(m_source_index)];

    std::string key = std::string(sscl::core::curseforge_api_key());
    util::Result<std::string> secret = sscl::core::load_secret_value(
        ::sscl::platform::data_dir() / L"secrets.dat", "curseforge_api_key");
    if (secret.is_ok() && !secret.value().empty()) {
        key = secret.value();
    }
    m_catalog_key = key;
    m_catalog = std::make_unique<sscl::core::ModCatalog>(
        sscl::core::make_mod_catalog_options(::sscl::platform::data_dir(), key, true));

    // PCL「名称」输入框
    m_search.placeholder = L"搜索 Mod 名称 / 关键词";
    m_search.max_length = 80;
    // PCL「版本」可编辑下拉
    m_version.text = kVersionOptions[0];
    m_version.placeholder = L"全部 (也可自行输入)";
    m_version.max_length = 32;

    // 本地管理按钮（仅本地模式显示于顶部操作条，不是左侧栏）
    auto local_toggle = std::make_unique<Button>(L"启用/禁用", 12.0f, false);
    auto local_delete = std::make_unique<Button>(L"删除", 12.0f, false);
    auto local_import = std::make_unique<Button>(L"导入…", 12.0f, false);
    auto local_check = std::make_unique<Button>(L"检查更新", 12.0f, false);
    auto local_apply = std::make_unique<Button>(L"应用更新", 12.0f, false);
    m_local_toggle = local_toggle.get();
    m_local_delete = local_delete.get();
    m_local_import = local_import.get();
    m_local_check = local_check.get();
    m_local_apply = local_apply.get();
    local_toggle->on_click = [this]() { toggle_selected_mod(); };
    local_delete->on_click = [this]() { delete_selected_mod(); };
    local_import->on_click = [this]() { import_local_mods(); };
    local_check->on_click = [this]() { check_local_updates(); };
    local_apply->on_click = [this]() { apply_local_updates(); };
    local_toggle->icon = "check";
    local_delete->icon = "delete";
    local_import->icon = "upload_file";
    local_check->icon = "refresh";
    local_apply->icon = "check";
    m_local_apply->enabled = false;
    m_sidebar.add(std::move(local_toggle));
    m_sidebar.add(std::move(local_delete));
    m_sidebar.add(std::move(local_import));
    m_sidebar.add(std::move(local_check));
    m_sidebar.add(std::move(local_apply));
    m_sidebar.visible = false;

    // 兼容保留：状态/目标 Label（隐藏，状态改为自绘）
    auto status = std::make_unique<Label>(L"", 11.5f, theme_text_muted());
    m_status_label = status.get();
    status->visible = false;
    m_sidebar.add(std::move(status));
    auto target = std::make_unique<Label>(L"", 12.0f, theme_text_secondary());
    m_target_label = target.get();
    target->visible = false;
    m_sidebar.add(std::move(target));

    // 详情弹层
    auto detail = std::make_unique<Card>(L"Mod 详情");
    detail->elevated = true;
    detail->visible = false;
    auto detail_title = std::make_unique<Label>(L"", 18.0f, theme_text_primary(), true);
    auto detail_summary = std::make_unique<Label>(L"", 12.0f, theme_text_secondary());
    auto detail_meta = std::make_unique<Label>(L"", 11.5f, theme_text_muted());
    auto detail_message = std::make_unique<Label>(L"", 12.0f, theme_text_secondary());
    auto detail_install = std::make_unique<Button>(L"另存为…", 13.0f, false);
    auto detail_install_to_instance = std::make_unique<Button>(L"安装到实例", 13.0f, true);
    auto detail_close = std::make_unique<Button>(L"关闭", 13.0f, false);
    m_detail_title = detail_title.get();
    m_detail_summary = detail_summary.get();
    m_detail_meta = detail_meta.get();
    m_detail_message = detail_message.get();
    m_detail_install = detail_install.get();
    m_detail_install_to_instance = detail_install_to_instance.get();
    m_detail_close = detail_close.get();
    detail_install->on_click = [this]() { start_prepare_install(); };
    // D-088：接通 Mod 一键安装（解析依赖 → 计划 → 确认 → 执行）。
    detail_install_to_instance->on_click = [this]() { start_install_to_instance(); };
    detail_close->on_click = [this]() { close_detail(); };
    detail_install->icon = "download";
    detail_install_to_instance->icon = "extension";
    detail_close->icon = "close";
    detail->add(std::move(detail_title));
    detail->add(std::move(detail_summary));
    detail->add(std::move(detail_meta));
    detail->add(std::move(detail_message));
    detail->add(std::move(detail_install_to_instance));
    detail->add(std::move(detail_install));
    detail->add(std::move(detail_close));
    m_detail_card = detail.get();
    m_detail_owner = std::move(detail);

    // 安装确认弹层（保留既有链路）
    auto confirm = std::make_unique<Card>(L"确认安装");
    confirm->elevated = true;
    confirm->visible = false;
    auto confirm_title = std::make_unique<Label>(L"", 16.0f, theme_text_primary(), true);
    auto confirm_detail = std::make_unique<Label>(L"", 12.0f, theme_text_secondary());
    auto confirm_message = std::make_unique<Label>(L"", 11.5f, theme_text_muted());
    auto confirm_ok = std::make_unique<Button>(L"确认安装", 13.0f, true);
    auto confirm_cancel = std::make_unique<Button>(L"取消", 13.0f, false);
    m_confirm_title = confirm_title.get();
    m_confirm_detail = confirm_detail.get();
    m_confirm_message = confirm_message.get();
    m_confirm_ok = confirm_ok.get();
    m_confirm_cancel = confirm_cancel.get();
    confirm_ok->on_click = [this]() { confirm_install(); };
    confirm_cancel->on_click = [this]() { close_overlay(); };
    confirm_ok->icon = "check";
    confirm_cancel->icon = "close";
    confirm->add(std::move(confirm_title));
    confirm->add(std::move(confirm_detail));
    confirm->add(std::move(confirm_message));
    confirm->add(std::move(confirm_ok));
    confirm->add(std::move(confirm_cancel));
    m_confirm_card = confirm.get();
    m_confirm_owner = std::move(confirm);

    // 依赖勾选弹层（D-098）：列出将要下载的 Mod 与必需前置，默认勾选尚未安装的项；
    // 确认后提交到后台任务中心，用户可继续浏览其它页面。
    auto dep = std::make_unique<Card>(L"");
    dep->elevated = true;
    dep->visible = false;
    auto dep_title = std::make_unique<Label>(L"确认安装", 16.0f, theme_text_primary(), true);
    auto dep_hint = std::make_unique<Label>(L"", 11.0f, theme_text_secondary());
    auto dep_all = std::make_unique<Button>(L"全不选", 12.0f, false);
    auto dep_start = std::make_unique<Button>(L"开始下载", 12.0f, true);
    auto dep_cancel = std::make_unique<Button>(L"取消", 12.0f, false);
    m_dep_title = dep_title.get();
    m_dep_hint = dep_hint.get();
    m_dep_all = dep_all.get();
    m_dep_start = dep_start.get();
    m_dep_cancel = dep_cancel.get();
    dep_all->on_click = [this]() {
        m_dep_all_checked = !m_dep_all_checked;
        set_all_dependencies(m_dep_all_checked);
    };
    dep_start->on_click = [this]() { start_dependency_install(); };
    dep_cancel->on_click = [this]() { close_dependency_dialog(); };
    dep_start->icon = "download";
    dep_cancel->icon = "close";
    dep->add(std::move(dep_title));
    dep->add(std::move(dep_hint));
    dep->add(std::move(dep_all));
    dep->add(std::move(dep_start));
    dep->add(std::move(dep_cancel));
    m_dep_card = dep.get();
    m_dep_owner = std::move(dep);

    std::wstring target_error;
    if (!ensure_target(&target_error)) {
        set_status(target_error);
    }
}

// ————————————————————————— 布局 —————————————————————————

void AddonsPage::layout(UINT width, UINT height, UINT dpi) {
    m_to_dip = 96.0f / static_cast<float>(dpi == 0 ? 96 : dpi);
    m_width_dip = static_cast<float>(width) * m_to_dip;
    m_height_dip = static_cast<float>(height) * m_to_dip;
    layout_filter();
    layout_search();
    layout_cards();
}

void AddonsPage::layout_filter() {
    const float left = kPageMargin;
    const float right = m_width_dip - kPageMargin;
    const float top = kNavHeightDip + 22.0f;
    m_card_rect = rect_of(left, top, right, top + kFilterCardHeight);

    const float inner_left = left + 25.0f;
    const float inner_right = right - 25.0f;
    const float row1 = top + 46.0f;
    const float row2 = top + 88.0f;
    const float field_h = 32.0f;

    // 行 1：名称（跨左侧大区） | 来源
    m_name_rect = rect_of(inner_left + 50.0f, row1, inner_left + (inner_right - inner_left) * 0.52f,
                          row1 + field_h);
    m_search.bounds = m_name_rect;
    m_source_rect = rect_of(inner_right - 230.0f, row1, inner_right, row1 + field_h);

    // 行 2：版本 + 加载器 | 类型
    m_version_rect = rect_of(inner_left + 50.0f, row2,
                             inner_left + (inner_right - inner_left) * 0.34f, row2 + field_h);
    m_version.bounds = m_version_rect;
    m_loader_rect = rect_of(m_version_rect.right + 10.0f, row2,
                            m_version_rect.right + 170.0f, row2 + field_h);
    m_tag_rect = rect_of(inner_right - 230.0f, row2, inner_right, row2 + field_h);

    // 按钮行（PCL：搜索/重置条件/安装已有整合包；此处第四个为本地 Mod 入口）
    const float btn_y = top + 132.0f;
    float x = inner_left;
    const float btn_w = 140.0f;
    const float gap = 12.0f;
    m_btn_search = rect_of(x, btn_y, x + btn_w, btn_y + 32.0f);
    x += btn_w + gap;
    m_btn_reset = rect_of(x, btn_y, x + btn_w, btn_y + 32.0f);
    x += btn_w + gap;
    m_btn_ranking = rect_of(x, btn_y, x + btn_w, btn_y + 32.0f);
    x += btn_w + gap;
    m_btn_local = rect_of(x, btn_y, x + 120.0f, btn_y + 32.0f);
    if (m_mode == Mode::kLocal) {
        m_btn_local = rect_of(inner_right - 120.0f, btn_y, inner_right, btn_y + 32.0f);
    }

    // 结果卡片的位置/高度由 layout_search() 按行数决定（D-096：整页滚动）。

    // 本地操作条（仅本地模式）
    Button* local_actions[5] = {m_local_toggle, m_local_delete, m_local_import, m_local_check,
                                m_local_apply};
    for (int i = 0; i < 5; ++i) {
        const float bx = inner_left + static_cast<float>(i) * 136.0f;
        local_actions[i]->bounds = rect_of(bx, btn_y, bx + 126.0f, btn_y + 32.0f);
    }
    m_sidebar.visible = (m_mode == Mode::kLocal);
}

void AddonsPage::layout_search() {
    // D-096：整页滚动 —— 筛选卡片在内容最上方，结果列表与分页条随页面一起滚动。
    // 结果卡片高度按行数撑开（不再内部滚动），分页条排在卡片下方。
    const std::size_t count =
        m_mode == Mode::kLocal ? m_local_items.size() : m_results.size();
    const float left = kPageMargin;
    const float right = m_width_dip - kPageMargin;
    const float top = m_card_rect.bottom + 14.0f + (m_warning.empty() ? 0.0f : 34.0f);
    const float list_height =
        (std::max)(static_cast<float>(count) * kRowHeight + 24.0f, 160.0f);
    m_results_rect = rect_of(left, top, right, top + list_height);

    const float center = m_width_dip * 0.5f;
    const float pager_y = m_results_rect.bottom + 16.0f;
    m_pager_prev = rect_of(center - 110.0f, pager_y, center - 60.0f, pager_y + 26.0f);
    m_pager_next = rect_of(center + 60.0f, pager_y, center + 110.0f, pager_y + 26.0f);

    const float content_bottom = m_mode == Mode::kLocal ? m_results_rect.bottom + 16.0f
                                                        : pager_y + 26.0f + 16.0f;
    // 视口：导航条下方到状态条上方，内容可整页滑动（筛选卡片会滚出视野）。
    const float view_top = kNavHeightDip + 8.0f;
    const float view_bottom = m_height_dip - 48.0f;
    // 滚动条贴窗口右边缘（与设置页一致），不再用 width-kPageMargin-6：
    // 那是 D-096 之前「列表内部滚动」的旧坐标，改成整页滚动后会挤在页边距里
    // 贴着结果卡片右缘，且末端垂到卡片下方的空白处（2026-11-12 反馈）。
    m_scroll.layout(view_top, view_bottom, content_bottom - view_top,
                    m_width_dip - kScrollbarInset);
}

void AddonsPage::layout_cards() {
    if (m_detail_card != nullptr) {
        float w = (std::min)(760.0f, m_width_dip - 80.0f);
        w = (std::max)(420.0f, w);
        const float h = (std::min)(540.0f, m_height_dip - kNavHeightDip - 80.0f);
        const float left = (m_width_dip - w) * 0.5f;
        const float top = kNavHeightDip + ((m_height_dip - kNavHeightDip) - h) * 0.5f;
        m_detail_card->bounds = rect_of(left, top, left + w, top + h);
        m_detail_title->bounds = rect_of(left + 100.0f, top + 44.0f, left + w - 20.0f,
                                        top + 72.0f);
        m_detail_summary->bounds = rect_of(left + 100.0f, top + 76.0f, left + w - 20.0f,
                                          top + 100.0f);
        m_detail_meta->bounds = rect_of(left + 100.0f, top + 104.0f, left + w - 20.0f,
                                       top + 126.0f);
        m_detail_message->bounds = rect_of(left + 20.0f, top + 140.0f, left + w - 20.0f,
                                          top + 168.0f);
        m_detail_icon_rect = rect_of(left + 20.0f, top + 44.0f, left + 20.0f + 64.0f,
                                    top + 44.0f + 64.0f);
        const float button_y = top + h - 48.0f;
        // D-088：三按钮——安装到实例（主）/ 另存为 / 关闭。
        m_detail_install_to_instance->bounds =
            rect_of(left + w - 164.0f, button_y, left + w - 20.0f, button_y + 32.0f);
        m_detail_install->bounds = rect_of(left + w - 292.0f, button_y, left + w - 176.0f,
                                           button_y + 32.0f);
        m_detail_close->bounds =
            rect_of((std::max)(left + 20.0f, left + w - 420.0f), button_y,
                    left + w - 304.0f, button_y + 32.0f);
    }
    if (m_confirm_card != nullptr) {
        float w = (std::min)(520.0f, m_width_dip - 100.0f);
        w = (std::max)(400.0f, w);
        const float h = 300.0f;
        const float left = (m_width_dip - w) * 0.5f;
        const float top = kNavHeightDip + ((m_height_dip - kNavHeightDip) - h) * 0.5f;
        m_confirm_card->bounds = rect_of(left, top, left + w, top + h);
        m_confirm_title->bounds = rect_of(left + 20.0f, top + 48.0f, left + w - 20.0f,
                                          top + 76.0f);
        m_confirm_detail->bounds = rect_of(left + 20.0f, top + 86.0f, left + w - 20.0f,
                                           top + 112.0f);
        m_confirm_message->bounds = rect_of(left + 20.0f, top + 118.0f, left + w - 20.0f,
                                            top + 168.0f);
        const float button_y = top + h - 48.0f;
        m_confirm_ok->bounds = rect_of(left + w - 152.0f, button_y, left + w - 20.0f,
                                       button_y + 32.0f);
        m_confirm_cancel->bounds = rect_of(left + w - 260.0f, button_y, left + w - 164.0f,
                                           button_y + 32.0f);
    }
    layout_dependency_dialog();
    rebuild_version_rows();
}

void AddonsPage::layout_dependency_dialog() {
    if (m_dep_card == nullptr) {
        return;
    }
    // 宽度与详情卡片同量级：700 上限，窄窗口下保底 420。
    const float w = (std::max)(420.0f, (std::min)(700.0f, m_width_dip - 80.0f));
    const float rows = static_cast<float>(m_dep_rows.size());
    // 列表高度随条数增长，最多 8 行可视；超出滚动，并保证整卡不出窗口（D-098）。
    const float max_h = (std::max)(260.0f, m_height_dip - kNavHeightDip - 40.0f);
    float list_height =
        (std::min)(8.0f * kDepRowPitch, (std::max)(kDepRowPitch, rows * kDepRowPitch));
    list_height = (std::min)(list_height, (std::max)(kDepRowPitch, max_h - 144.0f));
    const float h = 84.0f + list_height + 60.0f;
    const float left = (m_width_dip - w) * 0.5f;
    const float top = kNavHeightDip + ((m_height_dip - kNavHeightDip) - h) * 0.5f;
    m_dep_card->bounds = rect_of(left, top, left + w, top + h);
    m_dep_title->bounds = rect_of(left + 24.0f, top + 20.0f, left + w - 24.0f, top + 48.0f);
    m_dep_hint->bounds = rect_of(left + 24.0f, top + 50.0f, left + w - 24.0f, top + 72.0f);
    m_dep_list_rect =
        rect_of(left + 16.0f, top + 82.0f, left + w - 16.0f, top + 82.0f + list_height);
    const float button_y = top + h - 48.0f;
    m_dep_start->bounds =
        rect_of(left + w - 148.0f, button_y, left + w - 24.0f, button_y + 32.0f);
    m_dep_cancel->bounds =
        rect_of(left + w - 252.0f, button_y, left + w - 160.0f, button_y + 32.0f);
    m_dep_all->bounds = rect_of(left + 24.0f, button_y, left + 128.0f, button_y + 32.0f);
    m_dep_scroll.layout(m_dep_list_rect.top, m_dep_list_rect.bottom, rows * kDepRowPitch,
                        m_dep_list_rect.right - 6.0f);
}

// ————————————————————————— 状态 / 模式 —————————————————————————

bool AddonsPage::resolve_selected(sscl::core::InstanceTarget* target, std::wstring* error) {
    if (target != nullptr) {
        *target = sscl::core::InstanceTarget{};
    }
    if (m_service == nullptr || m_target_version_id.empty()) {
        return false;  // 没有选中版本：PCL 允许直接下载到 MC 根/兼容实例。
    }
    util::Result<sscl::core::InstanceTarget> resolved =
        m_service->resolve_target(m_target_version_id);
    if (resolved.is_error()) {
        if (error != nullptr) {
            *error = L"无法解析目标版本：" + widen(resolved.error().message);
        }
        return false;
    }
    if (target != nullptr) {
        *target = std::move(resolved.value());
    }
    return true;
}

bool AddonsPage::ensure_target(std::wstring* error) {
    sscl::core::InstanceTarget target;
    const bool has_target = resolve_selected(&target, error);
    if (error != nullptr && !error->empty()) {
        return false;
    }
    m_target_text = has_target
                        ? L"目标：" + widen(target.version_id) + L" · " +
                              widen(target.minecraft_version)
                        : L"目标：未选择版本（下载到 MC 根/兼容实例）";
    if (m_target_label != nullptr) {
        m_target_label->text = m_target_text;
    }
    return true;
}

void AddonsPage::refresh_curseforge_key() {
    // 优先级与构造时一致：用户自填（DPAPI） > 构建期/运行时环境变量。
    std::string key = std::string(sscl::core::curseforge_api_key());
    const util::Result<std::string> secret = sscl::core::load_secret_value(
        ::sscl::platform::data_dir() / L"secrets.dat", "curseforge_api_key");
    if (secret.is_ok() && !secret.value().empty()) {
        key = secret.value();
    }
    if (key == m_catalog_key) {
        return;
    }
    m_catalog_key = key;
    m_catalog = std::make_unique<sscl::core::ModCatalog>(
        sscl::core::make_mod_catalog_options(::sscl::platform::data_dir(), key, true));
}

void AddonsPage::on_enter() {
    // D-106：设置页改 Key 后切回本页立即生效（放在 m_auto_loaded 判断之前）。
    refresh_curseforge_key();
    if (m_auto_loaded) {
        return;
    }
    m_auto_loaded = true;
    // PCL PageResource_Loaded：首次进入自动搜索；无筛选条件时即按下载量排行。
    if (!m_searching) {
        start_search();
    }
}

void AddonsPage::set_status(std::wstring text, float ttl_ms) {
    m_status = std::move(text);
    if (m_status_label != nullptr) {
        m_status_label->text = m_status;
    }
    m_status_ttl_ms = ttl_ms;
    m_status_elapsed_ms = 0.0f;
    if (m_window != nullptr) {
        m_window->invalidate();
    }
}

std::wstring AddonsPage::default_status_text() const {
    if (m_mode == Mode::kLocal) {
        return m_local_status.empty() ? std::wstring(L"本地 Mod：选择一项后可启用/禁用/删除。")
                                      : m_local_status;
    }
    return L"输入关键词搜索 Mod，或点击「下载排行」。";
}

bool AddonsPage::tick_status(float dt_ms) {
    if (m_status_ttl_ms <= 0.0f) {
        return false;
    }
    m_status_elapsed_ms += dt_ms;
    if (m_status_elapsed_ms < m_status_ttl_ms) {
        return false;
    }
    m_status_ttl_ms = 0.0f;
    m_status_elapsed_ms = 0.0f;
    m_status = default_status_text();
    if (m_status_label != nullptr) {
        m_status_label->text = m_status;
    }
    return true;
}

void AddonsPage::set_mode(Mode mode) {
    if (m_mode == mode) {
        return;
    }
    m_mode = mode;
    motion::content().jump();  // D-108：页内切换瞬时；全局 content 已被整页转场占用
    m_search.set_focused(false);
    m_version.set_focused(false);
    close_dropdown();
    close_overlay();
    close_detail();
    layout_filter();
    layout_search();
    m_scroll.jump_to(0.0f);  // D-096：换模式回到内容顶部（筛选卡片可见）
    if (mode == Mode::kLocal) {
        refresh_local();
        set_status(L"已加载本地 Mod。选中一行后可启停/删除。");
    } else {
        set_status(L"输入关键词搜索 Mod；下载会弹出另存为。");
        if (!m_searching) {
            start_search();
        }
    }
    if (m_window != nullptr) {
        m_window->invalidate();
    }
}

// ————————————————————————— 筛选 —————————————————————————

void AddonsPage::reset_filter() {
    m_search.text.clear();
    m_version.text = kVersionOptions[0];
    m_source_index = 0;
    m_loader_index = 0;
    m_tag_index = 0;
    m_source_text = kSourceOptions[0];
    m_loader_text = kLoaderOptions[0];
    m_tag_text = kCategories[0].label;
    m_platform = sscl::core::ModPlatform::kModrinth;
    if (on_platform_changed) {
        on_platform_changed(m_platform);
    }
    m_filter_loader = sscl::core::ModLoader::kUnknown;
    m_category.clear();
    m_category_cf.clear();
    m_page = 0;
    m_ranking = true;
    close_dropdown();
    start_search();
}

void AddonsPage::close_dropdown() {
    m_dropdown = Dropdown::kNone;
    m_dropdown_labels.clear();
    m_dropdown_items.clear();
    m_dropdown_offset = 0.0f;
    m_dropdown_hover = -1;
}

void AddonsPage::open_dropdown(Dropdown kind) {
    m_dropdown = kind;
    m_dropdown_labels.clear();
    m_dropdown_offset = 0.0f;
    m_dropdown_hover = -1;
    D2D1_RECT_F anchor{};
    if (kind == Dropdown::kSource) {
        for (const wchar_t* item : kSourceOptions) m_dropdown_labels.push_back(item);
        anchor = m_source_rect;
    } else if (kind == Dropdown::kVersion) {
        for (const wchar_t* item : kVersionOptions) m_dropdown_labels.push_back(item);
        anchor = m_version_rect;
    } else if (kind == Dropdown::kLoader) {
        for (const wchar_t* item : kLoaderOptions) m_dropdown_labels.push_back(item);
        anchor = m_loader_rect;
    } else {
        for (const auto& item : kCategories) m_dropdown_labels.push_back(item.label);
        anchor = m_tag_rect;
    }
    const float width = (std::max)(anchor.right - anchor.left, 180.0f);
    const float max_h = 340.0f;
    const float wanted = static_cast<float>(m_dropdown_labels.size()) * 28.0f + 8.0f;
    const float height = (std::min)(wanted, max_h);
    float left = anchor.left;
    if (left + width > m_width_dip - 12.0f) {
        left = m_width_dip - 12.0f - width;
    }
    float top = anchor.bottom + 4.0f;
    // 锚点与菜单都在「内容坐标」中（D-096），可见下界要加回滚动偏移。
    if (top + height > m_height_dip + m_scroll.offset() - 12.0f) {
        top = anchor.top - height - 4.0f;
    }
    m_dropdown_rect = rect_of(left, top, left + width, top + height);
    rebuild_dropdown_items();
    if (request_repaint) {
        request_repaint();
    }
}

void AddonsPage::rebuild_dropdown_items() {
    m_dropdown_items.clear();
    const float width = m_dropdown_rect.right - m_dropdown_rect.left;
    for (std::size_t i = 0; i < m_dropdown_labels.size(); ++i) {
        const float y = m_dropdown_rect.top + 4.0f + static_cast<float>(i) * 28.0f -
                        m_dropdown_offset;
        m_dropdown_items.push_back(rect_of(m_dropdown_rect.left + 4.0f, y,
                                          m_dropdown_rect.left + width - 4.0f, y + 28.0f));
    }
}

void AddonsPage::apply_dropdown(std::size_t index) {
    if (m_dropdown == Dropdown::kSource) {
        if (index >= kSourceCount) return;
        m_source_index = static_cast<int>(index);
        m_source_text = kSourceOptions[index];
        m_platform = index == 0 ? sscl::core::ModPlatform::kModrinth
                                : sscl::core::ModPlatform::kCurseForge;
        if (on_platform_changed) {
            on_platform_changed(m_platform);
        }
        // 单平台模式下切换来源必须立即换结果，否则列表仍是上一个平台的内容。
        m_page = 0;
        motion::content().jump();  // D-108：页内切换瞬时；全局 content 已被整页转场占用
        if (!m_searching) {
            start_search();
        }
    } else if (m_dropdown == Dropdown::kVersion) {
        if (index >= sizeof(kVersionOptions) / sizeof(kVersionOptions[0])) return;
        m_version.text = kVersionOptions[index];
    } else if (m_dropdown == Dropdown::kLoader) {
        if (index >= 4) return;
        m_loader_index = static_cast<int>(index);
        m_loader_text = kLoaderOptions[index];
        switch (index) {
            case 1: m_filter_loader = sscl::core::ModLoader::kForge; break;
            case 2: m_filter_loader = sscl::core::ModLoader::kNeoForge; break;
            case 3: m_filter_loader = sscl::core::ModLoader::kFabric; break;
            default: m_filter_loader = sscl::core::ModLoader::kUnknown; break;
        }
    } else {
        if (index >= sizeof(kCategories) / sizeof(kCategories[0])) return;
        m_tag_index = static_cast<int>(index);
        m_tag_text = kCategories[index].label;
        const std::string tag = kCategories[index].tag;
        const std::size_t slash = tag.find('/');
        if (slash != std::string::npos) {
            m_category_cf = tag.substr(0, slash);
            m_category = tag.substr(slash + 1);
        } else {
            m_category_cf.clear();
            m_category = tag;
        }
    }
    close_dropdown();
    if (m_window != nullptr) {
        m_window->invalidate();
    }
}

void AddonsPage::change_page(int delta) {
    const int next = m_page + delta;
    if (next < 0 || (delta > 0 && !m_has_more)) {
        return;
    }
    m_page = next;
    start_search();
}

void AddonsPage::start_search() {
    if (m_searching) {
        return;
    }
    std::wstring error;
    if (!ensure_target(&error)) {
        set_status(error);
        return;
    }
    sscl::core::InstanceTarget target;
    std::wstring target_error;
    const bool has_target = resolve_selected(&target, &target_error);
    if (!target_error.empty()) {
        set_status(target_error);
        return;
    }

    const std::string text = sscl::util::wide_to_utf8(m_search.text);
    const std::wstring version = m_version.text;
    const std::string version_utf8 = sscl::util::wide_to_utf8(version);
    const bool version_valid = version_utf8.find('.') != std::string::npos ||
                               version_utf8.find('w') != std::string::npos;
    std::string mc = version_valid ? version_utf8
                                   : safe_mc_version(has_target ? target.minecraft_version
                                                                : std::string());
    const sscl::core::ModLoader loader =
        m_filter_loader != sscl::core::ModLoader::kUnknown
            ? m_filter_loader
            : (has_target ? loader_of(target) : sscl::core::ModLoader::kUnknown);
    m_ranking = text.empty() && m_category.empty() && m_category_cf.empty() &&
                m_filter_loader == sscl::core::ModLoader::kUnknown && !version_valid;

    sscl::core::ModSearchQuery query;
    query.platform = m_platform;
    query.text = text;
    query.game_version = mc;
    query.loader = loader;
    query.category = m_category;
    query.category_cf = m_category_cf;
    query.sort = m_ranking ? sscl::core::ModSort::kDownloads : sscl::core::ModSort::kRelevance;
    query.limit = kPageSize;
    query.offset = m_page * kPageSize;

    m_searching = true;
    m_warning.clear();
    set_status(m_ranking ? L"正在获取下载排行…" : L"正在搜索…");
    const std::uint64_t generation = ++m_search_generation;
    std::shared_ptr<AddonsPage> self = shared_from_this();
    std::thread([self, query, generation]() {
        // 来源已收敛为单平台，直接查所选平台，不再走双平台合并（避免等最慢的一边）。
        util::Result<sscl::core::ModSearchPage> page = self->m_catalog->search(query, {});
        std::vector<sscl::core::ModProject> items;
        std::wstring message;
        std::wstring warning;
        if (page.is_ok()) {
            items = std::move(page.value().items);
            message = (query.text.empty() ? L"下载排行 · 共 " : L"找到 ") +
                      std::to_wstring(items.size()) + L" 个结果";
            if (!page.value().warning.empty()) {
                warning = widen(page.value().warning);
                message += L"（" + warning + L"）";
            }
        } else {
            message = L"搜索失败：" + widen(page.error().message);
        }
        self->m_window->post_to_ui([self, generation, items = std::move(items),
                                    message = std::move(message),
                                    warning = std::move(warning),
                                    has_more = page.is_ok() && page.value().has_more]() mutable {
            if (self->m_search_generation != generation) {
                return;
            }
            self->m_searching = false;
            self->m_results = std::move(items);
            self->m_warning = std::move(warning);
            self->m_has_more = has_more;
            self->m_hover_row = static_cast<std::size_t>(-1);
            self->m_scroll.jump_to(0.0f);
            self->set_status(message);
            self->layout_filter();
            self->layout_search();
            if (self->request_repaint) {
                self->request_repaint();
            }
        });
    }).detach();
}

// ————————————————————————— 本地 Mod 管理（沿用既有实现） —————————————————————————

void AddonsPage::refresh_local() {
    m_local_items.clear();
    m_local_selected = static_cast<std::size_t>(-1);
    if (m_local_apply != nullptr) {
        m_local_apply->enabled = false;
    }
    sscl::core::InstanceTarget target;
    std::wstring error;
    if (!resolve_selected(&target, &error) || !error.empty()) {
        m_local_status = error.empty() ? L"请先选择一个已安装版本。" : error;
        set_status(m_local_status);
        layout_search();
        return;
    }
    const std::vector<sscl::core::LocalModEntry> entries =
        sscl::core::scan_pcl_mods(target.mods_dir);
    for (const auto& entry : entries) {
        sscl::core::ModUpdateInfo item;
        item.local = entry;
        m_local_items.push_back(std::move(item));
    }
    m_local_status = L"本地 Mod：" + std::to_wstring(m_local_items.size()) + L" 个";
    set_status(m_local_status);
    layout_search();
    m_scroll.jump_to(0.0f);
}

void AddonsPage::toggle_selected_mod() {
    if (m_local_selected >= m_local_items.size()) {
        return;
    }
    const auto& item = m_local_items[m_local_selected];
    util::Result<void> toggled =
        sscl::core::set_mod_enabled(item.local.full_path, !item.local.enabled);
    if (toggled.is_error()) {
        set_status(L"切换失败：" + widen(toggled.error().message));
        return;
    }
    refresh_local();
    set_status(L"已切换 Mod 状态。");
}

void AddonsPage::delete_selected_mod() {
    if (m_local_selected >= m_local_items.size()) {
        return;
    }
    const auto& item = m_local_items[m_local_selected];
    const bool permanent = ::GetKeyState(VK_SHIFT) < 0;
    const bool deleted = permanent ? sscl::platform::delete_file_permanent(item.local.full_path)
                                   : sscl::platform::recycle_file(item.local.full_path);
    if (!deleted) {
        set_status(L"删除失败（文件可能被游戏占用）。");
        return;
    }
    refresh_local();
    set_status(permanent ? L"已永久删除 Mod。" : L"已删除 Mod。");
}

void AddonsPage::import_local_mods() {
    sscl::core::InstanceTarget target;
    std::wstring error;
    if (!resolve_selected(&target, &error) || !error.empty()) {
        set_status(error.empty() ? L"请先选择一个已安装版本。" : error);
        return;
    }
    std::vector<std::wstring> selected;
    if (!sscl::platform::open_file_dialog(
            m_window != nullptr ? m_window->hwnd() : nullptr, L"导入 Mod",
            L"Mod 文件\0*.jar;*.litemod;*.zip;*.disabled;*.old\0所有文件\0*.*\0\0",
            &selected)) {
        return;
    }
    std::vector<std::filesystem::path> files;
    for (const std::wstring& path : selected) {
        files.emplace_back(path);
    }
    util::Result<int> imported = sscl::core::import_mod_files(target.mods_dir, files);
    if (imported.is_error()) {
        set_status(L"导入失败：" + widen(imported.error().message));
        return;
    }
    refresh_local();
    set_status(L"已导入 " + std::to_wstring(imported.value()) + L" 个文件。");
}

void AddonsPage::check_local_updates() {
    if (m_local_loading) {
        return;
    }
    sscl::core::InstanceTarget target;
    std::wstring error;
    if (!resolve_selected(&target, &error) || !error.empty()) {
        set_status(error.empty() ? L"请先选择一个已安装版本。" : error);
        return;
    }
    m_local_loading = true;
    m_local_stop = std::stop_source{};
    const std::stop_token stop = m_local_stop.get_token();
    m_local_status = L"正在检查 Mod 更新…";
    set_status(m_local_status);
    std::shared_ptr<AddonsPage> self = shared_from_this();
    const std::string game_version = safe_mc_version(target.minecraft_version);
    const sscl::core::ModLoader loader = loader_of(target);
    const std::filesystem::path mods_dir = target.mods_dir;
    std::thread([self, game_version, loader, mods_dir, stop]() {
        sscl::core::ModUpdateOptions options;
        options.game_version = game_version;
        options.loader = loader;
        sscl::core::ModHashCache cache(::sscl::platform::data_dir() / L"cache" /
                                       L"mod_hashes.json");
        util::Result<sscl::core::ModUpdateReport> report = sscl::core::check_mod_updates(
            *self->m_catalog, mods_dir, options, cache, stop);
        std::wstring message;
        std::vector<sscl::core::ModUpdateInfo> items;
        if (report.is_ok()) {
            items = std::move(report.value().items);
            message = L"检查完成：可更新 " + std::to_wstring(report.value().with_update) +
                      L" 个";
            if (!report.value().warning.empty()) {
                message += L"（" + widen(report.value().warning) + L"）";
            }
        } else {
            message = L"检查失败：" + widen(report.error().message);
        }
        self->m_window->post_to_ui([self, items = std::move(items),
                                    message = std::move(message)]() mutable {
            self->m_local_loading = false;
            self->m_local_items = std::move(items);
            self->m_local_selected = static_cast<std::size_t>(-1);
            self->m_local_status = message;
            self->set_status(message);
            self->m_local_apply->enabled =
                std::any_of(self->m_local_items.begin(), self->m_local_items.end(),
                            [](const sscl::core::ModUpdateInfo& item) {
                                return item.has_update;
                            });
            self->layout_search();
            if (self->request_repaint) {
                self->request_repaint();
            }
        });
    }).detach();
}

void AddonsPage::apply_local_updates() {
    if (m_local_loading || m_local_items.empty()) {
        return;
    }
    std::vector<sscl::core::ModUpdateInfo> updates;
    for (const auto& item : m_local_items) {
        if (item.has_update) {
            updates.push_back(item);
        }
    }
    if (updates.empty()) {
        set_status(L"没有可应用的更新。");
        return;
    }
    m_local_loading = true;
    m_local_stop = std::stop_source{};
    const std::stop_token stop = m_local_stop.get_token();
    m_local_status = L"正在应用更新…";
    set_status(m_local_status);
    std::shared_ptr<AddonsPage> self = shared_from_this();
    std::thread([self, updates = std::move(updates), stop]() {
        util::Result<int> applied = sscl::core::apply_mod_updates(
            updates, self->m_service->scheduler(),
            ::sscl::platform::data_dir() / L"cache" / L"mod_update_tmp", nullptr, stop);
        std::wstring message;
        if (applied.is_ok()) {
            message = L"已更新 " + std::to_wstring(applied.value()) + L" 个 Mod";
        } else {
            message = L"更新失败：" + widen(applied.error().message);
        }
        self->m_window->post_to_ui([self, message = std::move(message)]() mutable {
            self->m_local_loading = false;
            self->m_local_apply->enabled = false;
            self->refresh_local();
            self->set_status(message);
            if (self->request_repaint) {
                self->request_repaint();
            }
        });
    }).detach();
}

// ————————————————————————— 详情 / 另存为（沿用既有实现） —————————————————————————

void AddonsPage::open_detail(std::size_t index) {
    if (index >= m_results.size() || m_detail_loading) {
        return;
    }
    std::wstring error;
    if (!ensure_target(&error)) {
        set_status(error);
        return;
    }
    sscl::core::InstanceTarget target;
    std::wstring target_error;
    const bool has_target = resolve_selected(&target, &target_error);
    if (!target_error.empty()) {
        set_status(target_error);
        return;
    }
    // D-088：打开详情前清掉确认弹层残留，避免共用 modal 单例时误画。
    m_confirm_open = false;
    m_confirm_closing = false;
    if (m_confirm_card != nullptr) {
        m_confirm_card->visible = false;
    }
    m_detail_project = m_results[index];
    m_detail_platform = m_results[index].platform;
    m_detail_versions.clear();
    m_version_rows.clear();
    m_detail_version = sscl::core::ModVersion{};
    m_detail_loading = true;
    m_detail_open = true;
    m_detail_card->visible = true;
    motion::modal().open();  // 淡入 + 0.96→1 + 上移 8px（D-081）
    m_detail_title->text = widen(m_detail_project.title);
    m_detail_summary->text = widen(m_detail_project.summary);
    m_detail_meta->text = L"作者：" + widen(m_detail_project.author) + L" · 下载 " +
                          format_downloads(m_detail_project.downloads);
    m_detail_message->text = L"正在获取版本列表…";
    m_detail_install->enabled = false;
    m_detail_install_to_instance->enabled = false;
    layout_cards();
    const std::uint64_t generation = ++m_detail_generation;
    std::shared_ptr<AddonsPage> self = shared_from_this();
    const sscl::core::ModPlatform platform = m_detail_platform;
    const std::string project_id = m_detail_project.id;
    // PCL：详情页沿用列表的筛选条件（版本、加载器）。
    const std::string version_utf8 = sscl::util::wide_to_utf8(m_version.text);
    const bool version_valid = version_utf8.find('.') != std::string::npos ||
                               version_utf8.find('w') != std::string::npos;
    const std::string mc = version_valid ? version_utf8
                                        : safe_mc_version(has_target ? target.minecraft_version
                                                                     : std::string());
    const sscl::core::ModLoader loader =
        m_filter_loader != sscl::core::ModLoader::kUnknown
            ? m_filter_loader
            : (has_target ? loader_of(target) : sscl::core::ModLoader::kUnknown);
    std::thread([self, platform, project_id, mc, loader, generation]() {
        util::Result<sscl::core::ModProject> project =
            self->m_catalog->project(platform, project_id, {});
        util::Result<std::vector<sscl::core::ModVersion>> versions =
            self->m_catalog->versions(platform, project_id, mc, loader, false, {});
        std::vector<sscl::core::ModVersion> list;
        std::wstring message;
        if (versions.is_ok()) {
            list = std::move(versions.value());
        } else {
            message = L"获取版本失败：" + widen(versions.error().message);
        }
        if (list.empty()) {
            util::Result<std::vector<sscl::core::ModVersion>> all =
                self->m_catalog->versions(platform, project_id, mc, loader, true, {});
            if (all.is_ok()) {
                for (const auto& version : all.value()) {
                    if (version_matches(version, mc, loader)) {
                        list.push_back(version);
                    }
                }
            }
        }
        self->m_window->post_to_ui([self, generation, project = std::move(project),
                                    list = std::move(list), message = std::move(message)]() mutable {
            if (self->m_detail_generation != generation || !self->m_detail_open) {
                return;
            }
            self->m_detail_loading = false;
            if (project.is_ok()) {
                self->m_detail_project = project.value();
                self->m_detail_title->text = widen(self->m_detail_project.title);
                self->m_detail_summary->text = widen(self->m_detail_project.summary);
                self->m_detail_meta->text = L"作者：" + widen(self->m_detail_project.author) +
                                            L" · 下载 " +
                                            format_downloads(self->m_detail_project.downloads);
            }
            self->m_detail_versions = std::move(list);
            if (self->m_detail_versions.empty()) {
                self->m_detail_message->text =
                    message.empty() ? L"没有适配当前版本/加载器的文件。" : message;
                self->m_detail_install->enabled = false;
                self->m_version_rows.clear();
            } else {
                std::size_t selected = 0;
                for (std::size_t i = 0; i < self->m_detail_versions.size(); ++i) {
                    if (self->m_detail_versions[i].channel == sscl::core::ModChannel::kRelease) {
                        selected = i;
                        break;
                    }
                }
                self->select_detail_version(selected);
                self->rebuild_version_rows();
            }
            self->layout_cards();
            if (self->request_repaint) {
                self->request_repaint();
            }
        });
    }).detach();
}

void AddonsPage::rebuild_version_rows() {
    m_version_rows.clear();
    if (m_detail_card == nullptr || !m_detail_card->visible || m_detail_versions.empty()) {
        return;
    }
    const D2D1_RECT_F card = m_detail_card->bounds;
    const float left = card.left + 20.0f;
    const float right = card.right - 20.0f;
    const float top = card.top + 196.0f;
    const float bottom = card.bottom - 60.0f;
    if (bottom <= top) {
        return;
    }
    const std::size_t capacity = static_cast<std::size_t>((bottom - top) / kVersionRowHeight);
    const std::size_t count = (std::min)(m_detail_versions.size(), capacity);
    for (std::size_t i = 0; i < count; ++i) {
        const float y = top + static_cast<float>(i) * kVersionRowHeight;
        m_version_rows.push_back(rect_of(left, y, right, y + kVersionRowHeight - 2.0f));
    }
}

void AddonsPage::select_detail_version(std::size_t index) {
    if (index >= m_detail_versions.size()) {
        return;
    }
    m_detail_version_index = index;
    m_detail_version = m_detail_versions[index];
    m_detail_message->text = describe_version(m_detail_version);
    const sscl::core::ModFile* file = sscl::core::select_primary_file(m_detail_version);
    m_detail_install->enabled = file != nullptr && !file->url.empty();
    m_detail_install_to_instance->enabled = file != nullptr && !file->url.empty();
    if (m_window != nullptr) {
        m_window->invalidate();
    }
    if (request_repaint) {
        request_repaint();
    }
}

void AddonsPage::draw_detail_extras(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (target == nullptr || dwrite == nullptr || m_detail_card == nullptr) {
        return;
    }
    RemoteImage::instance().request(m_detail_project.icon_url, [this]() {
        if (request_repaint) {
            request_repaint();
        }
    });
    draw_icon(target, dwrite, m_detail_project.icon_url, widen(m_detail_project.title),
              m_detail_icon_rect);

    const D2D1_RECT_F card = m_detail_card->bounds;
    const std::wstring header =
        L"版本列表（共 " + std::to_wstring(m_detail_versions.size()) + L" 个）";
    draw_label(target, dwrite, header,
               rect_of(card.left + 20.0f, card.top + 170.0f, card.right - 20.0f,
                       card.top + 192.0f),
               12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, theme_text_muted());
    for (std::size_t i = 0; i < m_version_rows.size(); ++i) {
        const D2D1_RECT_F& row = m_version_rows[i];
        fill_round(target, row, 5.0f,
                   i == m_detail_version_index ? theme_primary_container() : theme_surface());
        motion::draw_hot_overlay(target, row, 5.0f, theme_hover_background(),
                             theme_primary_container());
        const auto& version = m_detail_versions[i];
        const std::wstring name =
            widen(version.name.empty() ? version.version_number : version.name);
        draw_label(target, dwrite, name,
                   rect_of(row.left + 12.0f, row.top, row.right - 170.0f, row.bottom),
                   11.5f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_primary());
        const sscl::core::ModFile* file = sscl::core::select_primary_file(version);
        const std::wstring meta = channel_text(version.channel) + L" · " +
                                  format_date(version.published_at) + L" · " +
                                  (file != nullptr ? format_size(file->size) : L"—");
        draw_label(target, dwrite, meta,
                   rect_of(row.right - 240.0f, row.top, row.right - 12.0f, row.bottom),
                   10.0f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_muted(),
                   DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

void AddonsPage::start_install_to_instance() {
    if (m_preparing || m_installing || m_detail_version.version_id.empty() ||
        m_detail_version.files.empty() || m_service == nullptr) {
        return;
    }
    const sscl::core::ModFile* file = sscl::core::select_primary_file(m_detail_version);
    if (file == nullptr || file->url.empty()) {
        set_status(L"该文件没有可用下载地址（可能禁止分发）。");
        return;
    }
    // D-088：一键安装必须有明确目标实例；没有选中版本时不能"装到空气里"。
    sscl::core::InstanceTarget target;
    std::wstring target_error;
    const bool has_target = resolve_selected(&target, &target_error);
    if (!target_error.empty() || !has_target) {
        set_status(target_error.empty() ? L"请先在版本选择页选中要安装 Mod 的版本。"
                                        : target_error);
        return;
    }

    m_preparing = true;
    m_detail_install_to_instance->enabled = false;
    m_detail_message->text = L"正在解析前置依赖…";
    set_status(L"正在解析前置依赖…");
    if (request_repaint) {
        request_repaint();
    }

    std::shared_ptr<AddonsPage> self = shared_from_this();
    const sscl::core::ModPlatform platform = m_detail_platform;
    const sscl::core::ModProject root_project = m_detail_project;
    const sscl::core::ModVersion root_version = m_detail_version;
    const std::string mc = safe_mc_version(target.minecraft_version);
    const sscl::core::ModLoader loader = loader_of(target);
    const std::string target_id = target.version_id;

    std::thread([self, platform, root_project, root_version, target, mc, loader,
                 target_id]() {
        // BFS 解析 required 前置：数量上限 32、深度上限 8（与 ModInstallOptions 一致）。
        std::vector<sscl::core::ModResolved> resolved;
        sscl::core::ModResolved root;
        root.project = root_project;
        root.version = root_version;
        root.role = sscl::core::ModPlanRole::kUser;
        root.depth = 0;
        resolved.push_back(root);
        std::vector<std::string> seen{root_project.id};
        std::wstring warnings;
        constexpr std::size_t kMaxResolved = 32;
        for (std::size_t i = 0; i < resolved.size() && resolved.size() < kMaxResolved; ++i) {
            if (resolved[i].depth >= 8) {
                continue;
            }
            for (const auto& dependency : resolved[i].version.dependencies) {
                if (dependency.kind != sscl::core::DependencyKind::kRequired ||
                    dependency.project_id.empty()) {
                    continue;
                }
                if (std::find(seen.begin(), seen.end(), dependency.project_id) != seen.end()) {
                    continue;
                }
                seen.push_back(dependency.project_id);
                util::Result<std::vector<sscl::core::ModVersion>> versions =
                    self->m_catalog->versions(platform, dependency.project_id, mc, loader,
                                              false, {});
                if (versions.is_error() || versions.value().empty()) {
                    if (!warnings.empty()) {
                        warnings += L"、";
                    }
                    warnings += widen(dependency.project_id);
                    continue;
                }
                sscl::core::ModResolved item;
                util::Result<sscl::core::ModProject> project =
                    self->m_catalog->project(platform, dependency.project_id, {});
                if (project.is_ok()) {
                    item.project = project.value();
                } else {
                    item.project.id = dependency.project_id;
                    item.project.title = dependency.project_id;
                }
                item.version = versions.value().front();
                item.role = sscl::core::ModPlanRole::kRequired;
                item.depth = resolved[i].depth + 1;
                resolved.push_back(std::move(item));
            }
        }

        sscl::core::ModHashCache cache(::sscl::platform::data_dir() / L"cache" /
                                       L"mod_hashes.json");
        sscl::core::ModInstallOptions options;
        options.max_dependency_depth = 8;
        sscl::core::ModInstallPlan plan =
            sscl::core::build_install_plan(target, resolved, options, cache);

        self->m_window->post_to_ui(
            [self, plan = std::move(plan), warnings = std::move(warnings), target_id]() mutable {
                self->m_preparing = false;
                self->m_detail_install_to_instance->enabled = true;
                // D-098：先让用户勾选要下载的 Mod/前置（默认勾选未安装的），
                // 确认后提交后台任务，不再占用前台。
                self->open_dependency_dialog(std::move(plan), std::move(warnings), target_id);
            });
    }).detach();
}

void AddonsPage::start_prepare_install() {
    if (m_preparing || m_installing || m_detail_version.version_id.empty() ||
        m_detail_version.files.empty() || m_service == nullptr) {
        return;
    }
    // PCL 2.99：下载 = 另存为，不自动安装、不解析依赖；主文件按 primary 选择。
    const sscl::core::ModFile* file = sscl::core::select_primary_file(m_detail_version);
    if (file == nullptr || file->url.empty()) {
        set_status(L"该文件没有可用下载地址（可能禁止分发）。");
        return;
    }
    sscl::core::InstanceTarget selected;
    std::wstring target_error;
    const bool has_target = resolve_selected(&selected, &target_error);
    if (!target_error.empty()) {
        set_status(target_error);
        return;
    }
    std::vector<sscl::core::InstanceTarget> instances;
    if (m_service != nullptr) {
        for (const auto& installed : m_service->installed_versions()) {
            util::Result<sscl::core::InstanceTarget> resolved =
                m_service->resolve_target(installed.id);
            if (resolved.is_ok()) {
                instances.push_back(std::move(resolved.value()));
            }
        }
    }
    sscl::core::ModDownloadPlan plan = sscl::core::plan_pcl_mod_download(
        m_service != nullptr ? m_service->layout().game_dir : std::filesystem::path(),
        has_target ? &selected : nullptr, instances, m_cached_folder, m_detail_version,
        file->filename, {}, 1);
    std::wstring target_path;
    std::wstring default_name = widen(plan.file_name);
    if (default_name.empty()) {
        default_name = widen(file->filename);
    }
    const std::filesystem::path default_dir = plan.default_folder.empty()
                                                  ? ::sscl::platform::data_dir()
                                                  : plan.default_folder;
    std::error_code ec;
    std::filesystem::create_directories(default_dir, ec);
    const std::wstring ext =
        std::filesystem::path(widen(file->filename)).extension().wstring() == L".litemod"
            ? L"litemod"
            : L"jar";
    if (!sscl::platform::save_file_dialog(
            m_window != nullptr ? m_window->hwnd() : nullptr, L"选择保存位置", default_name,
            default_dir.wstring(), L"Mod 文件\0*.jar;*.litemod\0所有文件\0*.*\0\0",
            ext.c_str(), &target_path)) {
        return;
    }
    const std::filesystem::path chosen(target_path);
    if (!chosen.parent_path().empty()) {
        m_cached_folder = sscl::util::wide_to_utf8(chosen.parent_path().wstring());
    }
    sscl::core::DownloadRequest request;
    request.url = file->url;
    request.destination = chosen;
    request.expected_sha1 = file->sha1;
    request.expected_size = file->size > 0 ? static_cast<std::int64_t>(file->size) : -1;
    request.display_name = file->filename;
    // D-098：「另存为」同样走后台任务中心，不再占用前台页面。
    std::shared_ptr<sscl::core::GameService> service = m_service;
    sscl::core::TaskCenter::instance().submit(
        "下载 Mod 文件", request.display_name,
        [service, request](sscl::core::JobContext& context) -> sscl::util::Result<void> {
            const sscl::core::DownloadProgressFn progress =
                [&context, &request](std::uint64_t done, std::uint64_t total) {
                    context.report(0, 0, done, total, request.display_name, "下载 Mod 文件");
                };
            return service->scheduler().download_file(request, progress,
                                                      context.stop_token());
        });
    m_detail_message->text = L"已加入后台任务…";
    set_status(L"已加入后台任务：下载到 " + target_path + L"；可在「下载」页查看进度。",
               6000.0f);
    if (request_repaint) {
        request_repaint();
    }
}

void AddonsPage::confirm_install() {
    if (!m_confirm_open || m_installing || m_plan.items.empty()) {
        return;
    }
    m_plan_options.overwrite_conflicts = m_plan.has_conflicts();
    // D-088：安装期间保持确认弹层可见：主按钮禁用、次按钮改为「取消安装」，
    // 让 P0 审计要求的"可取消安装入口"真正存在。
    m_confirm_ok->enabled = false;
    m_confirm_cancel->text = L"取消安装";
    m_confirm_cancel->on_click = [this]() { cancel_install(); };
    m_confirm_title->text = L"正在安装…";
    m_confirm_message->text = L"可点击「取消安装」中止。";
    if (request_repaint) {
        request_repaint();
    }
    execute_install();
}

void AddonsPage::execute_install() {
    if (m_installing || m_plan.items.empty()) {
        return;
    }
    m_installing = true;
    m_install_stop = std::stop_source{};
    m_install_message = L"正在安装…";
    m_detail_message->text = m_install_message;
    m_detail_install->enabled = false;
    const std::stop_token stop = m_install_stop.get_token();
    std::shared_ptr<AddonsPage> self = shared_from_this();
    const sscl::core::ModInstallPlan plan = m_plan;
    const sscl::core::ModInstallOptions options = m_plan_options;
    std::thread([self, plan, options, stop]() {
        sscl::core::ModHashCache cache(::sscl::platform::data_dir() / L"cache" /
                                       L"mod_hashes.json");
        const sscl::core::ModInstallProgressFn progress =
            [self](const sscl::core::ModInstallProgress& current) {
                self->m_window->post_to_ui([self, current]() {
                    if (!self->m_installing) {
                        return;
                    }
                    self->m_install_message =
                        L"安装中 " + std::to_wstring(current.completed_files) + L"/" +
                        std::to_wstring(current.total_files) + L"：" + widen(current.current_file);
                    self->m_detail_message->text = self->m_install_message;
                    if (self->request_repaint) {
                        self->request_repaint();
                    }
                });
            };
        util::Result<void> result = sscl::core::install_mod_plan(
            plan, options, self->m_service->scheduler(), cache,
            ::sscl::platform::data_dir() / L"cache" / L"mod_tmp", progress, stop);
        std::wstring message;
        if (result.is_ok()) {
            message = L"安装完成。";
        } else if (result.error().code == "mod.cancelled") {
            message = L"安装已取消。";
        } else {
            message = L"安装失败：" + widen(result.error().message);
        }
        self->m_window->post_to_ui([self, message = std::move(message)]() mutable {
            self->m_installing = false;
            self->m_install_message = message;
            self->m_detail_message->text = message;
            self->m_detail_install->enabled = !self->m_detail_version.version_id.empty();
            self->m_detail_install_to_instance->enabled =
                !self->m_detail_version.version_id.empty();
            // D-088：安装结束关闭确认弹层（淡出），恢复按钮初始态。
            self->m_confirm_open = false;
            self->m_confirm_closing = true;
            self->m_confirm_ok->enabled = true;
            self->m_confirm_cancel->text = L"取消";
            self->m_confirm_cancel->on_click = [self]() { self->close_overlay(); };
            motion::modal().close();
            self->set_status(message);
            if (self->request_repaint) {
                self->request_repaint();
            }
        });
    }).detach();
}

void AddonsPage::cancel_install() {
    if (m_installing) {
        m_install_stop.request_stop();
    }
}

void AddonsPage::close_overlay() {
    m_confirm_open = false;
    // D-088：卡片保留到淡出结束；用独立 closing 标志参与绘制，
    // 避免详情弹层打开时（同一 modal 单例已 open）把旧确认卡片误画出来。
    m_confirm_closing = true;
    motion::modal().close();
    if (request_repaint) {
        request_repaint();
    }
}

void AddonsPage::close_detail() {
    if (m_installing) {
        return;
    }
    ++m_detail_generation;
    m_detail_open = false;
    m_detail_loading = false;
    motion::modal().close();
    // 卡片内容保留到淡出结束（下次 open 时重新置 visible）。
    m_hover_version = -1;
}

// ————————————————————— 依赖勾选 / 后台任务（D-098）—————————————————————

void AddonsPage::open_dependency_dialog(sscl::core::ModInstallPlan plan, std::wstring warnings,
                                        std::string target_id) {
    m_plan = std::move(plan);
    m_dep_warnings = std::move(warnings);
    m_dep_target_id = std::move(target_id);
    m_dep_rows.clear();
    for (std::size_t i = 0; i < m_plan.items.size(); ++i) {
        const sscl::core::ModPlanItem& item = m_plan.items[i];
        DepRow row;
        row.title = widen(item.project.title.empty() ? item.project.id : item.project.title);
        row.version = widen(item.version.version_number);
        row.size = item.file.size > 0 ? static_cast<std::uint64_t>(item.file.size) : 0;
        row.plan_index = i;
        row.enabled = item.action != sscl::core::ModPlanAction::kUnavailable;
          // 默认勾选「尚未下载」的项（同名同哈希 = 已安装，同名不同内容 = 已有文件，
          // 两者默认不勾）；根 Mod 永远勾上（用户就是冲它来的）。
          const bool installed = item.action == sscl::core::ModPlanAction::kSkipSame;
          const bool conflict = item.action == sscl::core::ModPlanAction::kConflict;
          row.checked = row.enabled &&
                        ((!installed && !conflict) || item.role == sscl::core::ModPlanRole::kUser);
        if (item.role == sscl::core::ModPlanRole::kRequired) {
            row.tag = L"前置";
        }
        if (installed) {
            row.tag = row.tag.empty() ? L"已安装" : row.tag + L" · 已安装";
        } else if (item.action == sscl::core::ModPlanAction::kConflict) {
            row.tag = row.tag.empty() ? L"同名文件" : row.tag + L" · 同名";
        } else if (!row.enabled) {
            row.tag = L"不可用";
        }
        m_dep_rows.push_back(std::move(row));
    }

    m_dep_title->text = L"确认安装到 " + widen(m_dep_target_id);
    m_dep_all_checked = true;
    update_dependency_summary();
    m_dep_scroll.jump_to(0.0f);

    // 弹层互斥：收起详情（弹层画在列表之上，关闭后回到附加页列表 —— D-099）与旧确认层。
    m_detail_open = false;
    m_detail_loading = false;
    ++m_detail_generation;
    if (m_detail_card != nullptr) {
        m_detail_card->visible = false;
    }
    m_confirm_open = false;
    if (m_confirm_card != nullptr) {
        m_confirm_card->visible = false;
    }
    m_dep_closing = false;
    m_dep_open = true;
    m_dep_card->visible = true;
    motion::modal().open();
    layout_dependency_dialog();
    if (request_repaint) {
        request_repaint();
    }
}

void AddonsPage::update_dependency_summary() {
    std::size_t selected = 0;
    std::size_t enabled = 0;
    bool all = true;
    for (const DepRow& row : m_dep_rows) {
        if (row.checked) {
            ++selected;
        }
        if (row.enabled) {
            ++enabled;
            if (!row.checked) {
                all = false;
            }
        }
    }
    if (enabled == 0) {
        all = false;
    }
    m_dep_all_checked = all;
    if (m_dep_all != nullptr) {
        m_dep_all->text = all ? L"全不选" : L"全选";
    }
    if (m_dep_start != nullptr) {
        m_dep_start->enabled = selected > 0;
    }
    if (m_dep_hint != nullptr) {
        std::wstring hint = L"共 " + std::to_wstring(m_dep_rows.size()) + L" 项，已选 " +
                            std::to_wstring(selected) + L" 项";
        if (!m_dep_warnings.empty()) {
            hint += L"；未解析前置：" + m_dep_warnings;
        }
        if (m_plan.has_conflicts()) {
            hint += L"；同名文件默认不勾选（勾选即覆盖）";
        }
        if (m_dep_rows.empty()) {
            hint = L"没有可安装项。";
        }
        m_dep_hint->text = hint;
    }
}

void AddonsPage::toggle_dependency(std::size_t index) {
    if (index >= m_dep_rows.size() || !m_dep_rows[index].enabled) {
        return;
    }
    m_dep_rows[index].checked = !m_dep_rows[index].checked;
    update_dependency_summary();
    if (request_repaint) {
        request_repaint();
    }
}

void AddonsPage::set_all_dependencies(bool checked) {
    for (DepRow& row : m_dep_rows) {
        if (row.enabled) {
            row.checked = checked;
        }
    }
    update_dependency_summary();
    if (request_repaint) {
        request_repaint();
    }
}

int AddonsPage::dependency_row_hit(float x_dip, float y_dip) const {
    if (!m_dep_open || m_dep_rows.empty()) {
        return -1;
    }
    if (x_dip < m_dep_list_rect.left || x_dip > m_dep_list_rect.right ||
        y_dip < m_dep_list_rect.top || y_dip > m_dep_list_rect.bottom) {
        return -1;
    }
    const float local = y_dip - m_dep_list_rect.top + m_dep_scroll.offset();
    if (local < 0.0f) {
        return -1;
    }
    const std::size_t index = static_cast<std::size_t>(local / kDepRowPitch);
    if (index >= m_dep_rows.size()) {
        return -1;
    }
    return static_cast<int>(index);
}

void AddonsPage::close_dependency_dialog() {
    if (!m_dep_open) {
        return;
    }
    m_dep_open = false;
    m_dep_closing = true;
    m_dep_close_ms = 0.0f;
    motion::modal().close();
    if (request_repaint) {
        request_repaint();
    }
}

void AddonsPage::start_dependency_install() {
    if (!m_dep_open) {
        return;  // 防止按钮重复触发（弹层已关闭时不再提交）
    }
    sscl::core::ModInstallPlan subset;
    for (const DepRow& row : m_dep_rows) {
        if (!row.checked || !row.enabled || row.plan_index >= m_plan.items.size()) {
            continue;
        }
        subset.items.push_back(m_plan.items[row.plan_index]);
    }
    if (subset.items.empty()) {
        set_status(L"没有勾选任何项。");
        return;
    }
    for (const sscl::core::ModPlanItem& item : subset.items) {
        switch (item.action) {
            case sscl::core::ModPlanAction::kInstall:
                ++subset.install_count;
                break;
            case sscl::core::ModPlanAction::kSkipSame:
                ++subset.skip_count;
                break;
            case sscl::core::ModPlanAction::kConflict:
                ++subset.conflict_count;
                break;
            case sscl::core::ModPlanAction::kUnavailable:
                ++subset.unavailable_count;
                break;
        }
    }
    if (subset.install_count == 0) {
        close_dependency_dialog();
        set_status(L"所选项目均已在目标实例中，无需下载。");
        return;
    }
    const std::string target_id = m_dep_target_id;
    close_dependency_dialog();
    submit_mod_install(subset, target_id);
    const std::wstring message =
        L"已加入后台任务：" + std::to_wstring(subset.install_count) +
        L" 个文件；可在「下载」页或右下角悬浮窗查看进度。";
    if (m_detail_message != nullptr) {
        m_detail_message->text = message;
    }
    // D-099：后台任务提示 6 秒后自动恢复默认文案。
    set_status(message, 6000.0f);
    if (request_repaint) {
        request_repaint();
    }
}

void AddonsPage::submit_mod_install(const sscl::core::ModInstallPlan& plan,
                                    const std::string& target_id) {
    if (plan.items.empty() || m_service == nullptr) {
        return;
    }
    std::shared_ptr<sscl::core::GameService> service = m_service;
    sscl::core::ModInstallOptions options;
    options.overwrite_conflicts = plan.has_conflicts();
    const std::wstring title = L"安装 Mod 到 " + widen(target_id);
    const std::string subtitle =
        plan.items.size() > 1
            ? std::to_string(plan.items.size()) + " 个文件"
            : (plan.items.front().file_name.empty() ? plan.items.front().project.id
                                                    : plan.items.front().file_name);
    sscl::core::TaskCenter::instance().submit(
        sscl::util::wide_to_utf8(title), subtitle,
        [service, plan, options](sscl::core::JobContext& context) -> sscl::util::Result<void> {
            sscl::core::ModHashCache cache(::sscl::platform::data_dir() / L"cache" /
                                           L"mod_hashes.json");
            const sscl::core::ModInstallProgressFn progress =
                [&context](const sscl::core::ModInstallProgress& current) {
                    sscl::core::JobProgress job;
                    job.done_files = current.completed_files;
                    job.total_files = current.total_files;
                    job.done_bytes = current.total_done;
                    job.total_bytes = current.total_total;
                    job.current_done = current.current_done;
                    job.current_total = current.current_total;
                    job.current_file = current.current_file;
                    job.phase = "下载并安装 Mod";
                    context.report(job);
                };
            return sscl::core::install_mod_plan(
                plan, options, service->scheduler(), cache,
                ::sscl::platform::data_dir() / L"cache" / L"mod_tmp", progress,
                context.stop_token());
        });
}

// ————————————————————————— 命中 —————————————————————————

bool AddonsPage::list_viewport_hit(float x_dip, float y_dip) const {
    const float y = content_y(y_dip);
    return x_dip >= m_results_rect.left + 12.0f && x_dip < m_results_rect.right - 12.0f &&
           y >= m_results_rect.top + 12.0f && y < m_results_rect.bottom - 12.0f;
}

bool AddonsPage::row_hit(float x_dip, float y_dip, std::size_t* index) const {
    if (!list_viewport_hit(x_dip, y_dip) || m_results.empty()) {
        return false;
    }
    const float y = m_results_rect.top + 12.0f;
    const float relative = content_y(y_dip) - y;
    const std::size_t row = static_cast<std::size_t>((std::max)(0.0f, relative / kRowHeight));
    if (row >= m_results.size() || relative < static_cast<float>(row) * kRowHeight ||
        relative >= static_cast<float>(row) * kRowHeight + kRowHeight - 4.0f) {
        return false;
    }
    *index = row;
    return true;
}

bool AddonsPage::local_row_hit(float x_dip, float y_dip, std::size_t* index) const {
    if (!list_viewport_hit(x_dip, y_dip) || m_local_items.empty()) {
        return false;
    }
    const float y = m_results_rect.top + 12.0f;
    const float relative = content_y(y_dip) - y;
    const std::size_t row = static_cast<std::size_t>((std::max)(0.0f, relative / kRowHeight));
    if (row >= m_local_items.size() || relative < static_cast<float>(row) * kRowHeight ||
        relative >= static_cast<float>(row) * kRowHeight + kRowHeight - 4.0f) {
        return false;
    }
    *index = row;
    return true;
}

int AddonsPage::filter_hit(float x_dip, float y_dip) const {
    const float y = content_y(y_dip);
    if (m_mode == Mode::kLocal) {
        return in_rect(m_btn_local, x_dip, y) ? 9 : 0;
    }
    if (in_rect(m_name_rect, x_dip, y)) return 1;
    if (in_rect(m_source_rect, x_dip, y)) return 2;
    if (in_rect(m_version_rect, x_dip, y)) return 3;
    if (in_rect(m_loader_rect, x_dip, y)) return 4;
    if (in_rect(m_tag_rect, x_dip, y)) return 5;
    if (in_rect(m_btn_search, x_dip, y)) return 6;
    if (in_rect(m_btn_reset, x_dip, y)) return 7;
    if (in_rect(m_btn_ranking, x_dip, y)) return 8;
    if (in_rect(m_btn_local, x_dip, y)) return 9;
    return 0;
}

int AddonsPage::pager_hit(float x_dip, float y_dip) const {
    const float y = content_y(y_dip);
    if (in_rect(m_pager_prev, x_dip, y)) return 1;
    if (in_rect(m_pager_next, x_dip, y)) return 2;
    return 0;
}

// ————————————————————————— 绘制 —————————————————————————

void AddonsPage::draw_filter_card(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    fill_round(target, m_card_rect, 8.0f, theme_surface());
    stroke_round(target, m_card_rect, 8.0f, theme_border());
    if (m_mode == Mode::kLocal) {
        draw_label(target, dwrite, L"本地 Mod 管理",
                   rect_of(m_card_rect.left + 25.0f, m_card_rect.top + 12.0f,
                           m_card_rect.right - 25.0f, m_card_rect.top + 38.0f),
                   13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, theme_text_primary());
        draw_label(target, dwrite, L"选中一行后可启用/禁用、删除、导入或检查更新。",
                   rect_of(m_card_rect.left + 25.0f, m_card_rect.top + 44.0f,
                           m_card_rect.right - 160.0f, m_card_rect.top + 70.0f),
                   12.0f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_secondary());
        fill_round(target, m_btn_local, 6.0f, theme_surface());
        stroke_round(target, m_btn_local, 6.0f, theme_border());
        draw_label(target, dwrite, L"← 返回搜索", m_btn_local, 12.0f,
                   DWRITE_FONT_WEIGHT_NORMAL, theme_text_primary(),
                   DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    draw_label(target, dwrite, L"搜索 Mod",
               rect_of(m_card_rect.left + 25.0f, m_card_rect.top + 12.0f,
                       m_card_rect.right - 25.0f, m_card_rect.top + 38.0f),
               13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, theme_text_primary());

    const float label_left = m_card_rect.left + 25.0f;
    auto field_label = [&](const wchar_t* text, float top) {
        draw_label(target, dwrite, text, rect_of(label_left, top, label_left + 46.0f, top + 32.0f),
                   12.0f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_secondary());
    };
    field_label(L"名称", m_name_rect.top);
    field_label(L"版本", m_version_rect.top);
    draw_label(target, dwrite, L"来源",
               rect_of(m_source_rect.left - 52.0f, m_source_rect.top,
                       m_source_rect.left - 6.0f, m_source_rect.bottom),
               12.0f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_secondary());
    draw_label(target, dwrite, L"类型",
               rect_of(m_tag_rect.left - 52.0f, m_tag_rect.top,
                       m_tag_rect.left - 6.0f, m_tag_rect.bottom),
               12.0f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_secondary());

    // 输入框
    fill_round(target, m_name_rect, 6.0f, theme_container());
    stroke_round(target, m_name_rect, 6.0f,
                 m_search.is_focused() ? theme_focus() : theme_border());
    m_search.draw(target, dwrite);
    fill_round(target, m_version_rect, 6.0f, theme_container());
    stroke_round(target, m_version_rect, 6.0f,
                 m_version.is_focused() ? theme_focus() : theme_border());
    m_version.draw(target, dwrite);
    draw_label(target, dwrite, L"▾",
               rect_of(m_version_rect.right - 24.0f, m_version_rect.top,
                       m_version_rect.right - 6.0f, m_version_rect.bottom),
               11.0f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_secondary(),
               DWRITE_TEXT_ALIGNMENT_CENTER);

    // 下拉框（来源 / 加载器 / 类型）
    auto combo = [&](const D2D1_RECT_F& rect, const std::wstring& text, bool active) {
        const auto st = motion::button_colors(false, true, rect);
        const D2D1_COLOR_F base = active ? theme_primary_container() : theme_container();
        fill_round(target, rect, 6.0f,
                   motion::blend(base, theme_hover_background(), st.hover));
        stroke_round(target, rect, 6.0f, active ? theme_primary() : st.border);
        draw_label(target, dwrite, text,
                   rect_of(rect.left + 10.0f, rect.top, rect.right - 24.0f, rect.bottom),
                   12.0f, DWRITE_FONT_WEIGHT_NORMAL, st.text);
        draw_label(target, dwrite, L"▾",
                   rect_of(rect.right - 24.0f, rect.top, rect.right - 6.0f, rect.bottom),
                   11.0f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_secondary(),
                   DWRITE_TEXT_ALIGNMENT_CENTER);
    };
    combo(m_source_rect, m_source_text, m_dropdown == Dropdown::kSource);
    combo(m_loader_rect, m_loader_text, m_dropdown == Dropdown::kLoader);
    combo(m_tag_rect, m_tag_text, m_dropdown == Dropdown::kTag);

    // 按钮
    auto button = [&](const D2D1_RECT_F& rect, const wchar_t* text, bool primary) {
        const auto st = motion::button_colors(primary, true, rect);
        fill_round(target, rect, 6.0f, st.fill);
        stroke_round(target, rect, 6.0f, primary ? theme_primary() : st.border);
        draw_label(target, dwrite, text, rect, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, st.text,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
    };
    button(m_btn_search, L"搜索", true);
    button(m_btn_reset, L"重置条件", false);
    button(m_btn_ranking, L"下载排行", false);
    button(m_btn_local, m_mode == Mode::kLocal ? L"← 返回搜索" : L"本地 Mod", false);
}

void AddonsPage::draw_dropdown(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (m_dropdown == Dropdown::kNone) {
        return;
    }
    fill_round(target, m_dropdown_rect, 8.0f, theme_elevated_surface());
    stroke_round(target, m_dropdown_rect, 8.0f, theme_border());
    target->PushAxisAlignedClip(m_dropdown_rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (std::size_t i = 0; i < m_dropdown_items.size(); ++i) {
        const D2D1_RECT_F& item = m_dropdown_items[i];
        if (item.bottom < m_dropdown_rect.top || item.top > m_dropdown_rect.bottom) {
            continue;
        }
        motion::draw_hot_overlay(target, item, 5.0f, theme_hover_background(),
                             theme_primary_container());
        draw_label(target, dwrite, m_dropdown_labels[i],
                   rect_of(item.left + 10.0f, item.top, item.right - 10.0f, item.bottom),
                   12.0f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_primary());
    }
    target->PopAxisAlignedClip();
}

void AddonsPage::draw_hint(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (m_warning.empty()) {
        return;
    }
    const D2D1_RECT_F hint = rect_of(kPageMargin, m_card_rect.bottom + 8.0f,
                                     m_width_dip - kPageMargin, m_card_rect.bottom + 38.0f);
    fill_round(target, hint, 6.0f, theme_warning());
    draw_label(target, dwrite, m_warning,
               rect_of(hint.left + 12.0f, hint.top, hint.right - 12.0f, hint.bottom),
               11.0f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_primary());
}

void AddonsPage::draw_pager(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (m_mode == Mode::kLocal) {
        return;
    }
    auto pager_button = [&](const D2D1_RECT_F& rect, const wchar_t* text, bool enabled) {
        fill_round(target, rect, 5.0f, theme_surface());
        stroke_round(target, rect, 5.0f, theme_border());
        draw_label(target, dwrite, text, rect, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
                   enabled ? theme_text_primary() : theme_text_disabled(),
                   DWRITE_TEXT_ALIGNMENT_CENTER);
    };
    pager_button(m_pager_prev, L"上一页", m_page > 0);
    pager_button(m_pager_next, L"下一页", m_has_more);
    draw_label(target, dwrite, L"第 " + std::to_wstring(m_page + 1) + L" 页",
               rect_of(m_pager_prev.right, m_pager_prev.top, m_pager_next.left,
                       m_pager_next.bottom),
               11.0f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_secondary(),
               DWRITE_TEXT_ALIGNMENT_CENTER);
}

void AddonsPage::draw_status(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    const std::wstring text = m_local_loading ? m_local_status
                                              : (m_installing ? m_install_message : m_status);
    draw_label(target, dwrite, text,
               rect_of(kPageMargin, m_height_dip - 44.0f, m_width_dip * 0.5f - 130.0f,
                       m_height_dip - 16.0f),
               10.5f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_muted());
}

void AddonsPage::draw_row(ID2D1RenderTarget* target, IDWriteFactory* dwrite, std::size_t index,
                          float y) {
    if (index >= m_results.size()) {
        return;
    }
    const auto& project = m_results[index];
    const float left = m_results_rect.left + 12.0f;
    const float right = m_results_rect.right - 12.0f;
    const D2D1_RECT_F row = rect_of(left, y, right, y + kRowHeight - 2.0f);
    motion::draw_hot_overlay(target, row, 6.0f, theme_hover_background(), theme_primary_container());

    // Logo 50x50 圆角 6（PCL MyResourceItem）
    const D2D1_RECT_F logo = rect_of(left + 7.0f, y + 6.0f, left + 7.0f + kLogoSize,
                                    y + 6.0f + kLogoSize);
    RemoteImage::instance().request(project.icon_url, [this]() {
        if (request_repaint) {
            request_repaint();
        }
    });
    const std::wstring title = widen(project.title.empty() ? project.slug : project.title);
    draw_icon(target, dwrite, project.icon_url, title, logo);

    const float text_left = logo.right + 8.0f;
    const float title_right = right - 12.0f;
    // 标题 + 灰字原名（PCL LabTitle / LabTitleRaw）
    const float title_width = measure_label(dwrite, title, 14.0f);
    draw_label(target, dwrite, title,
               rect_of(text_left, y + 4.0f, title_right, y + 24.0f),
               14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, theme_text_primary());
    if (!project.slug.empty() && widen(project.slug) != title) {
        const float sub_left = (std::min)(text_left + title_width + 8.0f, right - 160.0f);
        draw_label(target, dwrite, L"| " + widen(project.slug),
                   rect_of(sub_left, y + 5.0f, title_right, y + 23.0f),
                   12.0f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_muted());
    }

    // Tags + 描述（PCL PanTags / LabInfo）
    float desc_left = text_left;
    const auto tags = project_tags(project);
    for (const auto& tag : tags) {
        const float width = measure_label(dwrite, tag, 10.0f) + 12.0f;
        const D2D1_RECT_F pill = rect_of(desc_left, y + 25.0f, desc_left + width, y + 43.0f);
        fill_round(target, pill, 3.0f, theme_primary_container());
        draw_label(target, dwrite, tag, pill, 10.0f, DWRITE_FONT_WEIGHT_NORMAL,
                   theme_text_secondary(), DWRITE_TEXT_ALIGNMENT_CENTER);
        desc_left += width + 6.0f;
    }
    draw_label(target, dwrite, widen(project.summary),
               rect_of(desc_left + 4.0f, y + 24.0f, title_right, y + 44.0f),
               12.0f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_secondary());

    // 底部信息栏（PCL：版本/加载器 · 下载量 · 更新时间 · 平台）
    const float bottom = y + 44.0f;
    const float col1 = text_left;
    const float col2 = right - 360.0f;
    const float col3 = right - 230.0f;
    const float col4 = right - 120.0f;
    draw_label(target, dwrite, L"⊙ " + loader_desc(project),
               rect_of(col1, bottom, col2 - 8.0f, y + 62.0f),
               11.0f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_muted());
    draw_label(target, dwrite, L"下载 " + format_downloads(project.downloads),
               rect_of(col2, bottom, col3 - 8.0f, y + 62.0f),
               11.0f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_muted());
    draw_label(target, dwrite, L"更新 " + relative_time(project.updated_at),
               rect_of(col3, bottom, col4 - 8.0f, y + 62.0f),
               11.0f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_muted());
    draw_label(target, dwrite, widen(sscl::core::mod_platform_key(project.platform)),
               rect_of(col4, bottom, right - 12.0f, y + 62.0f),
               11.0f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_muted(),
               DWRITE_TEXT_ALIGNMENT_TRAILING);
}

void AddonsPage::draw_local_row(ID2D1RenderTarget* target, IDWriteFactory* dwrite,
                                std::size_t index, float y) {
    if (index >= m_local_items.size()) {
        return;
    }
    const auto& item = m_local_items[index];
    const float left = m_results_rect.left + 12.0f;
    const float right = m_results_rect.right - 12.0f;
    const D2D1_RECT_F row = rect_of(left, y, right, y + kRowHeight - 2.0f);
    D2D1_COLOR_F fill = theme_background();
    if (index == m_hover_row) fill = theme_hover_background();
    if (index == m_local_selected) fill = theme_primary_container();
    fill_round(target, row, 6.0f, fill);
    const std::wstring title = widen(item.local.file_name);
    draw_label(target, dwrite, title,
               rect_of(left + 16.0f, y + 8.0f, right - 240.0f, y + 30.0f),
               14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, theme_text_primary());
    std::wstring meta;
    if (!item.project.title.empty()) meta = widen(item.project.title);
    if (!item.current.version_number.empty()) {
        if (!meta.empty()) meta += L" · ";
        meta += L"v" + widen(item.current.version_number);
    }
    if (!item.latest.version_number.empty() && item.has_update) {
        meta += L" → v" + widen(item.latest.version_number);
    }
    draw_label(target, dwrite, meta,
               rect_of(left + 16.0f, y + 32.0f, right - 240.0f, y + 56.0f),
               11.5f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_secondary());
    std::wstring state = item.local.enabled ? L"已启用" : L"已禁用";
    if (item.has_update) state += L" · 可更新";
    draw_label(target, dwrite, state,
               rect_of(right - 230.0f, y + 20.0f, right - 16.0f, y + 44.0f),
               11.5f, DWRITE_FONT_WEIGHT_NORMAL,
               item.has_update ? theme_text_primary() : theme_text_muted(),
               DWRITE_TEXT_ALIGNMENT_TRAILING);
}

void AddonsPage::draw_dependency_dialog(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (target == nullptr || dwrite == nullptr || m_dep_card == nullptr) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dim;
    if (SUCCEEDED(target->CreateSolidColorBrush(
            D2D1::ColorF(0.0f, 0.0f, 0.0f, motion::modal().dim()), dim.GetAddressOf()))) {
        target->FillRectangle(rect_of(0.0f, 0.0f, m_width_dip, m_height_dip), dim.Get());
    }
    // 卡片与自绘列表行一起进同一个弹层（缩放/淡入/位移一致）。
    const bool layered = motion::push_pop(target, motion::modal(), m_dep_card->bounds);
    m_dep_card->draw(target, dwrite);
    draw_dependency_rows(target, dwrite);
    if (layered) {
        motion::pop_pop(target);
    }
}

void AddonsPage::draw_dependency_rows(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (target == nullptr || dwrite == nullptr || m_dep_rows.empty()) {
        return;
    }
    const D2D1_RECT_F list = m_dep_list_rect;
    const float offset = m_dep_scroll.offset();
    target->PushAxisAlignedClip(list, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (std::size_t i = 0; i < m_dep_rows.size(); ++i) {
        const float top = list.top + static_cast<float>(i) * kDepRowPitch - offset;
        if (top + kDepRowPitch < list.top - 1.0f || top > list.bottom + 1.0f) {
            continue;
        }
        const DepRow& row = m_dep_rows[i];
        const D2D1_RECT_F row_rect =
            rect_of(list.left + 4.0f, top + 2.0f, list.right - 12.0f, top + kDepRowPitch - 4.0f);
        if (row.enabled) {
            motion::draw_hot_overlay(target, row_rect, 6.0f, theme_hover_background(),
                                     theme_primary_container());
        }

        // 复选框：勾选 = 实心主色 + 勾；未选 = 1px 描边；不可用 = 更淡描边。
        const bool on = row.checked && row.enabled;
        const float box_top = top + (kDepRowPitch - 18.0f) * 0.5f;
        const D2D1_RECT_F box =
            rect_of(row_rect.left + 12.0f, box_top, row_rect.left + 30.0f, box_top + 18.0f);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> mark;
        const D2D1_COLOR_F mark_color =
            on ? theme_primary() : (row.enabled ? theme_border() : theme_text_disabled());
        if (SUCCEEDED(target->CreateSolidColorBrush(mark_color, mark.GetAddressOf()))) {
            const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(box, 4.0f, 4.0f);
            if (on) {
                target->FillRoundedRectangle(rounded, mark.Get());
            } else {
                target->DrawRoundedRectangle(rounded, mark.Get(), 1.0f);
            }
        }
        if (on) {
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> tick;
            if (SUCCEEDED(target->CreateSolidColorBrush(theme_text_on_primary(),
                                                        tick.GetAddressOf()))) {
                target->DrawLine(D2D1::Point2F(box.left + 4.5f, box.top + 9.0f),
                                 D2D1::Point2F(box.left + 8.0f, box.top + 12.5f), tick.Get(),
                                 1.6f);
                target->DrawLine(D2D1::Point2F(box.left + 8.0f, box.top + 12.5f),
                                 D2D1::Point2F(box.left + 13.5f, box.top + 5.5f), tick.Get(),
                                 1.6f);
            }
        }

        const float text_left = row_rect.left + 42.0f;
        const float text_right = row_rect.right - 150.0f;
        draw_label(target, dwrite, row.title, rect_of(text_left, top + 5.0f, text_right,
                                                      top + 25.0f),
                   12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                   row.enabled ? theme_text_primary() : theme_text_disabled());
        std::wstring meta = row.version;
        if (row.size > 0) {
            meta += (meta.empty() ? L"" : L"  ·  ") + format_size(row.size);
        }
        draw_label(target, dwrite, meta,
                   rect_of(text_left, top + 24.0f, text_right, top + 42.0f), 10.5f,
                   DWRITE_FONT_WEIGHT_NORMAL, theme_text_muted());

        if (!row.tag.empty()) {
            const D2D1_COLOR_F tag_color =
                row.enabled ? (row.checked ? theme_text_secondary() : theme_warning())
                            : theme_text_muted();
            draw_label(target, dwrite, row.tag,
                       rect_of(row_rect.right - 148.0f, top + 11.0f, row_rect.right - 12.0f,
                               top + 33.0f),
                       10.5f, DWRITE_FONT_WEIGHT_NORMAL, tag_color,
                       DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
    }
    target->PopAxisAlignedClip();
    m_dep_scroll.draw(target);
}

void AddonsPage::draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (target == nullptr) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> background;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_background(),
                                                background.GetAddressOf()))) {
        target->FillRectangle(rect_of(0.0f, 0.0f, m_width_dip, m_height_dip),
                              background.Get());
    }

    // D-099：确认/详情/依赖三类弹层统一画在页面内容之后（见 draw() 末尾），
    // 不再用顶部 early-return —— 否则淡出期间背后没有页面，观感就是「整页黑屏」。

    // —— 整页滚动内容（D-096）——
    // 筛选卡片属于内容流的最上方：随页面一起滚动，滚出后由导航条区域裁剪。
    // 绘制统一用「内容坐标」，通过一次平移把滚动偏移应用到整块内容。
    const float scroll_offset = m_scroll.offset();
    target->PushAxisAlignedClip(rect_of(0.0f, kNavHeightDip, m_width_dip, m_height_dip),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (scroll_offset > 0.5f) {
        target->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, -scroll_offset));
    }

    draw_filter_card(target, dwrite);
    draw_hint(target, dwrite);
    if (m_mode == Mode::kLocal) {
        m_sidebar.draw(target, dwrite);
    }

    // 结果卡片（PCL CardProjects）：高度由行数撑开，不参与内部滚动。
    fill_round(target, m_results_rect, 8.0f, theme_surface());
    stroke_round(target, m_results_rect, 8.0f, theme_border());
    const D2D1_RECT_F clip = rect_of(m_results_rect.left + 4.0f, m_results_rect.top + 4.0f,
                                     m_results_rect.right - 4.0f, m_results_rect.bottom - 4.0f);
    target->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const std::size_t count = m_mode == Mode::kLocal ? m_local_items.size() : m_results.size();
    for (std::size_t i = 0; i < count; ++i) {
        const float y = m_results_rect.top + 12.0f + static_cast<float>(i) * kRowHeight;
        // 整页滚动后卡片很高：按可见视口裁剪，避免每帧绘制全部 40 行。
        if (y + kRowHeight < kNavHeightDip + scroll_offset ||
            y > m_height_dip + scroll_offset) {
            continue;
        }
        if (y + kRowHeight < clip.top || y > clip.bottom) {
            continue;
        }
        if (m_mode == Mode::kLocal) {
            draw_local_row(target, dwrite, i, y);
        } else {
            draw_row(target, dwrite, i, y);
        }
    }
    target->PopAxisAlignedClip();
    draw_pager(target, dwrite);

    // D-102：忙碌文案与转圈由外壳统一绘制（居中指示器），此处不再重复。
    motion::menu().set_open(m_dropdown != Dropdown::kNone, 3);
    if (m_dropdown != Dropdown::kNone || motion::menu().visible_for(3)) {
        const bool menu_layer = motion::push_fade(target, motion::menu().opacity(),
                                                  motion::menu().offset_y());
        draw_dropdown(target, dwrite);
        if (menu_layer) {
            motion::pop_layer(target);
        }
    }

    // 复位内容平移，滚动条与底部状态条固定在屏幕坐标系。
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    target->PopAxisAlignedClip();
    m_scroll.draw(target);
    draw_status(target, dwrite);

    // —— 弹层（D-099：统一叠在页面内容之后，淡出期间背后仍是完整页面）——
    if (m_confirm_open || (m_confirm_closing && motion::modal().visible())) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dim;
        if (SUCCEEDED(target->CreateSolidColorBrush(
                D2D1::ColorF(0.0f, 0.0f, 0.0f, motion::modal().dim()), dim.GetAddressOf()))) {
            target->FillRectangle(rect_of(0.0f, 0.0f, m_width_dip, m_height_dip), dim.Get());
        }
        const bool layered = motion::push_pop(target, motion::modal(), m_confirm_card->bounds);
        m_confirm_card->draw(target, dwrite);
        if (layered) {
            motion::pop_pop(target);
        }
    } else if ((m_detail_open || motion::modal().visible()) && m_detail_card != nullptr &&
               m_detail_card->visible) {
        // 详情浮层：深遮罩 + 弹出动画（D-081）。
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dim;
        if (SUCCEEDED(target->CreateSolidColorBrush(
                D2D1::ColorF(0.0f, 0.0f, 0.0f, motion::modal().dim()), dim.GetAddressOf()))) {
            target->FillRectangle(rect_of(0.0f, 0.0f, m_width_dip, m_height_dip), dim.Get());
        }
        const bool layered = motion::push_pop(target, motion::modal(), m_detail_card->bounds);
        m_detail_card->draw(target, dwrite);
        draw_detail_extras(target, dwrite);
        if (layered) {
            motion::pop_pop(target);
        }
    }
    // 依赖勾选弹层最上层（打开时详情/确认都已收起）。
    if (m_dep_open || m_dep_closing) {
        draw_dependency_dialog(target, dwrite);
    }
}

// ————————————————————————— 事件 —————————————————————————

bool AddonsPage::mouse_move(float x, float y) {
    m_dirty = false;
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;

    // D-098：依赖勾选弹层独占输入。
    if (m_dep_open) {
        m_dep_card->state_changed = false;
        m_dep_card->on_mouse_move(x_dip, y_dip);
        bool changed = m_dep_card->state_changed;
        if (m_dep_scroll.on_mouse_move(x_dip, y_dip)) {
            changed = true;
        }
        return changed;
    }
    if (m_dropdown != Dropdown::kNone) {
        int hover = -1;
        const float menu_y = content_y(y_dip);
        for (std::size_t i = 0; i < m_dropdown_items.size(); ++i) {
            if (in_rect(m_dropdown_items[i], x_dip, menu_y) &&
                in_rect(m_dropdown_rect, x_dip, menu_y)) {
                hover = static_cast<int>(i);
                break;
            }
        }
        if (hover != m_dropdown_hover) {
            m_dropdown_hover = hover;
            m_dirty = true;
        }
        return m_dirty;
    }
    if (m_confirm_open) {
        m_confirm_card->state_changed = false;
        m_confirm_card->on_mouse_move(x_dip, y_dip);
        return m_confirm_card->state_changed;
    }
    if (m_detail_open) {
        int hover = -1;
        for (std::size_t i = 0; i < m_version_rows.size(); ++i) {
            if (in_rect(m_version_rows[i], x_dip, y_dip)) {
                hover = static_cast<int>(i);
                break;
            }
        }
        const bool version_changed = hover != m_hover_version;
        m_hover_version = hover;
        m_detail_card->state_changed = false;
        m_detail_card->on_mouse_move(x_dip, y_dip);
        return version_changed || m_detail_card->state_changed;
    }

    // D-096：整页内容被平移绘制，`motion::hot()` 里登记的矩形是内容坐标；
    // 外壳已按屏幕坐标 pick 过一次，这里按内容坐标重取，保证滚动后悬停不错位。
    motion::hot().pick(x_dip, content_y(y_dip));

    std::size_t hover = static_cast<std::size_t>(-1);
    if (m_mode == Mode::kLocal) {
        local_row_hit(x_dip, y_dip, &hover);
    } else {
        row_hit(x_dip, y_dip, &hover);
    }
    if (hover != m_hover_row) {
        m_hover_row = hover;
        m_dirty = true;
    }
    if (m_mode == Mode::kLocal) {
        m_sidebar.state_changed = false;
        m_sidebar.on_mouse_move(x_dip, content_y(y_dip));
        if (m_sidebar.state_changed) m_dirty = true;
    }
    if (m_scroll.on_mouse_move(x_dip, y_dip)) {
        m_dirty = true;
    }
    if (m_dirty && m_window != nullptr) {
        m_window->start_frame_loop();
    }
    return m_dirty;
}

bool AddonsPage::mouse_down(float x, float y) {
    m_dirty = false;
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;
    // D-098：依赖勾选弹层独占输入（点遮罩不关闭，避免误触丢掉勾选）。
    if (m_dep_open) {
        m_dep_card->state_changed = false;
        m_dep_card->on_mouse_down(x_dip, y_dip);
        if (m_dep_scroll.on_mouse_down(x_dip, y_dip)) {
            m_dirty = true;
        }
        return m_dep_card->state_changed || m_dirty;
    }
    if (m_dropdown != Dropdown::kNone) {
        return false;
    }
    if (m_confirm_open) {
        if (!m_confirm_card->hit_test(x_dip, y_dip)) {
            close_overlay();
            return true;
        }
        m_confirm_card->state_changed = false;
        m_confirm_card->on_mouse_down(x_dip, y_dip);
        return m_confirm_card->state_changed;
    }
    if (m_detail_open) {
        if (!m_detail_card->hit_test(x_dip, y_dip)) {
            close_detail();
            return true;
        }
        m_detail_card->state_changed = false;
        m_detail_card->on_mouse_down(x_dip, y_dip);
        return m_detail_card->state_changed;
    }

    const float cy = content_y(y_dip);
    const bool name_hit = in_rect(m_name_rect, x_dip, cy);
    const bool version_hit = in_rect(m_version_rect, x_dip, cy);
    m_search.set_focused(name_hit);
    m_version.set_focused(version_hit);
    if (name_hit || version_hit) m_dirty = true;
    if (m_scroll.on_mouse_down(x_dip, y_dip)) m_dirty = true;
    if (m_mode == Mode::kLocal) {
        m_sidebar.state_changed = false;
        m_sidebar.on_mouse_down(x_dip, cy);
        if (m_sidebar.state_changed) m_dirty = true;
    }
    return m_dirty;
}

bool AddonsPage::mouse_up(float x, float y) {
    m_dirty = false;
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;

    if (m_dep_open) {
        const bool was_dragging = m_dep_scroll.on_mouse_up(x_dip, y_dip);
        m_dep_card->state_changed = false;
        m_dep_card->on_mouse_up(x_dip, y_dip);
        const bool card_changed = m_dep_card->state_changed;
        if (!was_dragging) {
            const int hit = dependency_row_hit(x_dip, y_dip);
            if (hit >= 0) {
                toggle_dependency(static_cast<std::size_t>(hit));
                m_dirty = true;
            }
        }
        return card_changed || m_dirty;
    }
    if (m_dropdown != Dropdown::kNone) {
        const float menu_y = content_y(y_dip);
        if (in_rect(m_dropdown_rect, x_dip, menu_y)) {
            for (std::size_t i = 0; i < m_dropdown_items.size(); ++i) {
                if (in_rect(m_dropdown_items[i], x_dip, menu_y) &&
                    in_rect(m_dropdown_rect, x_dip, menu_y)) {
                    apply_dropdown(i);
                    return true;
                }
            }
            return true;
        }
        close_dropdown();
        return true;
    }
    if (m_confirm_open) {
        m_confirm_card->state_changed = false;
        m_confirm_card->on_mouse_up(x_dip, y_dip);
        return m_confirm_card->state_changed;
    }
    if (m_detail_open) {
        for (std::size_t i = 0; i < m_version_rows.size(); ++i) {
            if (in_rect(m_version_rows[i], x_dip, y_dip)) {
                m_detail_card->state_changed = false;
                m_detail_card->on_mouse_up(x_dip, y_dip);
                select_detail_version(i);
                return true;
            }
        }
        m_detail_card->state_changed = false;
        m_detail_card->on_mouse_up(x_dip, y_dip);
        return m_detail_card->state_changed;
    }
    if (m_scroll.on_mouse_up(x_dip, y_dip)) {
        m_dirty = true;
    }
    if (m_mode == Mode::kLocal) {
        m_sidebar.state_changed = false;
        m_sidebar.on_mouse_up(x_dip, content_y(y_dip));
        if (m_sidebar.state_changed) m_dirty = true;
    }

    const int control = filter_hit(x_dip, y_dip);
    switch (control) {
        case 2: open_dropdown(Dropdown::kSource); return true;
        case 3:
            if (x_dip >= m_version_rect.right - 26.0f) {
                open_dropdown(Dropdown::kVersion);
            } else {
                m_version.set_focused(true);
                m_search.set_focused(false);
            }
            return true;
        case 4: open_dropdown(Dropdown::kLoader); return true;
        case 5: open_dropdown(Dropdown::kTag); return true;
        case 6: start_search(); return true;
        case 7: reset_filter(); return true;
        case 8:
            reset_filter();
            return true;
        case 9:
            set_mode(m_mode == Mode::kLocal ? Mode::kSearch : Mode::kLocal);
            return true;
        default: break;
    }
    const int pager = pager_hit(x_dip, y_dip);
    if (pager == 1) {
        change_page(-1);
        return true;
    }
    if (pager == 2) {
        change_page(1);
        return true;
    }
    std::size_t index = static_cast<std::size_t>(-1);
    if (m_mode == Mode::kLocal && local_row_hit(x_dip, y_dip, &index)) {
        m_local_selected = index;
        set_status(L"已选中：" + widen(m_local_items[index].local.file_name));
        return true;
    }
    if (m_mode == Mode::kSearch && row_hit(x_dip, y_dip, &index)) {
        open_detail(index);
        return true;
    }
    return m_dirty;
}

bool AddonsPage::mouse_leave() {
    m_search.set_focused(false);
    m_version.set_focused(false);
    if (m_mode == Mode::kLocal) {
        m_sidebar.on_mouse_leave();
    }
    const bool changed = m_hover_row != static_cast<std::size_t>(-1) ||
                         m_dropdown_hover != -1 || m_hover_version != -1;
    m_hover_row = static_cast<std::size_t>(-1);
    m_dropdown_hover = -1;
    m_hover_version = -1;
    const bool dep_changed = m_dep_open && m_dep_scroll.on_mouse_leave();
    return changed || dep_changed || m_scroll.on_mouse_leave();
}

bool AddonsPage::mouse_wheel(float x, float y, float delta) {
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;
    // D-098：勾选列表自带滚动；弹层打开时滚轮只作用于它。
    if (m_dep_open) {
        if (in_rect(m_dep_list_rect, x_dip, y_dip) && m_dep_scroll.on_wheel(delta, 60.0f)) {
            if (request_repaint) {
                request_repaint();
            }
        }
        return true;
    }
    if (m_dropdown != Dropdown::kNone && in_rect(m_dropdown_rect, x_dip, content_y(y_dip))) {
        const float max_offset = (std::max)(
            0.0f, static_cast<float>(m_dropdown_labels.size()) * 28.0f + 8.0f -
                      (m_dropdown_rect.bottom - m_dropdown_rect.top));
        m_dropdown_offset =
            (std::max)(0.0f, (std::min)(max_offset, m_dropdown_offset - delta * 0.35f));
        rebuild_dropdown_items();
        if (request_repaint) {
            request_repaint();
        }
        return true;
    }
    // D-096：整页滚动 —— 只要指针在导航条以下就滚动（滚动条贴在窗口右边缘，
    // 不能再按页边距裁掉右侧，否则指针停在滚动条上滚轮会失效）。
    if (m_detail_open || m_confirm_open || y_dip < kNavHeightDip) {
        return false;
    }
    if (m_scroll.on_wheel(delta, 120.0f)) {
        if (request_repaint) {
            request_repaint();
        }
        return true;
    }
    return false;
}

bool AddonsPage::handle_char(wchar_t ch) {
    if (m_dep_open || m_detail_open || m_confirm_open || m_dropdown != Dropdown::kNone) {
        return false;
    }
    TextBox* focused = m_search.is_focused() ? &m_search
                                             : (m_version.is_focused() ? &m_version : nullptr);
    if (focused == nullptr) {
        return false;
    }
    const bool changed = focused->handle_char(ch);
    if (changed && m_window != nullptr) {
        m_window->invalidate();
    }
    return changed;
}

bool AddonsPage::handle_key_down(unsigned long key) {
    // D-098：勾选弹层支持 Esc 关闭。
    if (m_dep_open && key == 0x1B) {
        close_dependency_dialog();
        return true;
    }
    if (m_dropdown != Dropdown::kNone && key == 0x1B) {
        close_dropdown();
        return true;
    }
    if (!m_search.is_focused() && !m_version.is_focused()) {
        return false;
    }
    if (key == VK_RETURN) {
        start_search();
        return true;
    }
    TextBox* focused = m_search.is_focused() ? &m_search : &m_version;
    const bool changed = focused->handle_key_down(key);
    if (changed && m_window != nullptr) {
        m_window->invalidate();
    }
    return changed;
}

bool AddonsPage::is_busy() const {
    return m_searching || m_local_loading || m_detail_loading || m_preparing || m_installing;
}

std::wstring AddonsPage::busy_text() const {
    if (m_preparing) return L"正在解析前置依赖…";
    if (m_detail_loading) return L"正在获取 Mod 版本列表…";
    if (m_local_loading) return L"正在读取本地 Mod…";
    if (m_searching) return L"正在获取 Mod 列表…";
    if (m_installing && !m_install_message.empty()) return m_install_message;
    return L"正在处理…";
}

bool AddonsPage::tick(float dt_ms) {
    bool active = m_scroll.tick(dt_ms);
    if (m_mode == Mode::kLocal && m_sidebar.tick(dt_ms)) {
        active = true;
    }
    if (m_dep_open && m_dep_card->tick(dt_ms)) {
        active = true;
    }
    if (m_dep_open && m_dep_scroll.tick(dt_ms)) {
        active = true;
    }
    // D-098/D-099：勾选弹层淡出结束后真正隐藏卡片；即使动画状态卡住，240ms 后也强制收尾。
    if (m_dep_closing) {
        m_dep_close_ms += dt_ms;
        active = true;
        if (!motion::modal().visible() || m_dep_close_ms >= 240.0f) {
            m_dep_closing = false;
            m_dep_close_ms = 0.0f;
            if (m_dep_card != nullptr) {
                m_dep_card->visible = false;
            }
        }
    }
    if (m_detail_open && m_detail_card->tick(dt_ms)) {
        active = true;
    }
    if (m_confirm_open && m_confirm_card->tick(dt_ms)) {
        active = true;
    }
    // D-088：确认弹层淡出结束后再真正隐藏卡片，避免与详情弹层混淆。
    if (m_confirm_closing && !motion::modal().visible()) {
        m_confirm_closing = false;
        if (m_confirm_card != nullptr) {
            m_confirm_card->visible = false;
        }
        active = true;
    }
    return active;
}

}  // namespace sscl::ui
