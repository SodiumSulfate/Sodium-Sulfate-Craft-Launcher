#pragma once

// 卡片容器（§8：卡片式布局、圆角、柔和阴影，对标 PCL MyCard）。
// 子元素用绝对 DIP 坐标布局（与 Panel 一致）；本控件负责背景/标题绘制。

#include <string>

#include "ui/framework/ui_element.hpp"

namespace sscl::ui {

class Card : public Panel {
public:
    explicit Card(std::wstring title = {});

    void draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) override;

    std::wstring title;
    float title_font_size = 15.0f;
    bool elevated = false;  // 弹窗/覆盖层：Elevated Surface + 边框（与主背景拉开层次）

private:
    void draw_background(ID2D1RenderTarget* target) const;
    void draw_title(ID2D1RenderTarget* target, IDWriteFactory* dwrite) const;
};

}  // namespace sscl::ui
