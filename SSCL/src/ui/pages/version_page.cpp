#include "ui/pages/version_page.hpp"

#include <algorithm>
#include <cmath>
#include <thread>
#include <utility>

#include "platform/file_utils.hpp"
#include "platform/path_utils.hpp"
#include "ui/framework/anim.hpp"
#include "ui/framework/motion.hpp"
#include "ui/framework/version_icon.hpp"
#include "ui/framework/window.hpp"
#include "ui/shell/shell_layout.hpp"
#include "ui/theme/theme.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

namespace sscl::ui {

namespace {

constexpr float kRowHeight = 40.0f;
// 组件选择弹层（D-040）
constexpr float kPickerWidthDip = 640.0f;
constexpr float kPickerHeightDip = 440.0f;
constexpr float kPickerRowHeight = 34.0f;
constexpr float kPickerHeaderHeight = 132.0f;  // 标题 + 游戏版本 + 加载器分段
constexpr float kPickerFooterHeight = 92.0f;   // 提示/状态 + 按钮行

D2D1_RECT_F rect_of(float left, float top, float right, float bottom) {
    return D2D1::RectF(left, top, right, bottom);
}

// 组件选择弹层的 4 个分段按钮与版本类型图标一一对应（D-085）。
VersionIconKind picker_icon_kind(int index) {
    switch (index) {
        case 1:
            return VersionIconKind::kFabric;
        case 2:
            return VersionIconKind::kForge;
        case 3:
            return VersionIconKind::kNeoForge;
        default:
            return VersionIconKind::kVanilla;
    }
}

std::wstring widen(const std::string& text) {
    return sscl::util::utf8_to_wide(text);
}

// 版本类型中文显示（D-036：清单 type → 用户可读）。
std::wstring type_display(const std::string& type) {
    if (type == "release") {
        return L"稳定版";
    }
    if (type == "snapshot") {
        return L"快照版";
    }
    if (type == "old_beta") {
        return L"远古 Beta";
    }
    if (type == "old_alpha") {
        return L"远古 Alpha";
    }
    return widen(type);
}

}  // namespace

VersionPage::VersionPage(AppWindow* window, std::shared_ptr<sscl::core::GameService> service)
    : m_window(window), m_service(std::move(service)) {
    auto heading = std::make_unique<Label>(L"下载游戏", 22.0f, theme_text_primary(), true);
    auto subtitle = std::make_unique<Label>(L"官方源下载，失败自动回退 BMCLAPI 镜像。",
                                            12.0f, theme_text_secondary());
    auto refresh_btn = std::make_unique<Button>(L"刷新版本列表", 13.0f, true);

    m_heading = heading.get();
    m_subtitle = subtitle.get();
    m_refresh = refresh_btn.get();

    refresh_btn->on_click = [this]() { this->refresh(); };  // 避免局部名遮蔽成员函数

    m_sidebar.add(std::move(heading));
    m_sidebar.add(std::move(subtitle));

    // 版本类型分类导航（D-036）：全部 / 稳定版 / 快照版 / 远古版。
    const std::wstring category_names[kCategoryCount] = {L"全部版本", L"稳定版", L"快照版",
                                                         L"远古版"};
    for (int i = 0; i < kCategoryCount; ++i) {
        auto category = std::make_unique<Button>(category_names[i], 13.0f, false);
        category->selected = (i == 0);  // 默认「全部版本」
        category->on_click = [this, i]() { apply_category(static_cast<VersionCategory>(i)); };
        m_category_buttons[i] = category.get();
        m_sidebar.add(std::move(category));
    }

    m_sidebar.add(std::move(refresh_btn));

    // ---- 组件选择弹层（D-040：原版 / Fabric / Forge / NeoForge + 加载器版本列表）----
    auto card = std::make_unique<Card>(L"选择安装组件");
    card->elevated = true;
    card->visible = false;  // 关闭时阻断命中/绘制

    auto game = std::make_unique<Label>(L"", 12.5f, theme_text_secondary());
    auto hint = std::make_unique<Label>(L"", 12.0f, theme_text_secondary());
    auto message = std::make_unique<Label>(L"", 12.0f, theme_text_secondary());
    const std::wstring kind_names[kLoaderKindCount] = {L"原版", L"Fabric", L"Forge",
                                                       L"NeoForge"};
    m_picker_game = game.get();
    m_picker_hint = hint.get();
    m_picker_status = message.get();
    card->add(std::move(game));
    for (int i = 0; i < kLoaderKindCount; ++i) {
        auto kind_button = std::make_unique<Button>(kind_names[i], 13.0f, false);
        kind_button->selected = (i == 0);
        // D-085：每个加载器按钮用对应的版本类型图标 + 独立语义色。
        kind_button->icon = version_icon_name(picker_icon_kind(i));
        kind_button->custom_icon_color = true;
        kind_button->icon_color = version_icon_color(picker_icon_kind(i));
        kind_button->on_click = [this, i]() {
            select_loader_kind(static_cast<sscl::core::LoaderKind>(i));
        };
        m_picker_kinds[i] = kind_button.get();
        card->add(std::move(kind_button));
    }
    card->add(std::move(hint));
    card->add(std::move(message));

    auto picker_cancel = std::make_unique<Button>(L"取消", 13.0f, false);
    auto picker_start = std::make_unique<Button>(L"开始安装", 13.0f, true);
    picker_cancel->on_click = [this]() { close_picker(); };
    picker_start->on_click = [this]() { start_picker_install(); };
    m_picker_cancel = picker_cancel.get();
    m_picker_start = picker_start.get();
    m_refresh->icon = "refresh";
    picker_cancel->icon = "close";
    picker_start->icon = "download";
    card->add(std::move(picker_cancel));
    card->add(std::move(picker_start));

    m_picker_card = card.get();
    m_picker_owner = std::move(card);
}

void VersionPage::layout(UINT width, UINT height, UINT dpi) {
    m_to_dip = 96.0f / static_cast<float>(dpi == 0 ? 96 : dpi);
    m_width_dip = static_cast<float>(width) * m_to_dip;
    m_height_dip = static_cast<float>(height) * m_to_dip;
    const float h = m_height_dip;
    const float margin = kSidebarMarginDip;

    m_heading->bounds = rect_of(margin, kNavHeightDip + 20.0f, kSidebarWidthDip - margin,
                                kNavHeightDip + 56.0f);
    m_subtitle->bounds = rect_of(margin, kNavHeightDip + 60.0f, kSidebarWidthDip - margin,
                                 kNavHeightDip + 116.0f);
    // 分类导航（D-036）
    float category_y = kNavHeightDip + 132.0f;
    for (int i = 0; i < kCategoryCount; ++i) {
        m_category_buttons[i]->bounds =
            rect_of(margin, category_y, kSidebarWidthDip - margin, category_y + 36.0f);
        category_y += 44.0f;
    }
    m_refresh->bounds = rect_of(margin, h - 110.0f, kSidebarWidthDip - margin, h - 75.0f);
    layout_picker();
}

void VersionPage::layout_picker() {
    if (m_picker_card == nullptr) {
        return;
    }
    const float content_w = m_width_dip - kSidebarWidthDip;
    float w = (std::min)(kPickerWidthDip, content_w - 80.0f);
    w = (std::max)(420.0f, w);
    const float h = (std::min)(kPickerHeightDip, m_height_dip - kNavHeightDip - 80.0f);
    const float left = kSidebarWidthDip + (content_w - w) * 0.5f;
    const float top = kNavHeightDip + ((m_height_dip - kNavHeightDip) - h) * 0.5f;
    m_picker_card->bounds = rect_of(left, top, left + w, top + h);

    m_picker_game->bounds = rect_of(left + 20.0f, top + 44.0f, left + w - 20.0f, top + 70.0f);
    const float kind_y = top + 78.0f;
    const float kind_gap = 8.0f;
    const float kind_w =
        (w - 40.0f - kind_gap * static_cast<float>(kLoaderKindCount - 1)) /
        static_cast<float>(kLoaderKindCount);
    float kind_x = left + 20.0f;
    for (int i = 0; i < kLoaderKindCount; ++i) {
        m_picker_kinds[i]->bounds = rect_of(kind_x, kind_y, kind_x + kind_w, kind_y + 34.0f);
        kind_x += kind_w + kind_gap;
    }
    const float list_top = top + kPickerHeaderHeight;
    const float list_bottom = top + h - kPickerFooterHeight;
    m_picker_scroll.layout(list_top, list_bottom,
                           static_cast<float>(m_picker_entries.size()) * kPickerRowHeight,
                           left + w - 8.0f);
    m_picker_hint->bounds = rect_of(left + 20.0f, list_bottom + 8.0f, left + w - 20.0f,
                                   list_bottom + 30.0f);
    m_picker_status->bounds = rect_of(left + 20.0f, list_bottom + 30.0f, left + w - 20.0f,
                                      list_bottom + 52.0f);
    const float button_y = top + h - 44.0f;
    m_picker_start->bounds = rect_of(left + w - 132.0f, button_y, left + w - 20.0f,
                                     button_y + 32.0f);
    m_picker_cancel->bounds = rect_of(left + w - 260.0f, button_y, left + w - 144.0f,
                                      button_y + 32.0f);
}

bool VersionPage::picker_card_hit(float x_dip, float y_dip) const {
    if (m_picker_card == nullptr) {
        return false;
    }
    const D2D1_RECT_F card = m_picker_card->bounds;
    return x_dip >= card.left && x_dip < card.right && y_dip >= card.top && y_dip < card.bottom;
}

bool VersionPage::picker_list_hit(float x_dip, float y_dip) const {
    if (m_picker_card == nullptr || !m_picker_open) {
        return false;
    }
    const D2D1_RECT_F card = m_picker_card->bounds;
    const float list_top = card.top + kPickerHeaderHeight;
    const float list_bottom = card.bottom - kPickerFooterHeight;
    return x_dip >= card.left + 20.0f && x_dip < card.right - 20.0f && y_dip >= list_top &&
           y_dip < list_bottom;
}

bool VersionPage::picker_row_hit(std::size_t index, float x_dip, float y_dip) const {
    if (!picker_list_hit(x_dip, y_dip)) {
        return false;
    }
    const D2D1_RECT_F card = m_picker_card->bounds;
    const float list_top = card.top + kPickerHeaderHeight;
    const float y = list_top + static_cast<float>(index) * kPickerRowHeight -
                    m_picker_scroll.offset();
    return y_dip >= y && y_dip < y + kPickerRowHeight;
}

void VersionPage::open_picker(const sscl::core::VersionListEntry& entry) {
    ++m_picker_generation;  // 使上一游戏版本的在途清单失效（D-051）
    m_picker_entry = entry;
    m_picker_has_entry = true;
    m_picker_kind = sscl::core::LoaderKind::kNone;
    m_picker_entries.clear();
    m_picker_selected = static_cast<std::size_t>(-1);
    m_picker_hover = static_cast<std::size_t>(-1);
    m_picker_loading = false;
    m_picker_message.clear();
    m_picker_confirm = false;
    if (m_picker_start != nullptr) {
        m_picker_start->text = L"开始安装";
    }
    m_picker_game->text = L"游戏版本：" + widen(entry.id) + L"（" + type_display(entry.type) + L"）";
    for (int i = 0; i < kLoaderKindCount; ++i) {
        m_picker_kinds[i]->selected = (i == 0);
    }
    m_picker_hint->text = L"将安装原版游戏（不含加载器）。";
    m_picker_status->text = L"";
    m_picker_start->enabled = true;
    m_picker_open = true;
    m_picker_card->visible = true;
    m_picker_view_anim = 0.0f;
    layout_picker();
    if (m_window != nullptr) {
        m_window->start_frame_loop();
    }
}

void VersionPage::close_picker() {
    ++m_picker_generation;  // 关闭后在途清单不再落地（D-051）
    m_picker_open = false;
    m_picker_card->visible = false;  // 关闭即阻断命中（淡出由 m_picker_anim 负责）
    if (m_window != nullptr) {
        m_window->start_frame_loop();
    }
}

void VersionPage::select_loader_kind(sscl::core::LoaderKind kind) {
    ++m_picker_generation;  // 切换加载器类型使旧类型结果失效（D-051）
    m_picker_kind = kind;
    m_picker_confirm = false;
    if (m_picker_start != nullptr) {
        m_picker_start->text = L"开始安装";
    }
    for (int i = 0; i < kLoaderKindCount; ++i) {
        m_picker_kinds[i]->selected = (static_cast<int>(kind) == i);
    }
    m_picker_entries.clear();
    m_picker_selected = static_cast<std::size_t>(-1);
    m_picker_hover = static_cast<std::size_t>(-1);
    m_picker_view_anim = 0.0f;
    m_picker_message.clear();
    m_picker_start->enabled = false;
    switch (kind) {
        case sscl::core::LoaderKind::kNone:
            m_picker_hint->text = L"将安装原版游戏（不含加载器）。";
            m_picker_start->enabled = true;
            break;
        case sscl::core::LoaderKind::kFabric:
            m_picker_hint->text = L"Fabric 只需下载配置与依赖库，不会运行任何安装程序。";
            break;
        case sscl::core::LoaderKind::kForge:
        case sscl::core::LoaderKind::kNeoForge:
            m_picker_hint->text =
                L"将下载官方安装器，并用本机 Java 运行其处理器（第三方代码，需 Java 17+）。";
            break;
    }
    refresh_loader_versions();
    layout_picker();
    if (m_window != nullptr) {
        m_window->start_frame_loop();
    }
}

void VersionPage::refresh_loader_versions() {
    m_picker_entries.clear();
    m_picker_selected = static_cast<std::size_t>(-1);
    if (m_picker_kind == sscl::core::LoaderKind::kNone || !m_picker_has_entry) {
        m_picker_loading = false;
        m_picker_status->text = L"";
        layout_picker();
        return;
    }
    m_picker_loading = true;
    m_picker_status->text = L"正在获取加载器版本列表…";
    layout_picker();

    std::shared_ptr<VersionPage> self = shared_from_this();
    const sscl::core::LoaderKind kind = m_picker_kind;
    const std::string mc = m_picker_entry.id;
    const std::uint64_t generation = m_picker_generation;
    std::thread([self, kind, mc, generation]() {
        sscl::core::LoaderCatalogOptions options;
        options.cache_dir = sscl::platform::data_dir() / L"cache" / L"loaders";
        sscl::util::Result<std::vector<sscl::core::LoaderEntry>> result =
            sscl::core::fetch_loader_versions(kind, mc, options);
        std::vector<sscl::core::LoaderEntry> entries;
        std::wstring message;
        if (result.is_ok()) {
            entries = std::move(result.value());
        } else {
            message = L"获取失败：" + widen(result.error().message);
        }
        self->m_window->post_to_ui([self, kind, mc, generation, entries = std::move(entries),
                                    message]() mutable {
            // D-051：代际 + 加载器类型 + MC 三重校验，旧游戏版本的清单绝不覆盖当前列表
            if (self->m_picker_kind != kind || self->m_picker_generation != generation ||
                !self->m_picker_has_entry || self->m_picker_entry.id != mc) {
                return;
            }
            self->m_picker_entries = std::move(entries);
            self->m_picker_loading = false;
            self->m_picker_selected = static_cast<std::size_t>(-1);
            self->m_picker_hover = static_cast<std::size_t>(-1);
            if (!self->m_picker_entries.empty()) {
                // 默认推荐项：recommended > stable > 第一项
                std::size_t chosen = 0;
                for (std::size_t i = 0; i < self->m_picker_entries.size(); ++i) {
                    if (self->m_picker_entries[i].recommended) {
                        chosen = i;
                        break;
                    }
                }
                if (self->m_picker_entries[chosen].recommended == false) {
                    for (std::size_t i = 0; i < self->m_picker_entries.size(); ++i) {
                        if (self->m_picker_entries[i].stable) {
                            chosen = i;
                            break;
                        }
                    }
                }
                self->m_picker_selected = chosen;
                self->m_picker_start->enabled = (kind != sscl::core::LoaderKind::kNone);
            }
            self->m_picker_status->text = self->m_picker_entries.empty()
                                              ? std::wstring(L"该游戏版本没有可用的加载器版本")
                                              : std::wstring();
            self->m_picker_message = message;
            if (!message.empty()) {
                self->m_picker_status->text = message;
            }
            self->layout_picker();
            if (self->m_window != nullptr) {
                self->m_window->start_frame_loop();
            }
        });
    }).detach();
}

void VersionPage::start_picker_install() {
    if (!m_picker_has_entry) {
        return;
    }
    if (m_picker_kind == sscl::core::LoaderKind::kNone) {
        const sscl::core::VersionListEntry entry = m_picker_entry;
        close_picker();
        if (on_start_install) {
            on_start_install(entry);
        }
        return;
    }
    if (m_picker_selected >= m_picker_entries.size()) {
        m_picker_status->text = L"请先选择加载器版本";
        return;
    }
    // D-040：Forge/NeoForge 会运行安装器里的第三方 Java 代码，需二次确认
    const bool needs_confirm = (m_picker_kind == sscl::core::LoaderKind::kForge ||
                                m_picker_kind == sscl::core::LoaderKind::kNeoForge);
    if (needs_confirm && !m_picker_confirm) {
        m_picker_confirm = true;
        m_picker_hint->text =
            L"继续将下载官方安装器并运行其 Java 处理器（第三方代码，需 Java 17+）。"
            L"再次点击「确认安装」继续。";
        m_picker_start->text = L"确认安装";
        m_picker_status->text = L"";
        if (m_window != nullptr) {
            m_window->start_frame_loop();
        }
        return;
    }
    sscl::core::InstallJob job;
    job.minecraft = m_picker_entry;
    job.loader.kind = m_picker_kind;
    job.loader.entry = m_picker_entries[m_picker_selected];
    close_picker();
    if (on_start_loader_job) {
        on_start_loader_job(job);
    }
}

void VersionPage::apply_category(VersionCategory category) {
    m_category = category;
    for (int i = 0; i < kCategoryCount; ++i) {
        m_category_buttons[i]->selected = (static_cast<int>(category) == i);
    }
    rebuild_filter();
    m_scroll_view.jump_to(0.0f);
    m_hover_row = static_cast<std::size_t>(-1);
    m_filter_anim = 0.0f;  // 分类切换内容淡入（D-033/D-036）
    if (m_window != nullptr) {
        m_window->start_frame_loop();
    }
}

void VersionPage::rebuild_filter() {
    m_filtered.clear();
    m_filtered.reserve(m_entries.size());
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        const std::string& type = m_entries[i].type;
        bool match = true;
        switch (m_category) {
            case VersionCategory::kAll:
                match = true;
                break;
            case VersionCategory::kRelease:
                match = type == "release";
                break;
            case VersionCategory::kSnapshot:
                match = type == "snapshot";  // 含 pre/RC（清单均为 snapshot 类型）
                break;
            case VersionCategory::kAncient:
                match = type == "old_beta" || type == "old_alpha";
                break;
        }
        if (match) {
            m_filtered.push_back(i);
        }
    }
}

