#include "ui/controls/label.hpp"

#include <cmath>
#include <utility>

#include "ui/theme/theme.hpp"

namespace sscl::ui {

namespace {

bool same_rgb(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) {
    return std::fabs(a.r - b.r) < 0.002f && std::fabs(a.g - b.g) < 0.002f &&
           std::fabs(a.b - b.b) < 0.002f;
}

}  // namespace

Label::Label(std::wstring text, float font_size, D2D1_COLOR_F color, bool bold)
    : text(std::move(text)), m_font_size(font_size), m_color(color), m_bold(bold) {
    // D-026：主题色标记为动态（调用点全部传 theme_text_*()，构造时比对命中即
    // 按语义在绘制时解析 → 运行时切主题文字同步换色，不留旧配色）。
    if (same_rgb(color, theme_text_primary())) {
        m_tone = Tone::kPrimary;
    } else if (same_rgb(color, theme_text_secondary())) {
        m_tone = Tone::kSecondary;
    } else if (same_rgb(color, theme_text_muted())) {
        m_tone = Tone::kMuted;
    } else {
        m_tone = Tone::kCustom;
    }
}

D2D1_COLOR_F Label::resolve_color() const {
    switch (m_tone) {
        case Tone::kPrimary:
            return theme_text_primary();
        case Tone::kSecondary:
            return theme_text_secondary();
        case Tone::kMuted:
            return theme_text_muted();
        case Tone::kCustom:
            break;
    }
    return m_color;
}

void Label::draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (target == nullptr || dwrite == nullptr || text.empty()) {
        return;
    }

    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (FAILED(dwrite->CreateTextFormat(theme_font_family(), nullptr,
                                        m_bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        m_font_size, L"zh-cn", format.GetAddressOf()))) {
        return;
    }
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(vertical_center ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
                                                  : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    format->SetWordWrapping(word_wrap ? DWRITE_WORD_WRAPPING_WRAP
                                      : DWRITE_WORD_WRAPPING_NO_WRAP);
    // D-054：单行文本按控件宽度自动省略。此前只靠调用点"猜字符数"截断，
    // 字体/DPI/侧栏宽度一变仍会越界；这里用 DirectWrite trimming 从根本上保证不出界。
    DWRITE_TRIMMING trimming{};
    trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(format.Get(), ellipsis.GetAddressOf()))) {
        format->SetTrimming(&trimming, ellipsis.Get());
    }

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(resolve_color(), brush.GetAddressOf()))) {
        return;
    }

    target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), bounds,
                      brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
}

}  // namespace sscl::ui
