#include "ui/setup/setup_wizard.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <thread>
#include <utility>

#include <d2d1helper.h>
#include <wrl/client.h>

#include "core/config/secret_store.hpp"
#include "core/java/java_manager.hpp"
#include "ui/framework/window.hpp"
#include "ui/theme/theme.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

namespace sscl::ui {

namespace {

D2D1_RECT_F rect_of(float left, float top, float right, float bottom) {
    return D2D1::RectF(left, top, right, bottom);
}

bool rect_valid(const D2D1_RECT_F& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

std::wstring widen(const std::string& text) {
    return sscl::util::utf8_to_wide(text);
}

std::string narrow(const std::wstring& text) {
    return sscl::util::wide_to_utf8(text);
}

std::wstring trim_wide(const std::wstring& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::iswspace(text[begin]) != 0) {
        ++begin;
    }
    while (end > begin && std::iswspace(text[end - 1]) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

void draw_text(ID2D1RenderTarget* target, IDWriteFactory* dwrite, const std::wstring& text,
               const D2D1_RECT_F& rect, float size, DWRITE_FONT_WEIGHT weight,
               const D2D1_COLOR_F& color,
               DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING,
               bool wrap = false) {
    if (target == nullptr || dwrite == nullptr || text.empty() || !rect_valid(rect)) {
        return;
    }
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (FAILED(dwrite->CreateTextFormat(theme_font_family(), nullptr, weight,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        size, L"zh-cn", format.GetAddressOf()))) {
        return;
    }
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetTextAlignment(alignment);
    format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
        return;
    }
    target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), rect,
                      brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
                      DWRITE_MEASURING_MODE_NATURAL);
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
                  const D2D1_COLOR_F& color, float width = 1.0f) {
    if (target == nullptr || !rect_valid(rect)) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(target->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
        target->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), width);
    }
}

}  // namespace

SetupWizard::SetupWizard(AppWindow* window, sscl::core::LauncherConfig config,
                         std::filesystem::path secrets_file,
                         std::function<void(const sscl::core::LauncherConfig&)> on_apply)
    : m_window(window), m_config(std::move(config)),
      m_secrets_file(std::move(secrets_file)), m_on_apply(std::move(on_apply)) {
    build_controls();
    set_step(Step::kWelcome);
}

SetupWizard::~SetupWizard() {
    ++m_scan_seq;  // 作废在途扫描回调（窗口销毁后不得再写 UI 状态）
}

