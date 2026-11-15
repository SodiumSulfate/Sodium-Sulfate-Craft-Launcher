#pragma once

// 轻量悬浮提示：延迟显示、跟随鼠标、自动贴边。
// 由页面在 mouse_move 中 update()，tick() 推进延迟，draw() 最后绘制。

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>

namespace sscl::ui {

class Tooltip {
public:
    // 返回 true 表示提示内容发生变化（需要重绘）。
    bool update(std::wstring text, const D2D1_RECT_F& anchor, float mouse_x, float mouse_y);
    void clear();
    bool visible() const { return m_visible; }
    bool tick(float dt_ms);
    void draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite, float width, float height) const;

private:
    std::wstring m_text;
    D2D1_RECT_F m_anchor{};
    float m_mouse_x = 0.0f;
    float m_mouse_y = 0.0f;
    float m_elapsed = 0.0f;
    bool m_visible = false;
    static constexpr float kDelayMs = 320.0f;
};

}  // namespace sscl::ui