void VersionPage::draw_row(ID2D1RenderTarget* target, IDWriteFactory* dwrite, std::size_t index,
                           float y) {
    // index 为过滤列表行号（D-036）。
    const sscl::core::VersionListEntry& entry = m_entries[m_filtered[index]];
    const bool hovered = (index == m_hover_row);

    const float list_left = kSidebarWidthDip + 24.0f;
    const float list_right = m_width_dip - 24.0f;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> row_brush;
    if (FAILED(target->CreateSolidColorBrush(hovered ? theme_hover_background()
                                                     : theme_background(),
                                              row_brush.GetAddressOf()))) {
        return;
    }
    target->FillRoundedRectangle(
        D2D1::RoundedRect(rect_of(list_left, y, list_right, y + kRowHeight - 4.0f), 6.0f, 6.0f),
        row_brush.Get());

    // 版本 id + 类型
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (FAILED(dwrite->CreateTextFormat(theme_font_family(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        14.0f, L"zh-cn", format.GetAddressOf()))) {
        return;
    }
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> secondary_brush;
    target->CreateSolidColorBrush(theme_text_primary(), text_brush.GetAddressOf());
    target->CreateSolidColorBrush(theme_text_secondary(), secondary_brush.GetAddressOf());

    const std::wstring id_text = widen(entry.id);
    target->DrawTextW(id_text.c_str(), static_cast<UINT32>(id_text.size()), format.Get(),
                      rect_of(list_left + 16.0f, y, list_right - 220.0f, y + kRowHeight - 4.0f),
                      text_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                      DWRITE_MEASURING_MODE_NATURAL);

    const std::wstring type_text = type_display(entry.type);
    target->DrawTextW(type_text.c_str(), static_cast<UINT32>(type_text.size()), format.Get(),
                      rect_of(list_right - 320.0f, y, list_right - 180.0f, y + kRowHeight - 4.0f),
                      secondary_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                      DWRITE_MEASURING_MODE_NATURAL);

    // 下载按钮
    const D2D1_RECT_F button =
        rect_of(list_right - 150.0f, y + 6.0f, list_right - 58.0f, y + kRowHeight - 10.0f);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> button_brush;
    target->CreateSolidColorBrush(theme_primary(), button_brush.GetAddressOf());
    target->FillRoundedRectangle(D2D1::RoundedRect(button, 6.0f, 6.0f), button_brush.Get());

    Microsoft::WRL::ComPtr<IDWriteTextFormat> button_format;
    if (SUCCEEDED(dwrite->CreateTextFormat(theme_font_family(), nullptr,
                                           DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-cn",
                                           button_format.GetAddressOf()))) {
        button_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        button_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> on_fill;
        target->CreateSolidColorBrush(theme_text_on_primary(), on_fill.GetAddressOf());
        const std::wstring label = L"下载";
        target->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), button_format.Get(),
                          button, on_fill.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                          DWRITE_MEASURING_MODE_NATURAL);
    }
}

