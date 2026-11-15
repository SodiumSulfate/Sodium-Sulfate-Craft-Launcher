#include "ui/framework/splash.hpp"

#include <dwmapi.h>

#include <algorithm>
#include <memory>
#include <mutex>

#include "resource.h"
#include "ui/theme/theme.hpp"
#include "util/log.hpp"

#pragma comment(lib, "dwmapi.lib")

namespace sscl::ui {

namespace {

constexpr wchar_t kClassName[] = L"SSCL.Splash";
constexpr int kCardWidthDip = 320;
constexpr int kCardHeightDip = 280;
constexpr int kCornerRadiusDip = 16;  // 圆角半径；GDI 椭圆参数为直径 = 2×
constexpr int kIconDip = 128;
constexpr int kIconTopDip = 40;
constexpr int kTitleTopDip = 182;
constexpr int kTitleHeightDip = 34;
constexpr int kTitleSizeDip = 20;
constexpr int kStatusTopDip = 222;
constexpr int kStatusHeightDip = 24;
constexpr int kStatusSizeDip = 12;

COLORREF to_ref(const D2D1_COLOR_F& color) {
    const int r = static_cast<int>(color.r * 255.0f + 0.5f);
    const int g = static_cast<int>(color.g * 255.0f + 0.5f);
    const int b = static_cast<int>(color.b * 255.0f + 0.5f);
    return RGB((std::clamp)(r, 0, 255), (std::clamp)(g, 0, 255), (std::clamp)(b, 0, 255));
}

HFONT make_font(int px, bool bold) {
    return ::CreateFontW(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, theme_font_family());
}

RECT scaled_rect(int left_dip, int top_dip, int right_dip, int bottom_dip, UINT dpi) {
    RECT r{};
    r.left = ::MulDiv(left_dip, static_cast<int>(dpi), 96);
    r.top = ::MulDiv(top_dip, static_cast<int>(dpi), 96);
    r.right = ::MulDiv(right_dip, static_cast<int>(dpi), 96);
    r.bottom = ::MulDiv(bottom_dip, static_cast<int>(dpi), 96);
    return r;
}

}  // namespace

SplashScreen::Palette SplashScreen::snapshot_palette() {
    // D-082：浅色 Surface/Elevated 与 Background 对比极低，浮层卡片必须带 1px Outline。
    // 语义色在主线程取快照后交给 splash 线程，避免跨线程读写 theme 的全局模式。
    Palette palette;
    palette.background = to_ref(theme_elevated_surface());
    palette.border = to_ref(theme_border());
    palette.title = to_ref(theme_text_primary());
    palette.status = to_ref(theme_text_secondary());
    return palette;
}

LRESULT CALLBACK SplashScreen::wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return ::DefWindowProcW(hwnd, msg, wparam, lparam);  // 保留系统默认创建流程
    }
    auto* self = reinterpret_cast<SplashScreen*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
        return self->handle_message(hwnd, msg, wparam, lparam);
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT SplashScreen::handle_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = ::BeginPaint(hwnd, &ps);
            if (dc != nullptr) {
                paint(hwnd, dc);
                ::EndPaint(hwnd, &ps);
            }
            return 0;
        }
        case kMsgRepaint:
            ::InvalidateRect(hwnd, nullptr, FALSE);
            ::UpdateWindow(hwnd);
            return 0;
        case kMsgDismiss:
            // 主窗口 ShowWindow 会激活并置顶；在 splash 线程内提到最顶层，
            // 这样 150ms 淡出才真正可见（渐隐后露出主界面）。放在线程内也避免
            // 主线程对尚未进入消息循环的窗口做跨线程 SetWindowPos（同步阻塞）。
            ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            // 启动很快时淡入可能尚未结束：就地中止，并从当前透明度接着淡出，避免跳变。
            if (m_fading_in) {
                ::KillTimer(hwnd, kFadeInTimerId);
                m_fading_in = false;
            }
            m_fade_out_from = m_current_alpha;
            m_fade_out_start = ::GetTickCount64();
            ::SetTimer(hwnd, kFadeOutTimerId, 16, nullptr);
            return 0;
        case WM_TIMER:
            if (wparam == kFadeInTimerId && m_fading_in) {
                const unsigned long long elapsed = ::GetTickCount64() - m_fade_in_start;
                const int alpha =
                    elapsed >= kFadeInDurationMs
                        ? 255
                        : static_cast<int>(kFadeInStartAlpha +
                                           (255 - kFadeInStartAlpha) *
                                               (static_cast<double>(elapsed) /
                                                static_cast<double>(kFadeInDurationMs)));
                ::SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(alpha), LWA_ALPHA);
                m_current_alpha = alpha;
                if (alpha >= 255) {
                    ::KillTimer(hwnd, kFadeInTimerId);
                    m_fading_in = false;
                }
                return 0;
            }
            if (wparam == kFadeOutTimerId) {
                const unsigned long long elapsed = ::GetTickCount64() - m_fade_out_start;
                const int alpha =
                    elapsed >= kFadeOutDurationMs
                        ? 0
                        : static_cast<int>(m_fade_out_from *
                                           (1.0 - static_cast<double>(elapsed) /
                                                      static_cast<double>(kFadeOutDurationMs)));
                ::SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(alpha), LWA_ALPHA);
                m_current_alpha = alpha;
                if (alpha == 0) {
                    ::KillTimer(hwnd, kFadeOutTimerId);
                    ::DestroyWindow(hwnd);
                    ::PostQuitMessage(0);  // 仅结束 splash 线程的消息循环
                }
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;  // WM_PAINT 全覆盖，避免 GDI 默认白底闪烁
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

