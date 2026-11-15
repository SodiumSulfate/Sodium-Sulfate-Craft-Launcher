#include "ui/framework/renderer.hpp"

#include <cstdio>

#include "ui/theme/theme.hpp"
#include "util/log.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dcomp.lib")

namespace sscl::ui {

namespace {

std::string format_hresult(HRESULT hr) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "HRESULT 0x%08X", static_cast<unsigned int>(hr));
    return buffer;
}

constexpr D3D_FEATURE_LEVEL kFeatureLevels[] = {
    D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0,
};

constexpr UINT kFeatureLevelCount =
    static_cast<UINT>(sizeof(kFeatureLevels) / sizeof(kFeatureLevels[0]));

}  // namespace

Renderer::~Renderer() {
    shutdown();
}

bool Renderer::initialized() const {
    return m_target != nullptr;
}

util::Result<void> Renderer::initialize(HWND hwnd, UINT dpi) {
    m_hwnd = hwnd;
    m_dpi = (dpi == 0) ? 96 : dpi;

    util::Result<void> created = create_device_and_swap_chain(hwnd);
    if (created.is_error()) {
        return created;
    }
    return create_render_target();
}

util::Result<void> Renderer::create_device_and_swap_chain(HWND hwnd) {
    D3D_FEATURE_LEVEL created_level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, kFeatureLevels,
                                     kFeatureLevelCount, D3D11_SDK_VERSION,
                                     m_device.ReleaseAndGetAddressOf(), &created_level, nullptr);
    if (FAILED(hr)) {
        // 无硬件设备（如部分远程环境）时退回 WARP 软件光栅器。
        hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, kFeatureLevels,
                                 kFeatureLevelCount, D3D11_SDK_VERSION,
                                 m_device.ReleaseAndGetAddressOf(), &created_level, nullptr);
    }
    if (FAILED(hr)) {
        return util::Result<void>::fail(util::Error::make(
            "ui.d3d_device_failed", "无法创建图形设备（Direct3D 11）", format_hresult(hr)));
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    hr = m_device.As(&dxgi_device);
    if (FAILED(hr)) {
        return util::Result<void>::fail(util::Error::make(
            "ui.d3d_device_failed", "图形设备不支持 DXGI 互操作", format_hresult(hr)));
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) {
        return util::Result<void>::fail(util::Error::make(
            "ui.d3d_device_failed", "无法获取图形适配器", format_hresult(hr)));
    }

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        return util::Result<void>::fail(util::Error::make(
            "ui.d3d_device_failed", "无法获取 DXGI 工厂", format_hresult(hr)));
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 0;  // 0 = 使用客户区尺寸（HWND 交换链）
    desc.Height = 0;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;  // D-004：flip model

    // D-016 玻璃模式：DirectComposition + premultiplied alpha（透明像素透出磨砂背板）。
    RECT client{};
    ::GetClientRect(hwnd, &client);
    desc.Width = static_cast<UINT>(client.right - client.left);
    desc.Height = static_cast<UINT>(client.bottom - client.top);
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    hr = factory->CreateSwapChainForComposition(m_device.Get(), &desc, nullptr,
                                                m_swap_chain.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = ::DCompositionCreateDevice(dxgi_device.Get(),
                                        IID_PPV_ARGS(m_dcomp_device.ReleaseAndGetAddressOf()));
        if (SUCCEEDED(hr)) {
            hr = m_dcomp_device->CreateTargetForHwnd(hwnd, TRUE,
                                                     m_dcomp_target.ReleaseAndGetAddressOf());
        }
        if (SUCCEEDED(hr)) {
            hr = m_dcomp_device->CreateVisual(m_dcomp_visual.ReleaseAndGetAddressOf());
        }
        if (SUCCEEDED(hr)) {
            hr = m_dcomp_visual->SetContent(m_swap_chain.Get());
        }
        if (SUCCEEDED(hr)) {
            hr = m_dcomp_target->SetRoot(m_dcomp_visual.Get());
        }
        if (SUCCEEDED(hr)) {
            hr = m_dcomp_device->Commit();
        }
        if (SUCCEEDED(hr)) {
            m_alpha_mode = true;
            util::log_info("[ui] renderer: DirectComposition transparent swap chain");
        } else {
            // DComp 部分失败：弃用组合交换链，回退普通 HWND 交换链。
            m_dcomp_visual.Reset();
            m_dcomp_target.Reset();
            m_dcomp_device.Reset();
            m_swap_chain.Reset();
        }
    }

    if (!m_alpha_mode) {
        desc.Width = 0;
        desc.Height = 0;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        hr = factory->CreateSwapChainForHwnd(m_device.Get(), hwnd, &desc, nullptr, nullptr,
                                             m_swap_chain.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            return util::Result<void>::fail(util::Error::make(
                "ui.swapchain_failed", "无法创建交换链（DXGI flip model）", format_hresult(hr)));
        }
        util::log_info("[ui] renderer: opaque HWND swap chain (backdrop degrade)");
    }
    // 窗口模式固定，禁止 Alt+Enter 切换独占全屏。
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                             m_d2d_factory.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        return util::Result<void>::fail(util::Error::make(
            "ui.d2d_failed", "无法创建 Direct2D 工厂", format_hresult(hr)));
    }

    hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                               reinterpret_cast<IUnknown**>(
                                   m_dwrite_factory.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        return util::Result<void>::fail(util::Error::make(
            "ui.dwrite_failed", "无法创建 DirectWrite 工厂", format_hresult(hr)));
    }

    m_width = static_cast<UINT>(client.right - client.left);
    m_height = static_cast<UINT>(client.bottom - client.top);
    return util::Result<void>::ok();
}

