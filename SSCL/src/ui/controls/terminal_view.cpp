#include "ui/controls/terminal_view.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "ui/theme/theme.hpp"

namespace sscl::ui {

namespace {

constexpr float kLineHeight = 18.0f;
constexpr float kFontSize = 9.5f;
constexpr float kWheelStepLines = 3.0f;
constexpr float kScrollbarWidth = 3.0f;

D2D1_COLOR_F tone_color(TerminalTone tone) {
    switch (tone) {
        case TerminalTone::kDim:
            return theme_text_secondary();
        case TerminalTone::kAccent:
            return theme_primary();
        case TerminalTone::kSuccess:
            return theme_success();
        case TerminalTone::kError:
            return theme_danger();
        case TerminalTone::kDefault:
            break;
    }
    return theme_text_primary();
}

// 选第一个系统可用的等宽字体（DirectWrite 找不到族时不会报错但会回退到默认字体，
// 因此先显式检查，保证终端观感）。
std::wstring pick_mono_family(IDWriteFactory* dwrite) {
    Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
    if (FAILED(dwrite->GetSystemFontCollection(collection.GetAddressOf()))) {
        return L"Consolas";
    }
    const wchar_t* candidates[] = {L"Cascadia Mono", L"Consolas", L"Courier New"};
    for (const wchar_t* name : candidates) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(collection->FindFamilyName(name, &index, &exists)) && exists) {
            return name;
        }
    }
    return L"Consolas";
}

bool point_in_rect(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

}  // namespace

void TerminalView::clear() {
    m_lines.clear();
    m_trimmed = 0;
    m_scroll = 0.0f;
    m_target_scroll = 0.0f;
    m_follow = true;
}

void TerminalView::set_bounds(const D2D1_RECT_F& bounds) {
    m_bounds = bounds;
    // 视口尺寸变化后重新贴底/夹紧，避免停在越界偏移上显示空白。
    if (m_follow) {
        follow_bottom();
    } else {
        clamp_scroll();
    }
}

void TerminalView::append(std::wstring text, TerminalTone tone) {
    m_lines.push_back(Line{std::move(text), tone});
    while (m_lines.size() > m_max_lines) {
        m_lines.pop_front();
        ++m_trimmed;
    }
    if (m_follow) {
        m_target_scroll = max_scroll();
        m_scroll = m_target_scroll;  // 跟随模式直接贴底，不做动画（每帧日志都会触发）
    } else {
        clamp_scroll();
    }
}

void TerminalView::set_max_lines(std::size_t max_lines) {
    m_max_lines = (std::max)(static_cast<std::size_t>(16), max_lines);
    trim_to(m_max_lines);
}

void TerminalView::trim_to(std::size_t keep) {
    const std::size_t target = (std::min)(keep, m_lines.size());
    while (m_lines.size() > target) {
        m_lines.pop_front();
        ++m_trimmed;
    }
    m_follow = true;
    m_target_scroll = max_scroll();
    m_scroll = m_target_scroll;
}

float TerminalView::content_height() const {
    return static_cast<float>(m_lines.size()) * kLineHeight;
}

float TerminalView::max_scroll() const {
    const float view = m_bounds.bottom - m_bounds.top;
    return (std::max)(0.0f, content_height() - (std::max)(0.0f, view));
}

void TerminalView::clamp_scroll() {
    const float limit = max_scroll();
    if (m_target_scroll < 0.0f) {
        m_target_scroll = 0.0f;
    }
    if (m_target_scroll > limit) {
        m_target_scroll = limit;
    }
    if (m_scroll > limit) {
        m_scroll = limit;
    }
    if (m_scroll < 0.0f) {
        m_scroll = 0.0f;
    }
}

void TerminalView::follow_bottom() {
    m_follow = true;
    m_target_scroll = max_scroll();
    m_scroll = m_target_scroll;
}

bool TerminalView::on_wheel(float delta) {
    if (max_scroll() <= 0.0f || delta == 0.0f) {
        return false;
    }
    const float step = kWheelStepLines * kLineHeight;
    if (delta > 0.0f) {
        m_target_scroll -= step;
        m_follow = false;
    } else {
        m_target_scroll += step;
    }
    if (m_target_scroll < 0.0f) {
        m_target_scroll = 0.0f;
    }
    const float limit = max_scroll();
    if (m_target_scroll >= limit - 0.5f) {
        m_target_scroll = limit;
        m_follow = true;
    }
    return true;
}

