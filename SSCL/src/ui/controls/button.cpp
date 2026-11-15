#include "ui/controls/button.hpp"

#include <algorithm>
#include <utility>

#include "ui/framework/icon_manager.hpp"
#include "ui/theme/theme.hpp"

namespace sscl::ui {

namespace {

// D-020 操作动画：hover 渐变混合。
D2D1_COLOR_F blend_color(D2D1_COLOR_F from, D2D1_COLOR_F to, float t) {
    if (t <= 0.0f) {
        return from;
    }
    if (t >= 1.0f) {
        return to;
    }
    D2D1_COLOR_F out;
    out.r = from.r + (to.r - from.r) * t;
    out.g = from.g + (to.g - from.g) * t;
    out.b = from.b + (to.b - from.b) * t;
    out.a = from.a + (to.a - from.a) * t;
    return out;
}

}  // namespace

Button::Button(std::wstring text, float font_size, bool primary)
    : text(std::move(text)), m_font_size(font_size), m_primary(primary) {}

void Button::draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (target == nullptr || dwrite == nullptr) {
        return;
    }

    // 背景（5 态：normal / hover / pressed / selected / disabled，D-081）
    D2D1_COLOR_F fill = theme_surface();
    D2D1_COLOR_F text_color = theme_text_primary();
    D2D1_COLOR_F border_color = theme_border();
    if (!enabled) {
        // 禁用：不参与 hover，文字弱化（theme-colors §三）。
        fill = theme_surface();
        text_color = theme_text_disabled();
    } else if (m_primary) {
        fill = blend_color(theme_primary(), theme_primary_hover(), hover_anim);
        if (pressed) {
            fill = theme_primary_pressed();
        }
        text_color = theme_text_on_primary();
    } else if (selected) {
        // 选中：PrimaryContainer 底 + Primary 描边（与自绘页同一语言）。
        fill = blend_color(theme_primary_container(), theme_hover_background(), hover_anim * 0.5f);
        if (pressed) {
            fill = theme_hover_background();
        }
        text_color = theme_primary();
        border_color = theme_primary();
    } else {
        // 次要按钮：悬停转向 Hover Background，按下 PrimaryContainer + Primary 描边。
        fill = pressed ? theme_primary_container()
                       : blend_color(theme_surface(), theme_hover_background(), hover_anim);
        text_color = (pressed || hover_anim > 0.4f) ? theme_primary() : theme_text_primary();
        border_color = (pressed || hover_anim > 0.4f) ? theme_primary() : theme_border();
    }

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(fill, brush.GetAddressOf()))) {
        return;
    }

    const D2D1_ROUNDED_RECT rounded =
        D2D1::RoundedRect(bounds, theme_radius(), theme_radius());
    target->FillRoundedRectangle(rounded, brush.Get());

    if (!m_primary) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border_brush;
        if (SUCCEEDED(target->CreateSolidColorBrush(border_color, border_brush.GetAddressOf()))) {
            target->DrawRoundedRectangle(rounded, border_brush.Get(), 1.0f);
        }
    }

    // 文本（居中）
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (FAILED(dwrite->CreateTextFormat(theme_font_family(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        m_font_size, L"zh-cn", format.GetAddressOf()))) {
        return;
    }
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_brush;
    if (FAILED(target->CreateSolidColorBrush(text_color, text_brush.GetAddressOf()))) {
        return;
    }
    D2D1_RECT_F text_rect = bounds;
    if (!icon.empty()) {
        const float height = bounds.bottom - bounds.top;
        const float width = bounds.right - bounds.left;
        const float icon_size = (std::min)(18.0f, (std::max)(10.0f, height - 12.0f));
        const float gap = 6.0f;
        float text_width = 0.0f;
        if (!text.empty()) {
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwrite->CreateTextLayout(
                    text.c_str(), static_cast<UINT32>(text.size()), format.Get(), 10000.0f,
                    height, layout.GetAddressOf())) &&
                layout != nullptr) {
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                    text_width = metrics.widthIncludingTrailingWhitespace;
                }
            }
            if (text_width <= 0.0f) {
                text_width = static_cast<float>(text.size()) * m_font_size;
            }
        }
        const float total = text.empty() ? icon_size : icon_size + gap + text_width;
        float start = bounds.left + (width - total) * 0.5f;
        if (start < bounds.left + 6.0f) {
            start = bounds.left + 6.0f;
        }
        const float center_y = (bounds.top + bounds.bottom) * 0.5f;
        const D2D1_RECT_F icon_rect = D2D1::RectF(start, center_y - icon_size * 0.5f,
                                                  start + icon_size, center_y + icon_size * 0.5f);
        if (!text.empty()) {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            text_rect.left = start + icon_size + gap;
            text_rect.right = (std::min)(bounds.right - 2.0f, start + total);
        }
        IconManager::instance().draw(target, icon, icon_rect,
                                     custom_icon_color ? icon_color : text_color);
    }
    target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), text_rect,
                      text_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                      DWRITE_MEASURING_MODE_NATURAL);
}

}  // namespace sscl::ui
