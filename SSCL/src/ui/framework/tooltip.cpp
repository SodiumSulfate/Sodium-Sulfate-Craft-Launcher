#include "ui/framework/tooltip.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <d2d1helper.h>

#include "ui/theme/theme.hpp"

namespace sscl::ui {

namespace {

D2D1_RECT_F rect_of(float left, float top, float right, float bottom) {
    return D2D1::RectF(left, top, right, bottom);
}

}  // namespace

bool Tooltip::update(std::wstring text, const D2D1_RECT_F& anchor, float mouse_x,
                     float mouse_y) {
    const bool moved = std::abs(mouse_x - m_mouse_x) > 0.5f ||
                       std::abs(mouse_y - m_mouse_y) > 0.5f;
    m_mouse_x = mouse_x;
    m_mouse_y = mouse_y;
    m_anchor = anchor;
    if (text.empty()) {
        const bool changed = !m_text.empty() || m_visible;
        clear();
        return changed;
    }
    if (text != m_text) {
        m_text = std::move(text);
        m_elapsed = 0.0f;
        m_visible = false;
        return true;
    }
    return moved && m_visible;
}

void Tooltip::clear() {
    m_text.clear();
    m_elapsed = 0.0f;
    m_visible = false;
    m_anchor = D2D1_RECT_F{};
}

bool Tooltip::tick(float dt_ms) {
    if (m_text.empty() || m_visible) {
        return false;
    }
    m_elapsed += (std::max)(0.0f, dt_ms);
    if (m_elapsed >= kDelayMs) {
        m_visible = true;
        return true;
    }
    // 保持帧循环，直到延迟结束；否则 AppShell 会在一帧后停止 tick。
    return true;
}

void Tooltip::draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite, float width,
                   float height) const {
    if (!m_visible || m_text.empty() || target == nullptr || dwrite == nullptr || width <= 0.0f ||
        height <= 0.0f) {
        return;
    }
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (FAILED(dwrite->CreateTextFormat(theme_font_family(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        10.5f, L"zh-cn", format.GetAddressOf()))) {
        return;
    }
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite->CreateTextLayout(m_text.c_str(), static_cast<UINT32>(m_text.size()),
                                        format.Get(), 320.0f, 180.0f,
                                        layout.GetAddressOf())) ||
        layout == nullptr) {
        return;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) {
        return;
    }
    const float padding_x = 10.0f;
    const float padding_y = 7.0f;
    float box_w = (std::min)(340.0f, metrics.widthIncludingTrailingWhitespace + padding_x * 2.0f);
    float box_h = metrics.height + padding_y * 2.0f;
    if (box_w < 48.0f) {
        box_w = 48.0f;
    }
    if (box_h < 30.0f) {
        box_h = 30.0f;
    }
    float x = m_mouse_x + 14.0f;
    float y = m_mouse_y + 20.0f;
    if (x + box_w > width - 8.0f) {
        x = width - box_w - 8.0f;
    }
    if (y + box_h > height - 8.0f) {
        y = m_mouse_y - box_h - 12.0f;
    }
    x = (std::max)(8.0f, (std::min)(x, width - box_w - 8.0f));
    y = (std::max)(8.0f, (std::min)(y, height - box_h - 8.0f));
    const D2D1_RECT_F box = rect_of(x, y, x + box_w, y + box_h);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> background;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text;
    if (FAILED(target->CreateSolidColorBrush(theme_elevated_surface(),
                                             background.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_border(), border.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_text_primary(), text.GetAddressOf()))) {
        return;
    }
    background->SetOpacity(0.98f);
    target->FillRoundedRectangle(D2D1::RoundedRect(box, 6.0f, 6.0f), background.Get());
    target->DrawRoundedRectangle(D2D1::RoundedRect(box, 6.0f, 6.0f), border.Get(), 1.0f);
    const D2D1_POINT_2F origin = D2D1::Point2F(x + padding_x, y + padding_y);
    target->DrawTextLayout(origin, layout.Get(), text.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

}  // namespace sscl::ui