void SetupWizard::build_controls() {
    auto title = std::make_unique<Label>(L"", 24.0f, theme_text_primary(), true);
    auto subtitle = std::make_unique<Label>(L"", 12.5f, theme_text_secondary());
    auto primary = std::make_unique<Button>(L"下一步", 13.5f, true);
    auto skip = std::make_unique<Button>(L"跳过", 12.5f, false);
    auto back = std::make_unique<Button>(L"上一步", 12.5f, false);

    auto intro1 = std::make_unique<Label>(
        L"SSCL 由 Sodium Sulfate Studio 开发，使用 C++ 与 Direct2D 自绘界面，"
        L"为 Windows 上的 Minecraft 提供轻快、完整的启动体验。",
        13.0f, theme_text_primary());
    auto intro2 = std::make_unique<Label>(
        L"接下来用几步完成基础设置：外观主题、Java 运行时顺序、以及可选的 CurseForge Key。",
        13.0f, theme_text_secondary());
    auto intro3 = std::make_unique<Label>(
        L"每一步都可以跳过，所有内容之后都能在「设置」里随时调整。",
        13.0f, theme_text_muted());

    auto theme_system = std::make_unique<Button>(L"跟随系统", 13.0f, false);
    auto theme_light = std::make_unique<Button>(L"浅色主题", 13.0f, false);
    auto theme_dark = std::make_unique<Button>(L"深色主题", 13.0f, false);
    auto theme_hint = std::make_unique<Label>(
        L"「跟随系统」会随 Windows 应用浅/深色模式自动切换；点击可立即预览。",
        11.5f, theme_text_muted());

    auto java_status = std::make_unique<Label>(L"正在扫描 Java…", 13.0f, theme_text_primary());
    auto java_hint = std::make_unique<Label>(
        L"列表越靠前的 Java 越优先用于启动游戏；用 ↑ / ↓ 调整顺序。",
        11.5f, theme_text_muted());

    auto key_box = std::make_unique<TextBox>(L"", 13.0f);
    key_box->placeholder = L"粘贴 CurseForge API Key（可留空）";
    key_box->max_length = 128;
    auto key_emphasis = std::make_unique<Label>(
        L"可以放心跳过 · SSCL 默认使用构建期 Key 与国内镜像源，"
        L"跳过完全不影响搜索和下载 Mod。",
        13.0f, theme_primary(), true);
    auto key_hint = std::make_unique<Label>(
        L"填写后仅以 DPAPI 加密保存在本机，不会上传；留空或跳过则不保存。",
        11.5f, theme_text_muted());

    auto done1 = std::make_unique<Label>(
        L"设置已经完成并应用，config.json 也已保存。", 15.0f, theme_text_primary(), true);
    auto done2 = std::make_unique<Label>(
        L"现在可以开始正常使用 SSCL 了：下载版本、安装加载器、搜索 Mod，或从主页一键启动。",
        13.0f, theme_text_secondary());

    m_title = title.get();
    m_subtitle = subtitle.get();
    m_primary = primary.get();
    m_skip = skip.get();
    m_back = back.get();
    m_intro1 = intro1.get();
    m_intro2 = intro2.get();
    m_intro3 = intro3.get();
    m_theme_system = theme_system.get();
    m_theme_light = theme_light.get();
    m_theme_dark = theme_dark.get();
    m_theme_hint = theme_hint.get();
    m_java_status = java_status.get();
    m_java_hint = java_hint.get();
    m_key_box = key_box.get();
    m_key_emphasis = key_emphasis.get();
    m_key_hint = key_hint.get();
    m_done1 = done1.get();
    m_done2 = done2.get();

    m_primary->icon = "check";
    m_skip->icon = "arrow_forward";
    m_back->icon = "arrow_back";
    m_theme_system->icon = "settings";
    m_theme_light->icon = "public";
    m_theme_dark->icon = "star";

    // 长段说明需自动换行；单行 NO_WRAP 会把介绍/强调文案从中间截断。
    for (Label* label : {m_intro1, m_intro2, m_intro3, m_theme_hint, m_java_hint,
                         m_key_emphasis, m_key_hint, m_done1, m_done2}) {
        label->word_wrap = true;
        label->vertical_center = true;
    }

    primary->on_click = [this]() { next_step(); };
    skip->on_click = [this]() { skip_current(); };
    back->on_click = [this]() { previous_step(); };
    theme_system->on_click = [this]() { choose_theme("system"); };
    theme_light->on_click = [this]() { choose_theme("light"); };
    theme_dark->on_click = [this]() { choose_theme("dark"); };

    m_root.add(std::move(title));
    m_root.add(std::move(subtitle));
    m_root.add(std::move(intro1));
    m_root.add(std::move(intro2));
    m_root.add(std::move(intro3));
    m_root.add(std::move(theme_system));
    m_root.add(std::move(theme_light));
    m_root.add(std::move(theme_dark));
    m_root.add(std::move(theme_hint));
    m_root.add(std::move(java_status));
    m_root.add(std::move(java_hint));
    m_root.add(std::move(key_box));
    m_root.add(std::move(key_emphasis));
    m_root.add(std::move(key_hint));
    m_root.add(std::move(done1));
    m_root.add(std::move(done2));
    m_root.add(std::move(primary));
    m_root.add(std::move(skip));
    m_root.add(std::move(back));
}