bool TerminalView::tick(float dt_ms) {
    const float diff = m_target_scroll - m_scroll;
    if (std::fabs(diff) < 0.5f) {
        m_scroll = m_target_scroll;
        return false;
    }
    // 帧率无关的指数趋近（约 80ms 收敛一半），滚轮滚动才产生动画。
    const float factor = 1.0f - std::exp(-dt_ms / 80.0f);
    m_scroll += diff * factor;
    return true;
}

bool TerminalView::hit(float x, float y) const {
    return point_in_rect(m_bounds, x, y);
}

void TerminalView::draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (target == nullptr || dwrite == nullptr) {
        return;
    }
    if (m_bounds.right <= m_bounds.left || m_bounds.bottom <= m_bounds.top) {
        return;
    }

    if (m_format == nullptr || m_format_factory != dwrite) {
        if (m_font_family.empty()) {
            m_font_family = pick_mono_family(dwrite);
        }
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        if (FAILED(dwrite->CreateTextFormat(
                m_font_family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, kFontSize, L"zh-cn",
                format.GetAddressOf()))) {
            return;
        }
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        m_format = format;
        m_format_factory = dwrite;
    }

    target->PushAxisAlignedClip(m_bounds, D2D1_ANTIALIAS_MODE_ALIASED);

    const std::size_t first_line =
        static_cast<std::size_t>((std::max)(0.0f, m_scroll) / kLineHeight);
    const float offset = m_scroll - static_cast<float>(first_line) * kLineHeight;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushes[5];
    const TerminalTone tones[5] = {TerminalTone::kDefault, TerminalTone::kDim,
                                   TerminalTone::kAccent, TerminalTone::kSuccess,
                                   TerminalTone::kError};
    for (int i = 0; i < 5; ++i) {
        target->CreateSolidColorBrush(tone_color(tones[i]), brushes[i].GetAddressOf());
    }

    float y = m_bounds.top - offset;
    for (std::size_t index = first_line; index < m_lines.size(); ++index) {
        if (y >= m_bounds.bottom) {
            break;
        }
        if (y + kLineHeight >= m_bounds.top) {
            const Line& line = m_lines[index];
            ID2D1SolidColorBrush* brush = brushes[0].Get();
            switch (line.tone) {
                case TerminalTone::kDim:
                    brush = brushes[1].Get();
                    break;
                case TerminalTone::kAccent:
                    brush = brushes[2].Get();
                    break;
                case TerminalTone::kSuccess:
                    brush = brushes[3].Get();
                    break;
                case TerminalTone::kError:
                    brush = brushes[4].Get();
                    break;
                case TerminalTone::kDefault:
                    break;
            }
            if (brush != nullptr) {
                const D2D1_RECT_F line_rect = D2D1::RectF(m_bounds.left, y, m_bounds.right,
                                                          y + kLineHeight);
                target->DrawTextW(line.text.c_str(), static_cast<UINT32>(line.text.size()),
                                  m_format.Get(), line_rect, brush,
                                  D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            }
        }
        y += kLineHeight;
    }

    // 滚动指示条（不占布局宽度，半透明细条）。
    const float limit = max_scroll();
    if (limit > 0.0f) {
        const float view_height = m_bounds.bottom - m_bounds.top;
        const float thumb_height =
            (std::max)(24.0f, view_height * view_height / content_height());
        const float travel = (std::max)(0.0f, view_height - thumb_height);
        const float thumb_top = m_bounds.top + (m_scroll / limit) * travel;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> thumb;
        if (SUCCEEDED(target->CreateSolidColorBrush(theme_border(), thumb.GetAddressOf()))) {
            thumb->SetOpacity(0.9f);
            const D2D1_RECT_F rect = D2D1::RectF(m_bounds.right - kScrollbarWidth,
                                                 thumb_top, m_bounds.right,
                                                 thumb_top + thumb_height);
            target->FillRoundedRectangle(
                D2D1::RoundedRect(rect, kScrollbarWidth * 0.5f, kScrollbarWidth * 0.5f),
                thumb.Get());
        }
    }

    target->PopAxisAlignedClip();
}

}  // namespace sscl::ui
