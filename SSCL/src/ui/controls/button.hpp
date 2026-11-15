#pragma once

// 圆角按钮（hover/pressed 态，§8 controls）。

#include <string>

#include "ui/framework/ui_element.hpp"

namespace sscl::ui {

class Button : public UiElement {
public:
    Button(std::wstring text, float font_size, bool primary = true);

    void draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) override;

    std::wstring text;
    bool selected = false;  // 选中态（分类/导航项，D-036：Hover Background + 强调色文字）
    std::string icon;       // Material Symbols 名称（空 = 纯文字）
    // D-085：可选图标独立配色（版本类型按钮）。false 时图标跟随文字色。
    // 语义色须在绘制前由调用方刷新，避免主题切换后残留旧色。
    bool custom_icon_color = false;
    D2D1_COLOR_F icon_color{};

private:
    float m_font_size;
    bool m_primary;
};

}  // namespace sscl::ui