util::Result<void> Renderer::create_render_target() {
    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    HRESULT hr = m_swap_chain->GetBuffer(0, IID_PPV_ARGS(surface.GetAddressOf()));
    if (FAILED(hr)) {
        return util::Result<void>::fail(util::Error::make(
            "ui.render_target_failed", "无法获取交换链后台缓冲", format_hresult(hr)));
    }

    const D2D1_RENDER_TARGET_PROPERTIES props =
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                      m_alpha_mode ? D2D1_ALPHA_MODE_PREMULTIPLIED
                                                                   : D2D1_ALPHA_MODE_IGNORE),
                                     static_cast<FLOAT>(m_dpi), static_cast<FLOAT>(m_dpi));
    hr = m_d2d_factory->CreateDxgiSurfaceRenderTarget(surface.Get(), &props,
                                                      m_target.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        return util::Result<void>::fail(util::Error::make(
            "ui.render_target_failed", "无法创建 Direct2D 渲染目标", format_hresult(hr)));
    }
    return util::Result<void>::ok();
}

void Renderer::resize(UINT width, UINT height) {
    if (m_swap_chain == nullptr || width == 0 || height == 0) {
        return;
    }
    if (width == m_width && height == m_height && m_target != nullptr) {
        return;
    }

    m_target.Reset();

    // DComp 视觉持有后台缓冲引用时 ResizeBuffers 会失败（2026-10-24 缺陷根因之一）：
    // 先解绑内容，成功后重绑并提交。
    const bool rebind = m_dcomp_visual != nullptr;
    if (rebind) {
        m_dcomp_visual->SetContent(nullptr);
    }

    HRESULT hr = m_swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        const std::string detail = format_hresult(hr);
        util::log_error("[ui] ResizeBuffers failed: " + detail + "; recreating swap chain");

        // 最终兜底：整链重建（设备/缓冲引用异常）。
        m_target.Reset();
        m_swap_chain.Reset();
        m_dcomp_visual.Reset();
        m_dcomp_target.Reset();
        m_dcomp_device.Reset();
        m_device.Reset();
        util::Result<void> rebuilt = create_device_and_swap_chain(m_hwnd);
        if (rebuilt.is_error()) {
            util::log_error("[ui] recreate swap chain failed: " + rebuilt.error().detail);
            return;
        }
        util::Result<void> target = create_render_target();
        if (target.is_error()) {
            util::log_error("[ui] recreate render target failed: " + target.error().detail);
        }
        return;
    }

    if (rebind && m_dcomp_visual != nullptr) {
        m_dcomp_visual->SetContent(m_swap_chain.Get());
        m_dcomp_device->Commit();
    }
    m_width = width;
    m_height = height;

    util::Result<void> created = create_render_target();
    if (created.is_error()) {
        const std::string detail = created.error().detail;
        util::log_error("[ui] recreate render target failed: " + detail);
    }
}

void Renderer::render_frame(const PaintFn& paint) {
    if (m_target == nullptr) {
        return;
    }

    m_target->BeginDraw();
    m_target->SetTransform(D2D1::Matrix3x2F::Identity());
    // D-027：一律以**不透明主题背景**清屏——页面渐变中间态/首帧不再透出灰白底色。
    D2D1_COLOR_F base = theme_background();
    base.a = 1.0f;
    m_target->Clear(base);
    if (paint) {
        paint(m_target.Get(), m_dwrite_factory.Get());
    }
    HRESULT hr = m_target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        // 设备相关资源失效：重建渲染目标，下次重绘恢复。
        m_target.Reset();
        create_render_target();
        return;
    }

    hr = m_swap_chain->Present(1, 0);  // vsync（60fps 上限）；空闲无绘制则不 Present
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        const std::string detail = format_hresult(hr);
        util::log_error("[ui] device lost on Present: " + detail);
        m_target.Reset();
    }
}

void Renderer::shutdown() {
    m_target.Reset();
    m_dwrite_factory.Reset();
    m_d2d_factory.Reset();
    m_swap_chain.Reset();
    m_dcomp_visual.Reset();
    m_dcomp_target.Reset();
    m_dcomp_device.Reset();
    m_device.Reset();
    m_hwnd = nullptr;
    m_alpha_mode = false;
}

}  // namespace sscl::ui