void SetupWizard::set_step(Step step) {
    m_step = step;
    m_back->text = L"上一步";
    switch (step) {
        case Step::kWelcome:
            m_title->text = L"欢迎使用 Sodium Sulfate Craft Launcher";
            m_subtitle->text = L"首次启动 · 用一分钟把体验调整到最舒服的状态";
            m_primary->text = L"开始设置";
            m_skip->text = L"跳过向导";
            break;
        case Step::kTheme:
            m_title->text = L"选择程序主题";
            m_subtitle->text = L"第 2 / 5 步 · 外观偏好（之后可在 设置 → 外观 修改）";
            m_primary->text = L"下一步";
            m_skip->text = L"跳过此步";
            refresh_theme_buttons();
            break;
        case Step::kJava:
            m_title->text = L"选择并排序 Java";
            m_subtitle->text = L"第 3 / 5 步 · 进入本页时会自动扫描 java.exe";
            m_primary->text = L"下一步";
            m_skip->text = L"跳过此步";
            m_java_scroll.jump_to(0.0f);
            start_java_scan();
            break;
        case Step::kCurseForge:
            m_title->text = L"CurseForge API Key（可选）";
            m_subtitle->text = L"第 4 / 5 步 · 这一项完全可以放心跳过";
            m_primary->text = L"下一步";
            m_skip->text = L"跳过此步";
            break;
        case Step::kDone:
            m_title->text = L"设置已完成";
            m_subtitle->text = L"第 5 / 5 步 · 所有更改都已应用并保存";
            m_primary->text = L"开始使用 SSCL";
            m_skip->text = L"跳过并结束";
            break;
    }
    apply_visibility();
    layout_controls();
    if (m_window != nullptr) {
        m_window->invalidate();
    }
}

void SetupWizard::apply_visibility() {
    const bool welcome = m_step == Step::kWelcome;
    const bool theme = m_step == Step::kTheme;
    const bool java = m_step == Step::kJava;
    const bool key = m_step == Step::kCurseForge;
    const bool done = m_step == Step::kDone;

    m_intro1->visible = welcome;
    m_intro2->visible = welcome;
    m_intro3->visible = welcome;
    m_theme_system->visible = theme;
    m_theme_light->visible = theme;
    m_theme_dark->visible = theme;
    m_theme_hint->visible = theme;
    m_java_status->visible = java;
    m_java_hint->visible = java;
    m_key_box->visible = key;
    m_key_emphasis->visible = key;
    m_key_hint->visible = key;
    m_done1->visible = done;
    m_done2->visible = done;
    m_back->visible = !welcome;   // 第一页没有上一步
    m_skip->visible = !done;      // 最后一页没有跳过
    m_key_box->set_focused(key);
}

void SetupWizard::layout(float width_dip, float height_dip, float dpi) {
    m_width_dip = width_dip;
    m_height_dip = height_dip;
    m_to_dip = dpi > 0.0f ? 96.0f / dpi : 1.0f;
    const float card_w = (std::min)(760.0f, width_dip - 64.0f);
    const float card_h = (std::min)(470.0f, height_dip - 64.0f);
    const float left = (width_dip - card_w) * 0.5f;
    const float top = (height_dip - card_h) * 0.5f;
    m_card = rect_of(left, top, left + card_w, top + card_h);
    layout_controls();
}

