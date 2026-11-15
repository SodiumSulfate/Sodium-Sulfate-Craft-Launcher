#pragma once

// D3D11 + DXGI flip model + Direct2D 渲染目标（开发文档 §4.1/§8；DECISIONS D-004）。
// 空闲不重绘：仅在 WM_PAINT（invalidate 后）绘制并 Present(1, 0)（vsync）。
// D-016 磨砂玻璃：优先 DirectComposition + premultiplied alpha 交换链（透明清屏，
// 系统磨砂背板透出）；DComp 不可用时回退 HWND 交换链（不透明清屏）。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <functional>

#include "util/result.hpp"

namespace sscl::ui {

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // 为 hwnd 创建 D3D11 设备 + FLIP 交换链 + D2D 渲染目标。
    util::Result<void> initialize(HWND hwnd, UINT dpi);

    // 客户区尺寸变化后重建交换链后台缓冲与渲染目标。
    void resize(UINT width, UINT height);

    // 绘制一帧：BeginDraw → 清屏 → paint 回调 → EndDraw → Present（仅由 WM_PAINT 驱动）。
    // paint 内的 D2D 绘制必须在此回调中执行（D2D 要求 BeginDraw/EndDraw 会话内）。
    using PaintFn = std::function<void(ID2D1RenderTarget* target, IDWriteFactory* dwrite)>;
    void render_frame(const PaintFn& paint);

    void shutdown();
    bool initialized() const;

    // UI 绘制接入点（控件在 on_paint 回调里使用）。
    ID2D1RenderTarget* target() const { return m_target.Get(); }
    IDWriteFactory* dwrite_factory() const { return m_dwrite_factory.Get(); }
    UINT dpi() const { return m_dpi; }

private:
    util::Result<void> create_render_target();
    util::Result<void> create_device_and_swap_chain(HWND hwnd);

    HWND m_hwnd = nullptr;
    UINT m_dpi = 96;
    UINT m_width = 0;
    UINT m_height = 0;
    bool m_alpha_mode = false;  // true = DComp 透明交换链（玻璃模式）

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swap_chain;
    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2d_factory;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwrite_factory;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> m_target;
    Microsoft::WRL::ComPtr<IDCompositionDevice> m_dcomp_device;
    Microsoft::WRL::ComPtr<IDCompositionTarget> m_dcomp_target;
    Microsoft::WRL::ComPtr<IDCompositionVisual> m_dcomp_visual;
};

}  // namespace sscl::ui