void VersionPage::draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (target == nullptr) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> background;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_background(), background.GetAddressOf()))) {
        target->FillRectangle(
            rect_of(0.0f, 0.0f, m_width_dip + 64.0f, m_height_dip + 64.0f), background.Get());
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> sidebar;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_sidebar_background(),
                                                sidebar.GetAddressOf()))) {
        target->FillRectangle(rect_of(0.0f, kNavHeightDip, kSidebarWidthDip, m_height_dip),
                              sidebar.Get());
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> divider;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_border(), divider.GetAddressOf()))) {
        divider->SetOpacity(0.6f);
        target->DrawLine(D2D1::Point2F(kSidebarWidthDip, kNavHeightDip),
                         D2D1::Point2F(kSidebarWidthDip, m_height_dip), divider.Get(), 1.0f);
    }
    m_sidebar.draw(target, dwrite);

    // 列表（D-022：SmoothScroll 平滑滚动 + 滚动条；D-024：overscan + 视口剪裁；
    // D-036：按分类过滤 m_filtered + 切换内容淡入）
    const float list_left = kSidebarWidthDip + 24.0f;
    const float list_right = m_width_dip - 24.0f;
    const float list_top = kNavHeightDip + 24.0f;
    const float list_bottom = m_height_dip - 24.0f;
    m_scroll_view.layout(list_top, list_bottom,
                         static_cast<float>(m_filtered.size()) * kRowHeight,
                         m_width_dip - 2.0f);

    if (m_filtered.empty()) {
        // D-102：转圈由外壳统一绘制（居中忙碌指示器）；此处只保留空态文案。
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        if (SUCCEEDED(dwrite->CreateTextFormat(theme_font_family(), nullptr,
                                               DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"zh-cn",
                                               format.GetAddressOf()))) {
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
            target->CreateSolidColorBrush(theme_text_secondary(), brush.GetAddressOf());
            const std::wstring empty = m_entries.empty()
                                          ? (m_loading ? L"正在获取版本列表…"
                                                       : L"暂无版本数据，点击左下角“刷新版本列表”")
                                          : L"该分类下暂无版本";
            target->DrawTextW(empty.c_str(), static_cast<UINT32>(empty.size()), format.Get(),
                              rect_of(list_left, list_top, list_right, list_bottom),
                              brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                              DWRITE_MEASURING_MODE_NATURAL);
        }
    }

    const float offset = m_scroll_view.offset();
    // D-024：可视区外多渲染（overscan）行，快滚时行在进入视口前已绘制 → 连贯；
    // PushAxisAlignedClip 剪裁到视口 → 交界处干净不越界。
    constexpr std::size_t kOverscanRows = 3;
    std::size_t first_row = static_cast<std::size_t>((std::max)(0.0f, offset / kRowHeight));
    first_row = first_row > kOverscanRows ? first_row - kOverscanRows : 0;
    const std::size_t visible = static_cast<std::size_t>(
        (list_bottom - list_top) / kRowHeight) + 2 * kOverscanRows + 2;

    // 分类切换内容淡入（D-033/D-036）：整组行透明度 0→1（~120ms）。
    Microsoft::WRL::ComPtr<ID2D1Layer> layer;
    if (m_filter_anim < 0.999f && SUCCEEDED(target->CreateLayer(layer.GetAddressOf()))) {
        D2D1_LAYER_PARAMETERS params = D2D1::LayerParameters();
        params.opacity = m_filter_anim;
        target->PushLayer(&params, layer.Get());
    }
    target->PushAxisAlignedClip(rect_of(list_left, list_top, list_right, list_bottom),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (std::size_t i = first_row; i < m_filtered.size() && i < first_row + visible; ++i) {
        const float y = list_top + static_cast<float>(i) * kRowHeight - offset;
        draw_row(target, dwrite, i, y);
    }
    target->PopAxisAlignedClip();
    if (layer != nullptr) {
        target->PopLayer();
    }
    m_scroll_view.draw(target);

    // 状态（左面板底部）
    Microsoft::WRL::ComPtr<IDWriteTextFormat> status_format;
    if (SUCCEEDED(dwrite->CreateTextFormat(theme_font_family(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                           12.0f, L"zh-cn", status_format.GetAddressOf()))) {
        status_format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        target->CreateSolidColorBrush(theme_text_secondary(), brush.GetAddressOf());
        target->DrawTextW(m_status.c_str(), static_cast<UINT32>(m_status.size()),
                          status_format.Get(),
                          rect_of(kSidebarMarginDip, m_height_dip - 68.0f,
                                  kSidebarWidthDip - kSidebarMarginDip, m_height_dip - 24.0f),
                          brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                          DWRITE_MEASURING_MODE_NATURAL);
    }

    // 组件选择弹层（D-040；开合 160ms + 内容淡入 120ms，D-033）
    if (m_picker_anim > 0.001f) {
        draw_picker(target, dwrite);
    }
}

void VersionPage::draw_picker(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (m_picker_card == nullptr) {
        return;
    }
    // 遮罩
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> mask;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_background(), mask.GetAddressOf()))) {
        mask->SetOpacity(0.72f * m_picker_anim);
        target->FillRectangle(rect_of(0.0f, kNavHeightDip, m_width_dip, m_height_dip),
                              mask.Get());
    }
    const D2D1_RECT_F card = m_picker_card->bounds;
    const D2D1_POINT_2F center =
        D2D1::Point2F((card.left + card.right) * 0.5f, (card.top + card.bottom) * 0.5f);
    const float scale = 0.94f + 0.06f * ease_out(m_picker_anim);
    target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, center));

    Microsoft::WRL::ComPtr<ID2D1Layer> layer;
    const float opacity = clamp01(m_picker_anim * m_picker_view_anim);
    if (opacity < 0.999f && SUCCEEDED(target->CreateLayer(layer.GetAddressOf()))) {
        D2D1_LAYER_PARAMETERS params = D2D1::LayerParameters();
        params.opacity = opacity;
        target->PushLayer(&params, layer.Get());
    }
    // 主题可能在运行中切换：图标语义色在每帧绘制前重取，避免残留旧配色。
    for (int i = 0; i < kLoaderKindCount; ++i) {
        if (m_picker_kinds[i] != nullptr) {
            m_picker_kinds[i]->icon_color = version_icon_color(picker_icon_kind(i));
        }
    }
    // 卡片（含标题/分段按钮/提示/按钮）
    m_picker_card->draw(target, dwrite);

    // 加载器版本列表（自绘行 + 剪裁 + 滚动条）
    const float list_left = card.left + 20.0f;
    const float list_right = card.right - 20.0f;
    const float list_top = card.top + kPickerHeaderHeight;
    const float list_bottom = card.bottom - kPickerFooterHeight;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> secondary_brush;
    target->CreateSolidColorBrush(theme_text_primary(), text_brush.GetAddressOf());
    target->CreateSolidColorBrush(theme_text_secondary(), secondary_brush.GetAddressOf());
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (SUCCEEDED(dwrite->CreateTextFormat(theme_font_family(), nullptr,
                                           DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-cn",
                                           format.GetAddressOf()))) {
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    if (m_picker_entries.empty()) {
        // D-102：转圈由外壳统一绘制（居中忙碌指示器）；此处只保留空态文案。
        if (format != nullptr) {
            const std::wstring empty = m_picker_loading ? L"正在获取…" : L"暂无可选版本";
            target->DrawTextW(empty.c_str(), static_cast<UINT32>(empty.size()), format.Get(),
                              rect_of(list_left, list_top, list_right, list_bottom),
                              secondary_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                              DWRITE_MEASURING_MODE_NATURAL);
        }
    } else {
        const float offset = m_picker_scroll.offset();
        const std::size_t first = static_cast<std::size_t>((std::max)(0.0f, offset / kPickerRowHeight));
        const std::size_t visible = static_cast<std::size_t>(
            (list_bottom - list_top) / kPickerRowHeight) + 2;
        target->PushAxisAlignedClip(rect_of(list_left, list_top, list_right, list_bottom),
                                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (std::size_t i = first; i < m_picker_entries.size() && i < first + visible; ++i) {
            const float y = list_top + static_cast<float>(i) * kPickerRowHeight - offset;
            const bool selected = (i == m_picker_selected);
            const bool hovered = (i == m_picker_hover);
            if (selected || hovered) {
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> row_brush;
                target->CreateSolidColorBrush(selected ? theme_primary()
                                                       : theme_hover_background(),
                                              row_brush.GetAddressOf());
                if (row_brush != nullptr) {
                    row_brush->SetOpacity(selected ? 0.22f : 1.0f);
                    target->FillRoundedRectangle(
                        D2D1::RoundedRect(rect_of(list_left, y + 2.0f, list_right,
                                                  y + kPickerRowHeight - 2.0f),
                                          6.0f, 6.0f),
                        row_brush.Get());
                }
            }
            const sscl::core::LoaderEntry& entry = m_picker_entries[i];
            const std::wstring version_text = widen(entry.version);
            if (format != nullptr) {
                target->DrawTextW(
                    version_text.c_str(), static_cast<UINT32>(version_text.size()), format.Get(),
                    rect_of(list_left + 14.0f, y, list_right - 140.0f, y + kPickerRowHeight),
                    text_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                    DWRITE_MEASURING_MODE_NATURAL);
                std::wstring tag = entry.recommended ? L"推荐" : (entry.stable ? L"稳定" : L"测试");
                target->DrawTextW(tag.c_str(), static_cast<UINT32>(tag.size()), format.Get(),
                                  rect_of(list_right - 130.0f, y, list_right - 14.0f,
                                          y + kPickerRowHeight),
                                  secondary_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                                  DWRITE_MEASURING_MODE_NATURAL);
            }
        }
        target->PopAxisAlignedClip();
        m_picker_scroll.draw(target);
    }

    if (layer != nullptr) {
        target->PopLayer();
    }
    target->SetTransform(D2D1::Matrix3x2F::Identity());
}

bool VersionPage::hit_download(std::size_t index, float x_dip, float y_dip) const {
    const float list_right = m_width_dip - 24.0f;
    const float list_top = kNavHeightDip + 24.0f;
    const float list_bottom = m_height_dip - 24.0f;
    const float y = list_top + static_cast<float>(index) * kRowHeight - m_scroll_view.offset();
    if (y + kRowHeight < list_top || y > list_bottom) {
        return false;
    }
    return x_dip >= list_right - 150.0f && x_dip <= list_right - 58.0f && y_dip >= y + 6.0f &&
           y_dip <= y + kRowHeight - 10.0f;
}

bool VersionPage::list_viewport_hit(float x_dip, float y_dip) const {
    // D-035：滚动区 = 列表视口（[list_left, list_right]×[list_top, list_bottom]）
    // + 右缘滚动条带（list_right 以右至窗口边缘）。左侧栏/导航栏不算。
    const float list_left = kSidebarWidthDip + 24.0f;
    const float list_top = kNavHeightDip + 24.0f;
    const float list_bottom = m_height_dip - 24.0f;
    return x_dip >= list_left && x_dip <= m_width_dip && y_dip >= list_top &&
           y_dip <= list_bottom;
}

bool VersionPage::mouse_move(float x, float y) {
    m_dirty = false;
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;
    if (m_picker_open) {
        m_picker_card->state_changed = false;
        m_picker_card->on_mouse_move(x_dip, y_dip);
        if (m_picker_card->state_changed) {
            m_dirty = true;
        }
        if (m_picker_scroll.on_mouse_move(x_dip, y_dip)) {
            m_dirty = true;
        }
        std::size_t hover = static_cast<std::size_t>(-1);
        if (!m_picker_entries.empty() && picker_list_hit(x_dip, y_dip)) {
            const float list_top = m_picker_card->bounds.top + kPickerHeaderHeight;
            const std::size_t index = static_cast<std::size_t>((std::max)(
                0.0f, (y_dip - list_top + m_picker_scroll.offset()) / kPickerRowHeight));
            if (index < m_picker_entries.size()) {
                hover = index;
            }
        }
        if (hover != m_picker_hover) {
            m_picker_hover = hover;
            m_dirty = true;
        }
        if (m_dirty && m_window != nullptr) {
            m_window->start_frame_loop();
        }
        return m_dirty;
    }
    m_sidebar.on_mouse_move(x_dip, y_dip);
    if (m_sidebar.state_changed) {
        m_dirty = true;
    }
    if (m_scroll_view.on_mouse_move(x_dip, y_dip)) {
        m_dirty = true;
        if (m_window != nullptr) {
            m_window->start_frame_loop();  // 拖拽/hover 动画（D-022）
        }
    }

    std::size_t hover = static_cast<std::size_t>(-1);
    if (!m_filtered.empty() && list_viewport_hit(x_dip, y_dip)) {
        // D-035：行悬停仅在滚动区内生效（含 x 校验）——侧栏上不高亮列表行。
        // D-036：行号为过滤列表行号。
        const float list_top = kNavHeightDip + 24.0f;
        const std::size_t index = static_cast<std::size_t>(
            (std::max)(0.0f, (y_dip - list_top + m_scroll_view.offset()) / kRowHeight));
        if (index < m_filtered.size()) {
            hover = index;
        }
    }
    if (hover != m_hover_row) {
        m_hover_row = hover;
        m_dirty = true;
    }
    return m_dirty;
}

bool VersionPage::mouse_down(float x, float y) {
    m_dirty = false;
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;
    if (m_picker_open) {
        if (!picker_card_hit(x_dip, y_dip)) {
            close_picker();  // 点击卡片外关闭
            return true;
        }
        m_picker_card->state_changed = false;
        m_picker_card->on_mouse_down(x_dip, y_dip);
        m_picker_scroll.on_mouse_down(x_dip, y_dip);
        return m_picker_card->state_changed;
    }
    if (m_scroll_view.on_mouse_down(x_dip, y_dip)) {
        m_dirty = true;
        return m_dirty;
    }
    m_sidebar.on_mouse_down(x_dip, y_dip);
    if (m_sidebar.state_changed) {
        m_dirty = true;
    }
    return m_dirty;
}

bool VersionPage::mouse_up(float x, float y) {
    m_dirty = false;
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;
    if (m_picker_open) {
        m_picker_card->state_changed = false;
        m_picker_card->on_mouse_up(x_dip, y_dip);
        m_picker_scroll.on_mouse_up(x_dip, y_dip);
        if (!m_picker_entries.empty() && picker_list_hit(x_dip, y_dip)) {
            const float list_top = m_picker_card->bounds.top + kPickerHeaderHeight;
            const std::size_t index = static_cast<std::size_t>((std::max)(
                0.0f, (y_dip - list_top + m_picker_scroll.offset()) / kPickerRowHeight));
            if (index < m_picker_entries.size() && index != m_picker_selected) {
                m_picker_selected = index;
                m_picker_confirm = false;
                m_picker_start->text = L"开始安装";
                m_picker_start->enabled = (m_picker_kind != sscl::core::LoaderKind::kNone);
                m_dirty = true;
            }
        }
        return m_dirty || m_picker_card->state_changed;
    }
    if (m_scroll_view.on_mouse_up(x_dip, y_dip)) {
        m_dirty = true;
    }
    m_sidebar.on_mouse_up(x_dip, y_dip);
    if (m_sidebar.state_changed) {
        m_dirty = true;
    }

    for (std::size_t i = 0; i < m_filtered.size(); ++i) {
        if (hit_download(i, x_dip, y_dip)) {
            // D-040：先弹组件选择（原版/Fabric/…），确认后再进任务页。
            open_picker(m_entries[m_filtered[i]]);
            m_dirty = true;
            break;
        }
    }
    return m_dirty;
}

bool VersionPage::mouse_leave() {
    m_dirty = false;
    if (m_picker_open) {
        m_picker_card->on_mouse_leave();
        if (m_picker_scroll.on_mouse_leave()) {
            m_dirty = true;
        }
        if (m_picker_hover != static_cast<std::size_t>(-1)) {
            m_picker_hover = static_cast<std::size_t>(-1);
            m_dirty = true;
        }
        return m_dirty;
    }
    m_sidebar.on_mouse_leave();
    if (m_scroll_view.on_mouse_leave()) {
        m_dirty = true;
    }
    if (m_hover_row != static_cast<std::size_t>(-1)) {
        m_hover_row = static_cast<std::size_t>(-1);
        m_dirty = true;
    }
    return m_dirty || m_sidebar.state_changed;
}

bool VersionPage::mouse_wheel(float x, float y, float delta) {
    // D-035：滚轮仅在滚动区（列表视口/滚动条带）内生效——侧栏等区域不滚动列表。
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;
    if (m_picker_open) {
        if (!picker_list_hit(x_dip, y_dip)) {
            return false;
        }
        if (m_picker_scroll.on_wheel(delta, kPickerRowHeight * 3.0f)) {
            if (m_window != nullptr) {
                m_window->start_frame_loop();
            }
            return true;
        }
        return false;
    }
    if (!list_viewport_hit(x_dip, y_dip)) {
        return false;
    }
    if (m_scroll_view.on_wheel(delta, kRowHeight * 3.0f)) {
        if (m_window != nullptr) {
            m_window->start_frame_loop();  // 平滑滚动动画（D-022）
        }
        return true;
    }
    return false;
}

bool VersionPage::is_busy() const {
    return m_loading || m_picker_loading;
}

std::wstring VersionPage::busy_text() const {
    if (m_picker_loading) {
        return L"正在获取可选版本…";
    }
    return L"正在获取版本列表…";
}

bool VersionPage::tick(float dt_ms) {
    bool active = m_sidebar.tick(dt_ms) || m_scroll_view.tick(dt_ms);
    // D-102：加载中保持帧循环（转圈相位由 AppShell 单点推进，避免重复加速）。
    if (m_loading || m_picker_loading) {
        active = true;
    }
    // 分类切换内容淡入（D-033/D-036）：时长随动效强度（D-081）
    if (motion::advance(m_filter_anim, 1.0f, dt_ms, motion::Kind::kFade)) {
        active = true;
    }
    // 组件选择弹层：开合（kPop）+ 内容淡入（kFade）+ 列表滚动
    if (motion::advance(m_picker_anim, m_picker_open ? 1.0f : 0.0f, dt_ms,
                        motion::Kind::kPop)) {
        active = true;
    }
    if (m_picker_open &&
        motion::advance(m_picker_view_anim, 1.0f, dt_ms, motion::Kind::kFade)) {
        active = true;
    }
    if (m_picker_anim > 0.001f && m_picker_card != nullptr && m_picker_card->tick(dt_ms)) {
        active = true;  // 分段按钮/操作按钮的 hover 渐变
    }
    if (m_picker_open && m_picker_scroll.tick(dt_ms)) {
        active = true;
    }
    return active;
}

void VersionPage::refresh() {
    if (m_loading || m_service == nullptr) {
        return;
    }
    m_loading = true;
    m_status = L"正在获取版本列表…";

    std::shared_ptr<VersionPage> self = shared_from_this();
    std::thread([self]() {
        const std::filesystem::path cache =
            self->m_service->layout().game_dir / "cache" / "version_manifest.json";
        std::error_code ec;
        std::filesystem::create_directories(cache.parent_path(), ec);

        sscl::util::Result<sscl::core::VersionList> list =
            sscl::core::fetch_version_list(self->m_service->scheduler(), cache);

        self->m_window->post_to_ui([self, list]() {
            self->m_loading = false;
            if (list.is_ok()) {
                self->m_entries = list.value().entries;
                self->rebuild_filter();  // D-036：按当前分类重建过滤索引
                self->m_scroll_view.jump_to(0.0f);
                self->m_filter_anim = 0.0f;  // 新数据淡入（D-033）
                self->m_status = L"共 " + std::to_wstring(self->m_entries.size()) + L" 个版本";
                sscl::util::log_info("[ui] version list loaded: " +
                                     std::to_string(self->m_entries.size()));
            } else {
                self->m_status = L"获取失败：" + widen(list.error().message);
                sscl::util::log_error("[ui] version list failed: " + list.error().code + " / " +
                                      list.error().detail);
            }
            if (self->request_repaint) {
                self->request_repaint();
            }
        });
    }).detach();
}

}  // namespace sscl::ui