void SetupWizard::layout_controls() {
    const float left = m_card.left + 36.0f;
    const float right = m_card.right - 36.0f;
    const float top = m_card.top;
    const float bottom = m_card.bottom;

    m_title->bounds = rect_of(left, top + 34.0f, right, top + 72.0f);
    m_subtitle->bounds = rect_of(left, top + 76.0f, right, top + 102.0f);

    const float content_top = top + 124.0f;
    const float content_bottom = bottom - 104.0f;

    m_intro1->bounds = rect_of(left, content_top, right, content_top + 58.0f);
    m_intro2->bounds = rect_of(left, content_top + 66.0f, right, content_top + 124.0f);
    m_intro3->bounds = rect_of(left, content_top + 132.0f, right, content_top + 180.0f);

    const float theme_w = (right - left - 24.0f) / 3.0f;
    m_theme_system->bounds = rect_of(left, content_top + 12.0f, left + theme_w, content_top + 58.0f);
    m_theme_light->bounds = rect_of(left + theme_w + 12.0f, content_top + 12.0f,
                                    left + theme_w * 2.0f + 12.0f, content_top + 58.0f);
    m_theme_dark->bounds = rect_of(left + theme_w * 2.0f + 24.0f, content_top + 12.0f, right,
                                   content_top + 58.0f);
    m_theme_hint->bounds = rect_of(left, content_top + 78.0f, right, content_top + 128.0f);

    m_java_status->bounds = rect_of(left, content_top, right, content_top + 28.0f);
    m_list_rect = rect_of(left, content_top + 36.0f, right, content_bottom - 44.0f);
    // 滚动条贴列表右缘；条目绘制与按钮留出 16px，避免被滚动条压住。
    // 构造期 set_step 会先于首次 layout() 调用到这里，矩形无效时跳过以免出现负视口。
    if (rect_valid(m_list_rect)) {
        m_java_scroll.layout(m_list_rect.top, m_list_rect.bottom,
                             static_cast<float>(m_java_entries.size()) * m_row_height,
                             m_list_rect.right - 4.0f);
    }
    m_java_hint->bounds = rect_of(left, content_bottom - 42.0f, right, content_bottom);

    m_key_emphasis->bounds = rect_of(left, content_top, right, content_top + 62.0f);
    m_key_box->bounds = rect_of(left, content_top + 76.0f, right, content_top + 116.0f);
    m_key_hint->bounds = rect_of(left, content_top + 128.0f, right, content_top + 172.0f);

    m_done1->bounds = rect_of(left, content_top + 8.0f, right, content_top + 52.0f);
    m_done2->bounds = rect_of(left, content_top + 60.0f, right, content_top + 124.0f);

    // 左侧「上一步」；右侧「跳过此步」紧邻「下一步」左边。
    m_back->bounds = rect_of(left, bottom - 72.0f, left + 132.0f, bottom - 34.0f);
    m_skip->bounds = rect_of(right - 312.0f, bottom - 72.0f, right - 180.0f, bottom - 34.0f);
    m_primary->bounds = rect_of(right - 168.0f, bottom - 72.0f, right, bottom - 34.0f);
}

void SetupWizard::draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (!m_active || target == nullptr) {
        return;
    }
    // 半透明遮罩：盖住主窗口内容，但仍透出主题背景，保持层次感。
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dim;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_background(), dim.GetAddressOf()))) {
        dim->SetOpacity(0.88f);
        target->FillRectangle(rect_of(0.0f, 0.0f, m_width_dip, m_height_dip), dim.Get());
    }
    draw_card(target);
    draw_progress(target);
    if (m_step == Step::kJava) {
        draw_java_list(target, dwrite);
    }
    m_root.draw(target, dwrite);
}

void SetupWizard::draw_card(ID2D1RenderTarget* target) const {
    fill_round(target, m_card, 16.0f, theme_elevated_surface());
    stroke_round(target, m_card, 16.0f, theme_border(), 1.2f);
    // 标题下的分隔线
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> line;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_border(), line.GetAddressOf()))) {
        line->SetOpacity(0.7f);
        target->DrawLine(D2D1::Point2F(m_card.left + 36.0f, m_card.top + 112.0f),
                         D2D1::Point2F(m_card.right - 36.0f, m_card.top + 112.0f), line.Get(),
                         1.0f);
    }
}

void SetupWizard::draw_progress(ID2D1RenderTarget* target) const {
    const float center_x = (m_card.left + m_card.right) * 0.5f;
    const float y = m_card.bottom - 20.0f;
    constexpr float kGap = 22.0f;
    constexpr float kRadius = 4.0f;
    for (int i = 0; i < kStepCount; ++i) {
        const float cx = center_x + (static_cast<float>(i) - 2.0f) * kGap;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dot;
        const D2D1_COLOR_F color =
            i == static_cast<int>(m_step) ? theme_primary() : theme_border();
        if (SUCCEEDED(target->CreateSolidColorBrush(color, dot.GetAddressOf()))) {
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, y), kRadius, kRadius), dot.Get());
        }
    }
}

