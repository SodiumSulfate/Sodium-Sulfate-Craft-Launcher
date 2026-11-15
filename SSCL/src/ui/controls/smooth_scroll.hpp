#pragma once

// 平滑滚动 + 滚动条控件（D-022；§8“滚动与惯性”，PCL MyScrollViewer/MyScrollBar 对照）。
// 目标式惯性：滚轮/拖拽更新目标位置，tick 以指数趋近（帧率无关，vsync 循环下丝滑）。
// 滚动条：右缘细条 + 圆角拇指，hover 变宽变深，拇指可拖拽、点击轨道跳转。

#include <d2d1.h>
#include <wrl/client.h>

namespace sscl::ui {

class SmoothScroll {
public:
    // 布局（DIP）：视口上下界、内容总高度、滚动条右缘 x。
    void layout(float view_top, float view_bottom, float content_height, float track_right_x);

    // 滚轮（delta：WHEEL_DELTA 单位，正=向上；step：一格滚动距离）。
    bool on_wheel(float delta, float step);

    // 指针交互（DIP 坐标）：返回是否有状态变化。
    bool on_mouse_down(float x, float y);
    bool on_mouse_move(float x, float y);
    bool on_mouse_up(float x, float y);
    bool on_mouse_leave();

    // 动画帧：滚动趋近 + thumb hover 渐变；返回是否仍在动画。
    bool tick(float dt_ms);

    // 绘制滚动条（无溢出时不画）。
    void draw(ID2D1RenderTarget* target) const;

    float offset() const { return m_scroll; }
    float max_offset() const;
    void jump_to(float offset);  // 立即定位（如回到顶部）

private:
    D2D1_RECT_F thumb_rect() const;
    D2D1_RECT_F track_rect() const;
    bool bar_hit(float x, float y) const;

    float m_scroll = 0.0f;   // 当前偏移
    float m_target = 0.0f;   // 目标偏移（滚轮/拖拽写入）
    float m_view_top = 0.0f;
    float m_view_bottom = 0.0f;
    float m_content = 0.0f;
    float m_track_right = 0.0f;

    bool m_dragging = false;
    float m_drag_grab = 0.0f;  // 抓取点距 thumb 顶
    bool m_bar_hovered = false;
    float m_hover_anim = 0.0f;
};

}  // namespace sscl::ui