void SplashScreen::paint(HWND hwnd, HDC dc) {
    RECT client{};
    ::GetClientRect(hwnd, &client);

    Palette palette;
    std::wstring status;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        palette = m_palette;
        status = m_status;
    }

    HBRUSH background = ::CreateSolidBrush(palette.background);
    if (background != nullptr) {
        ::FillRect(dc, &client, background);
        ::DeleteObject(background);
    }

    // 圆角描边：与 SetWindowRgn 的椭圆参数一致，浅色主题下给出卡片边界。
    HPEN pen = ::CreatePen(PS_SOLID, 1, palette.border);
    if (pen != nullptr) {
        HGDIOBJ old_pen = ::SelectObject(dc, pen);
        HGDIOBJ old_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
        ::RoundRect(dc, 1, 1, client.right - 1, client.bottom - 1, m_corner, m_corner);
        ::SelectObject(dc, old_brush);
        ::SelectObject(dc, old_pen);
        ::DeleteObject(pen);
    }

    const int icon_px = ::MulDiv(kIconDip, static_cast<int>(m_dpi), 96);
    const int icon_top = ::MulDiv(kIconTopDip, static_cast<int>(m_dpi), 96);
    if (m_icon != nullptr) {
        ::DrawIconEx(dc, (client.right - icon_px) / 2, icon_top, m_icon, icon_px, icon_px,
                     0, nullptr, DI_NORMAL | DI_NOMIRROR);
    }

    ::SetBkMode(dc, TRANSPARENT);
    HFONT title_font = make_font(::MulDiv(kTitleSizeDip, static_cast<int>(m_dpi), 96), true);
    if (title_font != nullptr) {
        HGDIOBJ old = ::SelectObject(dc, title_font);
        ::SetTextColor(dc, palette.title);
        RECT title = scaled_rect(0, kTitleTopDip, kCardWidthDip,
                                 kTitleTopDip + kTitleHeightDip, m_dpi);
        ::DrawTextW(dc, L"SSCL", -1, &title,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        ::SelectObject(dc, old);
        ::DeleteObject(title_font);
    }

    HFONT status_font = make_font(::MulDiv(kStatusSizeDip, static_cast<int>(m_dpi), 96), false);
    if (status_font != nullptr) {
        HGDIOBJ old = ::SelectObject(dc, status_font);
        ::SetTextColor(dc, palette.status);
        RECT status_rect = scaled_rect(0, kStatusTopDip, kCardWidthDip,
                                       kStatusTopDip + kStatusHeightDip, m_dpi);
        ::DrawTextW(dc, status.c_str(), -1, &status_rect,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        ::SelectObject(dc, old);
        ::DeleteObject(status_font);
    }
}

void SplashScreen::request_repaint() {
    HWND hwnd = m_hwnd.load();
    if (hwnd != nullptr) {
        ::PostMessageW(hwnd, kMsgRepaint, 0, 0);
    }
}

void SplashScreen::thread_main(HINSTANCE instance, std::latch& ready) {
    bool signaled = false;
    auto signal = [&ready, &signaled]() {
        if (!signaled) {
            signaled = true;
            ready.count_down();
        }
    };

    try {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &SplashScreen::wnd_proc;
        wc.hInstance = instance;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        wc.hbrBackground = nullptr;  // WM_PAINT 自绘
        if (::RegisterClassExW(&wc) == 0 &&
            ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            util::log_warn("[splash] RegisterClassExW failed");
            signal();
            return;
        }

        m_dpi = ::GetDpiForSystem();
        m_corner = ::MulDiv(kCornerRadiusDip * 2, static_cast<int>(m_dpi), 96);
        const int width = ::MulDiv(kCardWidthDip, static_cast<int>(m_dpi), 96);
        const int height = ::MulDiv(kCardHeightDip, static_cast<int>(m_dpi), 96);
        const int screen_width = ::GetSystemMetrics(SM_CXSCREEN);
        const int screen_height = ::GetSystemMetrics(SM_CYSCREEN);
        const int x = (screen_width - width) / 2;
        const int y = (screen_height - height) / 2;

        HWND hwnd = ::CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"SSCL",
            WS_POPUP, x, y, width, height, nullptr, nullptr, instance, this);
        if (hwnd == nullptr) {
            util::log_warn("[splash] CreateWindowExW failed");
            signal();
            return;
        }
        m_hwnd.store(hwnd);

        HRGN region = ::CreateRoundRectRgn(0, 0, width + 1, height + 1, m_corner, m_corner);
        if (region != nullptr) {
            if (::SetWindowRgn(hwnd, region, FALSE) == 0) {
                ::DeleteObject(region);  // 失败时系统未接管，需自行释放
            }
        }
        // 首帧即以 160/255 可见；剩余 90ms 淡入由 splash 线程定时器自驱，
        // 主线程在 signal() 后立即继续初始化，绝不为动画多等一帧。
        m_current_alpha = kFadeInStartAlpha;
        ::SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(m_current_alpha), LWA_ALPHA);

        const int icon_px = ::MulDiv(kIconDip, static_cast<int>(m_dpi), 96);
        m_icon = static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(IDI_SSCL_APP),
                                                 IMAGE_ICON, icon_px, icon_px,
                                                 LR_DEFAULTCOLOR));

        ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        ::UpdateWindow(hwnd);  // 同步提交首帧（WM_PAINT）
        m_fade_in_start = ::GetTickCount64();
        m_fading_in = true;
        ::SetTimer(hwnd, kFadeInTimerId, 16, nullptr);  // 在 signal() 之前启动，避免与 dismiss 抢序
        ::DwmFlush();          // 等 DWM 合成，返回时图标已真实可见
        signal();              // 主线程此刻才继续初始化

        MSG msg{};
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    } catch (...) {
        util::log_warn("[splash] unexpected failure in splash thread");
    }

    if (m_icon != nullptr) {
        ::DestroyIcon(m_icon);
        m_icon = nullptr;
    }
    m_hwnd.store(nullptr);
    signal();  // 兜底：任何提前返回都不让主线程卡在 wait()
}

void SplashScreen::show(HINSTANCE instance) {
    if (m_thread.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_palette = snapshot_palette();
    }
    try {
        auto ready = std::make_shared<std::latch>(1);
        m_thread = std::thread([this, instance, ready]() { thread_main(instance, *ready); });
        ready->wait();  // 首帧已提交，返回后主线程才开始耗时初始化
    } catch (...) {
        // 线程/内存创建失败绝不阻断启动器：直接跳过启动画面。
        util::log_warn("[splash] failed to start splash thread, continuing without it");
    }
}

void SplashScreen::update_theme() {
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_palette = snapshot_palette();
    }
    request_repaint();
}

void SplashScreen::dismiss() {
    if (m_dismissed.exchange(true)) {
        return;
    }
    HWND hwnd = m_hwnd.load();
    if (hwnd == nullptr) {
        return;
    }
    // 提顶层与淡出都由 splash 线程内完成，主线程只投递一条消息后立即返回。
    ::PostMessageW(hwnd, kMsgDismiss, 0, 0);
}

SplashScreen::~SplashScreen() {
    dismiss();  // 异常/失败路径兜底；正常路径已调用过，exchange 保证不重复
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

}  // namespace sscl::ui
