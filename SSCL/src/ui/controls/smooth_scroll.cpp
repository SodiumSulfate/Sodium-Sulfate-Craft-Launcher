#include "ui/controls/smooth_scroll.hpp"

#include <algorithm>
#include <cmath>

#include "ui/framework/motion.hpp"
#include "ui/theme/theme.hpp"

namespace sscl::ui {

namespace {

constexpr float kWheelDelta = 120.0f;
constexpr float kMinThumbHeight = 28.0f;
constexpr float kBarWidth = 6.0f;
constexpr float kBarWidthHover = 10.0f;
constexpr float kBarGap = 2.0f;  // 滚动条距内容右缘

float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

}  // namespace

void SmoothScroll::layout(float view_top, float view_bottom, float content_height,
                          float track_right_x) {
    m_view_top = view_top;
    m_view_bottom = view_bottom;
    m_content = content_height;
    m_track_right = track_right_x;
    const float max = max_offset();
    if (m_target > max) {
        m_target = max;
    }
    if (m_scroll > max) {
        m_scroll = max;
    }
}

float SmoothScroll::max_offset() const {
    const float viewport = m_view_bottom - m_view_top;
    return (std::max)(0.0f, m_content - viewport);
}

void SmoothScroll::jump_to(float offset) {
    const float max = max_offset();
    m_target = (std::min)(max, (std::max)(0.0f, offset));
    m_scroll = m_target;
}

bool SmoothScroll::on_wheel(float delta, float step) {
    const float max = max_offset();
    if (max <= 0.0f) {
        return false;
    }
    const float next =
        (std::min)(max, (std::max)(0.0f, m_target - (delta / kWheelDelta) * step));
    if (next == m_target) {
        return false;
    }
    m_target = next;
    return true;
}

D2D1_RECT_F SmoothScroll::track_rect() const {
    const float width = kBarWidth + (kBarWidthHover - kBarWidth) * m_hover_anim;
    return D2D1::RectF(m_track_right - kBarGap - width, m_view_top, m_track_right - kBarGap,
                       m_view_bottom);
}

D2D1_RECT_F SmoothScroll::thumb_rect() const {
    const float viewport = m_view_bottom - m_view_top;
    const float max = max_offset();
    float thumb_height = viewport;
    if (m_content > 0.0f) {
        thumb_height = viewport * (viewport / m_content);
    }
    thumb_height = (std::max)(kMinThumbHeight, (std::min)(viewport, thumb_height));
    float thumb_top = m_view_top;
    if (max > 0.0f) {
        thumb_top = m_view_top + (m_scroll / max) * (viewport - thumb_height);
    }
    const D2D1_RECT_F track = track_rect();
    return D2D1::RectF(track.left, thumb_top, track.right, thumb_top + thumb_height);
}

bool SmoothScroll::bar_hit(float x, float y) const {
    if (max_offset() <= 0.0f) {
        return false;
    }
    const D2D1_RECT_F track = track_rect();
    // 命中区比可见条略宽（易点）
    return x >= track.left - 4.0f && x <= track.right + 2.0f && y >= track.top &&
           y <= track.bottom;
}

bool SmoothScroll::on_mouse_down(float x, float y) {
    if (max_offset() <= 0.0f || !bar_hit(x, y)) {
        return false;
    }
    const D2D1_RECT_F thumb = thumb_rect();
    if (y >= thumb.top && y <= thumb.bottom) {
        m_dragging = true;
        m_drag_grab = y - thumb.top;
    } else {
        // 点击轨道：视口中心跳转
        const float viewport = m_view_bottom - m_view_top;
        const float max = max_offset();
        const float rel = clamp01((y - m_view_top - (thumb.bottom - thumb.top) * 0.5f) /
                                  (std::max)(1.0f, viewport - (thumb.bottom - thumb.top)));
        m_target = rel * max;
    }
    return true;
}

bool SmoothScroll::on_mouse_move(float x, float y) {
    bool changed = false;
    if (m_dragging) {
        const float viewport = m_view_bottom - m_view_top;
        const D2D1_RECT_F thumb = thumb_rect();
        const float thumb_height = thumb.bottom - thumb.top;
        const float max = max_offset();
        const float rel = clamp01((y - m_drag_grab - m_view_top) /
                                  (std::max)(1.0f, viewport - thumb_height));
        const float next = rel * max;
        if (next != m_target) {
            m_target = next;
            m_scroll = next;  // 拖拽即时跟手
            changed = true;
        }
        return changed;
    }
    const bool hovered = bar_hit(x, y);
    if (hovered != m_bar_hovered) {
        m_bar_hovered = hovered;
        changed = true;
    }
    return changed;
}

bool SmoothScroll::on_mouse_up(float x, float y) {
    (void)x;
    (void)y;
    const bool was = m_dragging;
    m_dragging = false;
    return was;
}

bool SmoothScroll::on_mouse_leave() {
    bool changed = false;
    if (m_bar_hovered) {
        m_bar_hovered = false;
        changed = true;
    }
    return changed;
}

bool SmoothScroll::tick(float dt_ms) {
    bool active = false;
    // 指数趋近（帧率无关平滑；D-022 丝滑滚动）
    const float max = max_offset();
    if (m_target > max) {
        m_target = max;
    }
    if (!m_dragging && std::fabs(m_target - m_scroll) > 0.05f) {
        const float t = 1.0f - std::exp(-dt_ms / 65.0f);
        m_scroll += (m_target - m_scroll) * t;
        if (std::fabs(m_target - m_scroll) <= 0.05f) {
            m_scroll = m_target;
        }
        active = true;
    }
    // thumb hover 渐变（统一走 motion，随「动效强度」缩放）
    if (motion::tween(m_hover_anim, m_bar_hovered ? 1.0f : 0.0f, dt_ms,
                      motion::Kind::kScrollbar)) {
        active = true;
    }
    return active;
}

void SmoothScroll::draw(ID2D1RenderTarget* target) const {
    if (target == nullptr || max_offset() <= 0.0f) {
        return;
    }
    const D2D1_RECT_F thumb = thumb_rect();

    // 轨道（极淡）
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> track_brush;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_border(), track_brush.GetAddressOf()))) {
        track_brush->SetOpacity(0.25f + 0.15f * m_hover_anim);
        const D2D1_RECT_F track = track_rect();
        target->FillRoundedRectangle(
            D2D1::RoundedRect(track, track.right - track.left, track.right - track.left),
            track_brush.Get());
    }

    // 拇指（hover 变深）
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> thumb_brush;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_text_secondary(),
                                                thumb_brush.GetAddressOf()))) {
        thumb_brush->SetOpacity(0.45f + 0.25f * m_hover_anim);
        const float radius = (thumb.right - thumb.left) * 0.5f;
        target->FillRoundedRectangle(D2D1::RoundedRect(thumb, radius, radius), thumb_brush.Get());
    }
}

}  // namespace sscl::ui
