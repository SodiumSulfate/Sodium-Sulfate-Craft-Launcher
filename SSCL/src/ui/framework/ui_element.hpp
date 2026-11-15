#pragma once

// 自绘控件基类（§8 framework：布局、事件、命中测试）。
// 坐标为客户区物理像素；控件自行按 DPI 缩放尺寸。

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/framework/motion.hpp"

namespace sscl::ui {

class UiElement {
public:
    virtual ~UiElement() = default;

    virtual void draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) = 0;

    virtual bool hit_test(float x, float y) const {
        if (!visible) {
            return false;
        }
        return x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom;
    }

    virtual void on_mouse_enter() { hovered = enabled; }
    virtual void on_mouse_leave() {
        hovered = false;
        pressed = false;
    }
    virtual void on_mouse_move(float x, float y) {
        (void)x;
        (void)y;
    }
    virtual void on_mouse_down(float x, float y) {
        if (enabled && hit_test(x, y)) {
            pressed = true;
        }
    }
    virtual void on_mouse_up(float x, float y) {
        if (pressed && enabled && hit_test(x, y) && on_click) {
            on_click();
        }
        pressed = false;
    }

    D2D1_RECT_F bounds{0.0f, 0.0f, 0.0f, 0.0f};
    bool hovered = false;
    bool pressed = false;
    bool enabled = true;
    bool visible = true;          // 不可见：不绘制、不命中（D-028）
    float hover_anim = 0.0f;  // 悬停渐变 0..1（D-020 操作动画；draw 用其混合颜色）
    std::function<void()> on_click;

    // 动画帧（D-020 / D-081）：推进 hover 渐变；返回是否仍在动画（驱动定时器）。
    // 时长与缓动统一由 motion 模块给出，随「动效强度」缩放。
    virtual bool tick(float dt_ms) {
        const float target = (hovered && enabled) ? 1.0f : 0.0f;
        return motion::tween(hover_anim, target, dt_ms, motion::Kind::kHover);
    }
};

// 顺序容器：事件按子元素倒序（顶层优先），绘制按正序。
class Panel : public UiElement {
public:
    void add(std::unique_ptr<UiElement> child) {
        children.push_back(std::move(child));
    }

    void draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) override {
        for (auto& child : children) {
            if (child->visible) {
                child->draw(target, dwrite);
            }
        }
    }

    void on_mouse_move(float x, float y) override {
        if (!visible) {
            return;  // 不可见容器不路由命中/悬停（弹窗淡出期不接受交互）
        }
        for (auto& child : children) {
            const bool inside = child->hit_test(x, y);
            if (inside && !child->hovered) {
                child->on_mouse_enter();
                state_changed = true;
            } else if (!inside && child->hovered) {
                child->on_mouse_leave();
                state_changed = true;
            }
            child->on_mouse_move(x, y);  // 嵌套容器（Card）继续路由
        }
    }

    void on_mouse_leave() override {
        for (auto& child : children) {
            if (child->hovered) {
                child->on_mouse_leave();
                state_changed = true;
            }
        }
    }

    bool tick(float dt_ms) override {
        bool active = UiElement::tick(dt_ms);
        for (auto& child : children) {
            if (child->tick(dt_ms)) {
                active = true;
            }
        }
        return active;
    }

    void on_mouse_down(float x, float y) override {
        if (!visible) {
            return;
        }
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            if ((*it)->hit_test(x, y)) {
                (*it)->on_mouse_down(x, y);
                state_changed = true;
                return;
            }
        }
    }

    void on_mouse_up(float x, float y) override {
        if (!visible) {
            return;
        }
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            if ((*it)->pressed || (*it)->hit_test(x, y)) {
                (*it)->on_mouse_up(x, y);
                state_changed = true;
                return;
            }
        }
    }

    std::vector<std::unique_ptr<UiElement>> children;
    bool state_changed = false;  // mouse 事件后供上层决定是否重绘
};

}  // namespace sscl::ui
