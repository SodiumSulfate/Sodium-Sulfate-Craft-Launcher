#pragma once

// 文本标签（DirectWrite）。
// D-026：构造时传入的主题色（TextPrimary/Secondary/Muted）标记为动态语义，
// 绘制时按当前主题解析 → 运行时切主题文字同步换色；其他具体颜色按原值使用。

#include <string>

#include "ui/framework/ui_element.hpp"

namespace sscl::ui {

class Label : public UiElement {
public:
    Label(std::wstring text, float font_size, D2D1_COLOR_F color, bool bold = false);

    void draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) override;

    std::wstring text;
    // 垂直居中（默认顶部对齐）。用于与图标/图表在同一行中间线对齐的场景。
    bool vertical_center = false;
    // 自动换行（默认关闭，保持所有既有单行/省略号行为）。长段说明文字
    // （如首次启动向导）设 true 并给足 bounds 高度，否则 NO_WRAP 会截断。
    bool word_wrap = false;

private:
    enum class Tone {
        kPrimary,
        kSecondary,
        kMuted,
        kCustom,
    };

    D2D1_COLOR_F resolve_color() const;

    float m_font_size;
    Tone m_tone = Tone::kCustom;
    D2D1_COLOR_F m_color;  // Tone::kCustom 时使用
    bool m_bold;
};

}  // namespace sscl::ui
