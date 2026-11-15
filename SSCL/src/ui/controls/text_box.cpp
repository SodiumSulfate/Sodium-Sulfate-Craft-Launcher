#include "ui/controls/text_box.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <utility>

#include "platform/shell_utils.hpp"
#include "ui/theme/theme.hpp"
#include "util/string_utils.hpp"

namespace sscl::ui {

namespace {
constexpr unsigned long kKeyBack = 0x08;    // VK_BACK
constexpr unsigned long kKeyReturn = 0x0D;  // VK_RETURN
}  // namespace

TextBox::TextBox(std::wstring text, float font_size)
    : text(std::move(text)), m_font_size(font_size) {}

void TextBox::draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (target == nullptr || dwrite == nullptr) {
        return;
    }

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> surface;
    if (FAILED(target->CreateSolidColorBrush(theme_surface(), surface.GetAddressOf()))) {
        return;
    }
    const D2D1_ROUNDED_RECT rounded =
        D2D1::RoundedRect(bounds, theme_radius(), theme_radius());
    target->FillRoundedRectangle(rounded, surface.Get());

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border_brush;
    if (SUCCEEDED(target->CreateSolidColorBrush(m_focused ? theme_focus() : theme_border(),
                                                border_brush.GetAddressOf()))) {
        target->DrawRoundedRectangle(rounded, border_brush.Get(), m_focused ? 1.5f : 1.0f);
    }

    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (FAILED(dwrite->CreateTextFormat(theme_font_family(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        m_font_size, L"zh-cn", format.GetAddressOf()))) {
        return;
    }
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    const D2D1_RECT_F text_rect =
        D2D1::RectF(bounds.left + 10.0f, bounds.top, bounds.right - 10.0f, bounds.bottom);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_brush;
    if (FAILED(target->CreateSolidColorBrush(theme_text_primary(), text_brush.GetAddressOf()))) {
        return;
    }
    if (text.empty() && !placeholder.empty() && !m_focused) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hint_brush;
        if (FAILED(target->CreateSolidColorBrush(theme_text_muted(), hint_brush.GetAddressOf()))) {
            return;
        }
        target->DrawTextW(placeholder.c_str(), static_cast<UINT32>(placeholder.size()),
                          format.Get(), text_rect, hint_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                          DWRITE_MEASURING_MODE_NATURAL);
        return;
    }

    const std::wstring display =
        password ? std::wstring(text.size(), L'\u25CF') : text;  // D-118：密码掩码
    target->DrawTextW(display.c_str(), static_cast<UINT32>(display.size()), format.Get(), text_rect,
                      text_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                      DWRITE_MEASURING_MODE_NATURAL);

    // 尾部光标（静态竖线，§8 框架完善后再做闪烁与移动）。
    if (m_focused) {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        const float max_width = text_rect.right - text_rect.left;
        if (SUCCEEDED(dwrite->CreateTextLayout(display.c_str(), static_cast<UINT32>(display.size()),
                                               format.Get(), max_width, bounds.bottom - bounds.top,
                                               layout.GetAddressOf()))) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                const float caret_x = text_rect.left + metrics.widthIncludingTrailingWhitespace;
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> caret_brush;
                if (SUCCEEDED(target->CreateSolidColorBrush(theme_text_primary(),
                                                            caret_brush.GetAddressOf()))) {
                    target->DrawLine(D2D1::Point2F(caret_x, bounds.top + 7.0f),
                                     D2D1::Point2F(caret_x, bounds.bottom - 7.0f),
                                     caret_brush.Get(), 1.2f);
                }
            }
        }
    }
}

bool TextBox::handle_char(wchar_t ch) {
    if (!m_focused) {
        return false;
    }
    if (ch < 32 || ch == 0x7F) {
        return false;  // 控制字符
    }
    if (text.size() >= max_length) {
        return false;
    }
    text.push_back(ch);
    return true;
}

bool TextBox::handle_key_down(unsigned long key) {
    if (!m_focused) {
        return false;
    }
    // D-120：Ctrl 组合键。WM_KEYDOWN 先于 WM_CHAR 到达，此时 Ctrl 仍是按下状态，
    // 直接用 GetKeyState 读取即可（不引入额外的修饰键状态机）。
    if ((::GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        switch (key) {
            case 'V':
                return paste_from_clipboard();
            case 'C':
                return copy_to_clipboard();
            case 'X':
                return cut_to_clipboard();
            default:
                break;
        }
    }
    if (key == kKeyBack && !text.empty()) {
        text.pop_back();
        return true;
    }
    (void)kKeyReturn;
    return false;
}

// 单行框的内容清洗：控制字符（换行/制表符/退格…）一律丢弃，
// 并按剩余可容纳长度截断——粘贴与程序化填充共用这一条规则。
bool TextBox::insert_text(const std::wstring& value) {
    if (value.empty()) {
        return false;
    }
    if (text.size() >= max_length) {
        return false;
    }
    const std::size_t remaining = max_length - text.size();
    std::wstring filtered;
    filtered.reserve(remaining < value.size() ? remaining : value.size());
    for (const wchar_t ch : value) {
        if (ch < 32 || ch == 0x7F) {
            continue;
        }
        filtered.push_back(ch);
        if (filtered.size() >= remaining) {
            break;
        }
    }
    if (filtered.empty()) {
        return false;
    }
    text += filtered;
    return true;
}

bool TextBox::paste_from_clipboard() {
    if (!m_focused) {
        return false;
    }
    const std::string utf8 = sscl::platform::get_clipboard_text();
    if (utf8.empty()) {
        return false;
    }
    return insert_text(sscl::util::utf8_to_wide(utf8));
}

bool TextBox::copy_to_clipboard() const {
    // 密码框不导出明文：与系统编辑控件一致，复制/剪切一律拒绝。
    if (!m_focused || password || text.empty()) {
        return false;
    }
    return sscl::platform::set_clipboard_text(sscl::util::wide_to_utf8(text));
}

bool TextBox::cut_to_clipboard() {
    if (!m_focused || password || text.empty()) {
        return false;
    }
    if (!sscl::platform::set_clipboard_text(sscl::util::wide_to_utf8(text))) {
        return false;
    }
    text.clear();
    return true;
}

}  // namespace sscl::ui
