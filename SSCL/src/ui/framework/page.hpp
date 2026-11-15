#pragma once

// 页面接口（UI framework）：布局/绘制/鼠标事件；坐标为 DIP（由 D2D 唯一缩放）。

#include <d2d1.h>
#include <dwrite.h>
#include <functional>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sscl::ui {

class Page {
public:
    virtual ~Page() = default;

    virtual void layout(UINT width, UINT height, UINT dpi) = 0;
    virtual void draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) = 0;

    // 鼠标坐标为物理像素；页面自行转 DIP。返回是否有视觉变化（需重绘）。
    virtual bool mouse_move(float x, float y) = 0;
    virtual bool mouse_down(float x, float y) = 0;
    virtual bool mouse_up(float x, float y) = 0;
    // 右键（PCL 版本行右键=打开详情）；默认无操作。
    virtual bool mouse_right_down(float x, float y) {
        (void)x;
        (void)y;
        return false;
    }
    virtual bool mouse_leave() = 0;
    virtual bool mouse_wheel(float x, float y, float delta) {
        (void)x;
        (void)y;
        (void)delta;
        return false;
    }

    // 键盘（输入框聚焦时由外壳路由；默认无操作）。
    virtual bool handle_char(wchar_t ch) {
        (void)ch;
        return false;
    }
    virtual bool handle_key_down(unsigned long key) {
        (void)key;
        return false;
    }

    // 动画帧（D-020）：推进控件渐变；返回是否仍在动画（驱动定时器）。
    virtual bool tick(float dt_ms) {
        (void)dt_ms;
        return false;
    }

    // D-102：前台等待状态（获取版本列表 / 搜索 Mod / 解析前置 / 修复文件 / 导入整合包…）。
    // 外壳统一绘制转圈指示器、推进相位并保持帧循环；页面只需声明「我在等什么」。
    // 默认 false，未接线的页面不受影响。
    virtual bool is_busy() const { return false; }
    virtual std::wstring busy_text() const { return L"正在处理…"; }

    std::function<void()> request_repaint;
};

}  // namespace sscl::ui