void SetupWizard::draw_java_list(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    m_row_hits.clear();
    if (!rect_valid(m_list_rect)) {
        return;
    }
    const int count = static_cast<int>(m_java_entries.size());
    // 条目数变化/窗口缩放后刷新滚动范围；无溢出时 SmoothScroll 不画滚动条。
    m_java_scroll.layout(m_list_rect.top, m_list_rect.bottom,
                         static_cast<float>(count) * m_row_height,
                         m_list_rect.right - 4.0f);
    fill_round(target, m_list_rect, 10.0f, theme_container());
    if (count == 0) {
        const std::wstring text = m_scanning ? L"正在扫描 Java，请稍候…"
                                             : L"未找到 Java · 可以直接跳过，SSCL 会在启动时自动检测";
        draw_text(target, dwrite, text,
                  rect_of(m_list_rect.left + 14.0f, m_list_rect.top,
                          m_list_rect.right - 14.0f, m_list_rect.top + 40.0f),
                  12.0f, DWRITE_FONT_WEIGHT_NORMAL, theme_text_muted());
        return;
    }
    target->PushAxisAlignedClip(m_list_rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    // 平滑滚动偏移：允许最后一行只露出一部分，明确提示「下面还有」。
    const float offset = m_java_scroll.offset();
    const int first_row = (std::max)(0, static_cast<int>(offset / m_row_height));
    for (int i = first_row; i < count; ++i) {
        const float row_top = m_list_rect.top + static_cast<float>(i) * m_row_height - offset;
        if (row_top >= m_list_rect.bottom) {
            break;
        }
        if (row_top + m_row_height <= m_list_rect.top) {
            continue;
        }
        const D2D1_RECT_F row =
            rect_of(m_list_rect.left + 4.0f, row_top + 2.0f, m_list_rect.right - 16.0f,
                    row_top + m_row_height - 2.0f);
        if (i == m_hover_row) {
            fill_round(target, row, 8.0f, theme_hover_background());
        }
        const std::wstring version =
            m_java_entries[static_cast<std::size_t>(i)].major > 0
                ? L"Java " + std::to_wstring(m_java_entries[static_cast<std::size_t>(i)].major)
                : L"Java";
        draw_text(target, dwrite, version,
                  rect_of(row.left + 12.0f, row.top, row.left + 92.0f, row.bottom), 12.5f,
                  DWRITE_FONT_WEIGHT_SEMI_BOLD, theme_primary());
        draw_text(target, dwrite, widen(m_java_entries[static_cast<std::size_t>(i)].path),
                  rect_of(row.left + 100.0f, row.top, row.right - 76.0f, row.bottom), 11.5f,
                  DWRITE_FONT_WEIGHT_NORMAL, theme_text_secondary());

        const D2D1_RECT_F up =
            rect_of(row.right - 70.0f, row.top + 6.0f, row.right - 40.0f, row.bottom - 6.0f);
        const D2D1_RECT_F down =
            rect_of(row.right - 36.0f, row.top + 6.0f, row.right - 6.0f, row.bottom - 6.0f);
        fill_round(target, up, 6.0f, theme_surface());
        fill_round(target, down, 6.0f, theme_surface());
        draw_text(target, dwrite, L"↑", up, 13.0f, DWRITE_FONT_WEIGHT_BOLD,
                  theme_text_primary(), DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(target, dwrite, L"↓", down, 13.0f, DWRITE_FONT_WEIGHT_BOLD,
                  theme_text_primary(), DWRITE_TEXT_ALIGNMENT_CENTER);
        m_row_hits.push_back(RowHit{up, down, i});
    }
    target->PopAxisAlignedClip();
    // 滚动条画在列表之上、底部按钮之下（位置不重叠）。
    m_java_scroll.draw(target);
}

int SetupWizard::row_hit(float x_dip, float y_dip, bool* up) const {
    // 平滑滚动时首/末行可能只露出一部分：命中仍限制在列表可视区内。
    if (y_dip < m_list_rect.top || y_dip >= m_list_rect.bottom) {
        return -1;
    }
    for (const RowHit& hit : m_row_hits) {
        if (x_dip >= hit.up.left && x_dip < hit.up.right && y_dip >= hit.up.top &&
            y_dip < hit.up.bottom) {
            *up = true;
            return hit.index;
        }
        if (x_dip >= hit.down.left && x_dip < hit.down.right && y_dip >= hit.down.top &&
            y_dip < hit.down.bottom) {
            *up = false;
            return hit.index;
        }
    }
    return -1;
}

bool SetupWizard::mouse_move(float x_physical, float y_physical) {
    if (!m_active) {
        return false;
    }
    const float x = x_physical * m_to_dip;
    const float y = y_physical * m_to_dip;
    bool changed = false;
    if (m_step == Step::kJava) {
        if (m_java_scroll.on_mouse_move(x, y)) {
            changed = true;
        }
        int hover = -1;
        if (x >= m_list_rect.left && x < m_list_rect.right && y >= m_list_rect.top &&
            y < m_list_rect.bottom) {
            for (const RowHit& hit : m_row_hits) {
                const D2D1_RECT_F row = rect_of(m_list_rect.left, hit.up.top - 6.0f,
                                                m_list_rect.right - 16.0f, hit.down.bottom + 6.0f);
                if (x >= row.left && x < row.right && y >= row.top && y < row.bottom) {
                    hover = hit.index;
                    break;
                }
            }
        }
        if (hover != m_hover_row) {
            m_hover_row = hover;
            changed = true;
        }
    }
    m_root.state_changed = false;
    m_root.on_mouse_move(x, y);
    if (changed || m_root.state_changed) {
        // 按钮 hover 渐变需要帧循环推进（主窗口侧只负责 invalidate）。
        if (m_window != nullptr) {
            m_window->start_frame_loop();
        }
    }
    return changed || m_root.state_changed;
}

bool SetupWizard::mouse_down(float x_physical, float y_physical) {
    if (!m_active) {
        return false;
    }
    const float x = x_physical * m_to_dip;
    const float y = y_physical * m_to_dip;
    // 滚动条优先（拖拽/点击轨道），再处理列表排序按钮。
    if (m_step == Step::kJava) {
        if (m_java_scroll.on_mouse_down(x, y)) {
            if (m_window != nullptr) {
                m_window->start_frame_loop();
            }
            return true;
        }
        bool dummy = false;
        if (row_hit(x, y, &dummy) >= 0) {
            return true;
        }
    }
    // 点击输入框时聚焦（第四页）。
    if (m_step == Step::kCurseForge && m_key_box->hit_test(x, y)) {
        m_key_box->set_focused(true);
    }
    m_root.state_changed = false;
    m_root.on_mouse_down(x, y);
    return true;
}

bool SetupWizard::mouse_up(float x_physical, float y_physical) {
    if (!m_active) {
        return false;
    }
    const float x = x_physical * m_to_dip;
    const float y = y_physical * m_to_dip;
    if (m_step == Step::kJava && m_java_scroll.on_mouse_up(x, y)) {
        return true;  // 刚结束滚动条拖拽，不再触发行内按钮
    }
    bool up = false;
    const int row = (m_step == Step::kJava) ? row_hit(x, y, &up) : -1;
    if (row >= 0) {
        move_java(row, up ? -1 : 1);
        if (m_window != nullptr) {
            m_window->invalidate();
        }
        return true;
    }
    m_root.state_changed = false;
    m_root.on_mouse_up(x, y);
    return true;
}

bool SetupWizard::mouse_wheel(float x_physical, float y_physical, float delta) {
    if (!m_active || m_step != Step::kJava) {
        return false;
    }
    const float x = x_physical * m_to_dip;
    const float y = y_physical * m_to_dip;
    if (x < m_list_rect.left || x >= m_list_rect.right || y < m_list_rect.top ||
        y >= m_list_rect.bottom) {
        return false;
    }
    // 与附加页一致：滚轮只更新目标偏移，tick 内指数趋近（平滑滚动）。
    if (!m_java_scroll.on_wheel(delta, m_row_height * 3.0f)) {
        return false;
    }
    if (m_window != nullptr) {
        m_window->start_frame_loop();
    }
    return true;
}

bool SetupWizard::mouse_leave() {
    if (!m_active) {
        return false;
    }
    bool changed = false;
    if (m_step == Step::kJava && m_java_scroll.on_mouse_leave()) {
        changed = true;
    }
    if (m_hover_row != -1) {
        m_hover_row = -1;
        changed = true;
    }
    m_root.state_changed = false;
    m_root.on_mouse_leave();
    return changed || m_root.state_changed;
}

bool SetupWizard::handle_char(wchar_t ch) {
    if (!m_active || m_step != Step::kCurseForge) {
        return false;
    }
    return m_key_box->handle_char(ch);
}

bool SetupWizard::handle_key_down(unsigned long key) {
    if (!m_active) {
        return false;
    }
    if (key == VK_ESCAPE) {
        skip_current();
        return true;
    }
    if (m_step != Step::kCurseForge) {
        return false;
    }
    return m_key_box->handle_key_down(key);
}

bool SetupWizard::tick(float dt_ms) {
    if (!m_active) {
        return false;
    }
    bool active = m_root.tick(dt_ms);
    if (m_step == Step::kJava && m_java_scroll.tick(dt_ms)) {
        active = true;
    }
    return active;
}

void SetupWizard::choose_theme(const std::string& key) {
    m_config.theme_mode = key;
    sscl::ui::apply_theme_preference(key);  // 全局语义色立即切换，主窗口随之实时预览
    refresh_theme_buttons();
    commit();
    if (m_window != nullptr) {
        m_window->start_frame_loop();
        m_window->invalidate();
    }
}

void SetupWizard::refresh_theme_buttons() {
    m_theme_system->selected = m_config.theme_mode == "system";
    m_theme_light->selected = m_config.theme_mode == "light";
    m_theme_dark->selected = m_config.theme_mode == "dark";
}

void SetupWizard::start_java_scan() {
    if (m_java_started) {
        return;
    }
    m_java_started = true;
    m_scanning = true;
    m_java_status->text = L"正在扫描 Java…";
    const std::uint64_t seq = ++m_scan_seq;

    std::vector<std::string> dirs = m_config.java_scan_dirs;
    if (dirs.empty()) {
        // 极端情况（AppShell 尚未填充）时自行取默认扫描位置，保证这一页永远有内容。
        for (const std::filesystem::path& dir : sscl::core::default_java_scan_dirs()) {
            dirs.push_back(sscl::util::wide_to_utf8(dir.wstring()));
        }
    }
    const std::weak_ptr<SetupWizard> weak = weak_from_this();
    std::thread([weak, dirs, seq]() {
        std::vector<std::filesystem::path> roots;
        roots.reserve(dirs.size());
        for (const std::string& dir : dirs) {
            if (!dir.empty()) {
                roots.emplace_back(sscl::util::utf8_to_wide(dir));
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
        std::shared_ptr<SetupWizard> page = weak.lock();
        if (!page || page->m_window == nullptr) {
            return;
        }
        // D-123：post_to_ui 支持窗口未创建时暂存，销毁后丢弃；这里只访问 UI 线程状态。
        page->m_window->post_to_ui([weak, found, seq]() {
            std::shared_ptr<SetupWizard> alive = weak.lock();
            if (!alive || !alive->m_active || seq != alive->m_scan_seq) {
                return;
            }
            alive->m_java_entries = std::move(found);
            alive->m_scanning = false;
            alive->m_java_status->text =
                alive->m_java_entries.empty()
                    ? L"未找到 Java · 可以直接跳过，SSCL 会在启动时自动检测"
                    : L"已找到 " + std::to_wstring(alive->m_java_entries.size()) +
                          L" 个 Java 运行时";
            alive->sync_java_config();
            alive->apply_visibility();
            if (alive->m_window != nullptr) {
                alive->m_window->invalidate();
            }
        });
    }).detach();
}

void SetupWizard::sync_java_config() {
    m_config.java_candidates.clear();
    for (const JavaEntry& entry : m_java_entries) {
        if (!entry.path.empty()) {
            m_config.java_candidates.push_back(entry.path);
        }
    }
    commit();
}

void SetupWizard::move_java(int index, int delta) {
    const int count = static_cast<int>(m_java_entries.size());
    const int target = index + delta;
    if (index < 0 || index >= count || target < 0 || target >= count) {
        return;
    }
    std::swap(m_java_entries[static_cast<std::size_t>(index)],
              m_java_entries[static_cast<std::size_t>(target)]);
    sync_java_config();  // 顺序即优先级，改动立即落盘
}

void SetupWizard::save_curseforge_key() {
    const std::string key =
        sscl::util::wide_to_utf8(trim_wide(m_key_box->text));
    if (key.empty()) {
        return;  // 留空 = 不保存，继续使用构建期 Key / 镜像源
    }
    const sscl::util::Result<void> saved =
        sscl::core::store_secret_value(m_secrets_file, "curseforge_api_key", key);
    if (saved.is_error()) {
        sscl::util::log_warn("[setup] curseforge key save failed: " + saved.error().detail);
    }
}

void SetupWizard::commit() {
    if (m_on_apply) {
        m_on_apply(m_config);
    }
}

void SetupWizard::next_step() {
    if (m_step == Step::kCurseForge) {
        save_curseforge_key();
    }
    if (m_step == Step::kDone) {
        finish();
        return;
    }
    commit();
    set_step(static_cast<Step>(static_cast<int>(m_step) + 1));
}

void SetupWizard::previous_step() {
    if (m_step == Step::kWelcome) {
        return;
    }
    set_step(static_cast<Step>(static_cast<int>(m_step) - 1));
}

void SetupWizard::skip_current() {
    // 第一页 = 跳过整个向导，所有值保持默认（不做任何改动）。
    if (m_step == Step::kWelcome) {
        finish();
        return;
    }
    switch (m_step) {
        case Step::kTheme:
            m_config.theme_mode = "system";
            sscl::ui::apply_theme_preference("system");
            refresh_theme_buttons();
            break;
        case Step::kJava:
            ++m_scan_seq;  // 作废在途扫描，避免跳过之后又把结果写回配置
            m_java_entries.clear();
            m_java_scroll.jump_to(0.0f);
            m_config.java_candidates.clear();
            // 允许「上一步」回到本页时重新扫描（跳过时列表已被清空）。
            m_java_started = false;
            m_scanning = false;
            m_java_status->text = L"未找到 Java · 可以直接跳过，SSCL 会在启动时自动检测";
            break;
        case Step::kCurseForge:
            m_key_box->text.clear();  // 不保存 Key，使用构建期 Key / 镜像源
            break;
        case Step::kDone:
            finish();
            return;
        default:
            break;
    }
    commit();
    set_step(static_cast<Step>(static_cast<int>(m_step) + 1));
}

void SetupWizard::finish() {
    m_active = false;
    ++m_scan_seq;
    commit();  // 保证 config.json 存在且为最终值
    if (m_window != nullptr) {
        m_window->invalidate();
    }
}

void SetupWizard::dismiss() {
    m_active = false;
    ++m_scan_seq;
    if (m_window != nullptr) {
        m_window->invalidate();
    }
}

}  // namespace sscl::ui
