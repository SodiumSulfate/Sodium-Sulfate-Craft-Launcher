#include "ui/controls/card.hpp"

#include <utility>

#include "ui/theme/theme.hpp"

namespace sscl::ui {

Card::Card(std::wstring title) : title(std::move(title)) {}

void Card::draw_background(ID2D1RenderTarget* target) const {
    // 柔和阴影：3 层偏移半透明圆角矩形（自绘近似，§8）。
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shadow;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_card_shadow(), shadow.GetAddressOf()))) {
        for (int layer = 3; layer >= 1; --layer) {
            const float spread = static_cast<float>(layer) * 1.2f;
            const D2D1_RECT_F rect =
                D2D1::RectF(bounds.left, bounds.top + spread, bounds.right,
                            bounds.bottom + spread);
            const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(
                rect, theme_radius() + spread * 0.5f, theme_radius() + spread * 0.5f);
            target->FillRoundedRectangle(rounded, shadow.Get());
        }
    }

    const D2D1_COLOR_F fill = elevated ? theme_elevated_surface() : theme_surface();
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> surface;
    if (SUCCEEDED(target->CreateSolidColorBrush(fill, surface.GetAddressOf()))) {
        const D2D1_ROUNDED_RECT rounded =
            D2D1::RoundedRect(bounds, theme_radius(), theme_radius());
        target->FillRoundedRectangle(rounded, surface.Get());
    }
    // 弹窗态：加描边，与深色背景边界分明。
    if (elevated) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border;
        if (SUCCEEDED(target->CreateSolidColorBrush(theme_border(), border.GetAddressOf()))) {
            border->SetOpacity(1.0f);
            const D2D1_ROUNDED_RECT rounded =
                D2D1::RoundedRect(bounds, theme_radius(), theme_radius());
            target->DrawRoundedRectangle(rounded, border.Get(), 1.0f);
        }
    }
}

void Card::draw_title(ID2D1RenderTarget* target, IDWriteFactory* dwrite) const {
    if (title.empty()) {
        return;
    }
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (FAILED(dwrite->CreateTextFormat(theme_font_family(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        title_font_size, L"zh-cn", format.GetAddressOf()))) {
        return;
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_brush;
    if (FAILED(target->CreateSolidColorBrush(theme_text_primary(), text_brush.GetAddressOf()))) {
        return;
    }
    const D2D1_RECT_F title_rect = D2D1::RectF(
        bounds.left + 16.0f, bounds.top + 12.0f, bounds.right - 16.0f, bounds.top + 40.0f);
    target->DrawTextW(title.c_str(), static_cast<UINT32>(title.size()), format.Get(), title_rect,
                      text_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                      DWRITE_MEASURING_MODE_NATURAL);
}

void Card::draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (target == nullptr || dwrite == nullptr) {
        return;
    }
    draw_background(target);
    draw_title(target, dwrite);
    Panel::draw(target, dwrite);
}

}  // namespace sscl::ui
