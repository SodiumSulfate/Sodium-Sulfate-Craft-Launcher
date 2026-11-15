#include "ui/framework/window.hpp"

#include <algorithm>

#include <dwmapi.h>
#include <string_view>

#include "resource.h"
#include "platform/win_error.hpp"
#include "ui/shell/shell_layout.hpp"
#include "ui/theme/theme.hpp"
#include "util/log.hpp"

#pragma comment(lib, "dwmapi.lib")

namespace sscl::ui {

namespace {

constexpr wchar_t kWindowClassName[] = L"SSCL.MainWindow";

// —— 启动白闪（D-034）——————————————————————————————
// 症状：启动先闪纯白窗口再显示界面。
// 根因（两层）：
//   ① **主因**：`apply_placement` 用 SetWindowPlacement 恢复窗口位置时 showCmd 写了
//      SW_SHOWNORMAL/SW_SHOWMAXIMIZED —— SetWindowPlacement 会据此**立即显示窗口**，
//      而它发生在 renderer.initialize 之前 → 未绘制窗口（GDI 客户区默认白底）可见数十毫秒。
//      只要 config.json 有保存过窗口位置（首次关闭后每次启动）就必现。
//   ② 次因：即使窗口保持隐藏，首帧经 WM_PAINT → Present(1,0)（vsync）才呈现；
//      空窗期间 DWM/GDI 填充默认白底。
// 修复：① apply_placement 一律 showCmd=SW_HIDE（最大化意图由 show() 以 SW_SHOWMAXIMIZED 生效）；
//       ② show() 在窗口仍隐藏时先渲染提交首帧 + DwmFlush 再显示；
//       ③ WM_ERASEBKGND 用主题背景色（OLED 纯黑）GDI 填充，任何擦除路径不再出白；
//       ④ 禁用 DWM 显隐过渡动画（避免空内容快照参与开场动画）。

// D2D 颜色 → GDI COLORREF（类背景刷用）。
COLORREF color_to_ref(const D2D1_COLOR_F& color) {
    const int r = static_cast<int>(color.r * 255.0f + 0.5f);
    const int g = static_cast<int>(color.g * 255.0f + 0.5f);
    const int b = static_cast<int>(color.b * 255.0f + 0.5f);
    return RGB(r, g, b);
}

// 类背景刷（进程级一次性；主题切换时经 SetClassLongPtr 换色）。
HBRUSH make_background_brush() {
    return ::CreateSolidBrush(color_to_ref(theme_background()));
}

void update_class_background(HWND hwnd) {
    if (hwnd == nullptr) {
        return;
    }
    const HBRUSH brush = make_background_brush();
    if (brush != nullptr) {
        ::SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(brush));
    }
}

// —— 窗口装饰（D-027：阴影/圆角；**不再挂磨砂背板**）————————
// D-026 主题全部表面不透明（OLED 纯黑），DWM acrylic 背板只会从
// 启动首帧与页面渐变中间态漏出灰白色 → 显式关闭背板，仅保留阴影与圆角。

constexpr int kDwmWindowCornerPreference = 33;  // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr int kDwmCornerRound = 2;              // DWMWCP_ROUND
constexpr int kDwmSystemBackdropType = 38;      // DWMWA_SYSTEMBACKDROP_TYPE
constexpr int kDwmBackdropNone = 0;             // DWMSBT_NONE

void apply_window_backdrop(HWND hwnd) {
    // 1px 玻璃边：保留 DWM 阴影、最小化/最大化动画与 Win11 贴靠。
    const MARGINS margins{1, 1, 1, 1};
    ::DwmExtendFrameIntoClientArea(hwnd, &margins);

    // Win11：圆角；显式关闭系统背板（防灰白漏出）。
    const int corner = kDwmCornerRound;
    ::DwmSetWindowAttribute(hwnd, kDwmWindowCornerPreference, &corner, sizeof(corner));
    const int backdrop = kDwmBackdropNone;
    ::DwmSetWindowAttribute(hwnd, kDwmSystemBackdropType, &backdrop, sizeof(backdrop));

    // D-034：禁用显隐过渡动画（DWMWA_TRANSITION_FORCEDISABLED），
    // 避免开场动画拿空内容快照参与合成 → 白闪。
    constexpr int kDwmTransitionForcedDisabled = 3;
    const int no_transition = 1;
    ::DwmSetWindowAttribute(hwnd, kDwmTransitionForcedDisabled, &no_transition,
                            sizeof(no_transition));
    util::log_info("[ui] window chrome: shadow+round corners, backdrop disabled (D-027)");
}

}  // namespace

AppWindow::~AppWindow() {
    if (m_hwnd != nullptr) {
        ::SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        ::DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

util::Result<void> AppWindow::create(const std::wstring& title, int width, int height) {
    HINSTANCE instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &AppWindow::wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    // 应用图标（resources/sscl.rc，负责人制作的 sscl-app-icon.ico）：任务栏/Alt-Tab/窗口。
    wc.hIcon = static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(IDI_SSCL_APP),
                                               IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    wc.hIconSm = static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(IDI_SSCL_APP),
                                                 IMAGE_ICON, 16, 16, 0));
    wc.lpszClassName = kWindowClassName;
    // 启动白闪（D-034）：类背景 = 主题背景色，系统擦除/首显填充不会出白。
    wc.hbrBackground = make_background_brush();
    if (::RegisterClassExW(&wc) == 0) {
        const unsigned long err = ::GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            return util::Result<void>::fail(util::Error::make(
                "ui.register_class_failed", "无法注册窗口类", platform::format_win_error(err)));
        }
    }

    // 无边框 + 自绘标题栏（D-016）。
    // ⚠ 不含 WS_CAPTION：系统标题栏绝无可能出现（不依赖 WM_NCCALCSIZE 消息时序）；
    // WS_THICKFRAME 保留贴靠/边缘缩放（边框由 WM_NCCALCSIZE 隐去，命中由 WM_NCHITTEST 定义）。
    const DWORD style = WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;

    // 宽高按 DIP 解释，按系统 DPI 缩放为物理像素（否则高 DPI 下创建出的窗口
    // 会小于 WM_GETMINMAXINFO 的 DIP 最小尺寸，激活时被强制撑大 → 布局跳变）。
    const UINT system_dpi = ::GetDpiForSystem();
    int width_px = (width * static_cast<int>(system_dpi)) / 96;
    int height_px = (height * static_cast<int>(system_dpi)) / 96;

    // WS_POPUP 不认 CW_USEDEFAULT（会被解析为 0,0 → 左上角）；默认在工作区居中。
    // 用 MonitorFromPoint + GetMonitorInfo 取物理工作区，避免 SPI_GETWORKAREA 在
    // 高 DPI/多显示器下的坐标语义差异导致窗口偏到左上角。
    RECT work{};
    const POINT anchor{0, 0};
    const HMONITOR monitor = ::MonitorFromPoint(anchor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (monitor != nullptr && ::GetMonitorInfoW(monitor, &monitor_info) != FALSE) {
        work = monitor_info.rcWork;
    } else {
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    }
    const int work_width = static_cast<int>(work.right - work.left);
    const int work_height = static_cast<int>(work.bottom - work.top);
    if (work_width > 0 && work_height > 0) {
        width_px = (std::min)(width_px, work_width);
        height_px = (std::min)(height_px, work_height);
    }
    const int x = static_cast<int>(work.left) + (work_width - width_px) / 2;
    const int y = static_cast<int>(work.top) + (work_height - height_px) / 2;

    m_hwnd = ::CreateWindowExW(WS_EX_APPWINDOW, kWindowClassName, title.c_str(), style,
                               x, y, width_px, height_px, nullptr,
                               nullptr, instance, this);
    if (m_hwnd == nullptr) {
        const unsigned long err = ::GetLastError();
        return util::Result<void>::fail(
            util::Error::make("ui.create_window_failed", "无法创建窗口",
                              platform::format_win_error(err)));
    }

    m_dpi = ::GetDpiForWindow(m_hwnd);
    LARGE_INTEGER frequency{};
    if (::QueryPerformanceFrequency(&frequency) != FALSE) {
        m_frame_frequency = frequency.QuadPart;
    }
    apply_window_backdrop(m_hwnd);

    // D-123：派发 create() 之前暂存的异步回调（启动早期后台线程的结果）。
    // 在此之前 m_hwnd 为空，post_to_ui 无法 PostMessage，只把回调放进队列。
    m_created = true;
    bool has_pending = false;
    {
        std::lock_guard<std::mutex> lock(m_ui_mutex);
        has_pending = !m_ui_queue.empty();
    }
    if (has_pending) {
        ::PostMessageW(m_hwnd, kMsgUiDispatch, 0, 0);
    }
    return util::Result<void>::ok();
}

void AppWindow::show(int show_command) {
    if (m_hwnd == nullptr) {
        return;
    }
    // 启动白闪（D-034）：窗口仍隐藏时先渲染并提交首帧，等 DWM 合成器接收后再显示。
    // 用户看到的第一眼即是完整界面，而非空窗白闪（Present(1,0) 的 vsync 等待也被隐藏期吸收）。
    if (on_paint) {
        on_paint();
        ::DwmFlush();
    }
    // 首次 ShowWindow 可能采用 STARTUPINFO 的位置（IDE/某些启动器会传 0,0），
    // 覆盖 CreateWindow/SetWindowPlacement 已设好的位置。先记住隐藏期几何，
    // ShowWindow 后再强制一次，保证用户看到的第一帧就在目标位置。
    RECT desired{};
    const bool enforce_rect =
        !m_show_maximized && ::GetWindowRect(m_hwnd, &desired) != FALSE;
    // apply_placement 记录的最大化意图在此生效（不走 SetWindowPlacement 的 showCmd）。
    const int command = m_show_maximized ? SW_SHOWMAXIMIZED : show_command;
    ::ShowWindow(m_hwnd, command);
    if (enforce_rect && ::IsZoomed(m_hwnd) == FALSE && ::IsIconic(m_hwnd) == FALSE) {
        ::SetWindowPos(m_hwnd, nullptr, desired.left, desired.top,
                       desired.right - desired.left, desired.bottom - desired.top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ::UpdateWindow(m_hwnd);
    RECT shown{};
    if (::GetWindowRect(m_hwnd, &shown) != FALSE) {
        util::log_info("[ui] window shown at " + std::to_string(shown.left) + "," +
                       std::to_string(shown.top) + " " +
                       std::to_string(shown.right - shown.left) + "x" +
                       std::to_string(shown.bottom - shown.top));
    }
}

int AppWindow::run_message_loop() {
    MSG msg{};
    BOOL result = 0;
    while ((result = ::GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return (result == 0) ? static_cast<int>(msg.wParam) : 1;
}

void AppWindow::invalidate() {
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void AppWindow::hide_window() {
    if (m_hwnd != nullptr) {
        ::ShowWindow(m_hwnd, SW_HIDE);
    }
}

void AppWindow::start_frame_loop() {
    if (m_hwnd == nullptr || m_frame_loop) {
        return;
    }
    m_frame_loop = true;
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    m_last_frame_qpc = now.QuadPart;
    ::InvalidateRect(m_hwnd, nullptr, FALSE);  // 启动循环（随后由 WM_PAINT 自驱）
}

void AppWindow::start_status_timer() {
    if (m_hwnd == nullptr || m_status_timer_started) {
        return;
    }
    if (::SetTimer(m_hwnd, kStatusTimerId, 100, nullptr) == 0) {
        util::log_warn("[ui] SetTimer(100ms) failed");
        return;
    }
    m_status_timer_started = true;
}

// 高精度帧间隔（QPC，毫秒）。
double AppWindow::frame_delta_ms() {
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    if (m_frame_frequency <= 0 || m_last_frame_qpc == 0) {
        m_last_frame_qpc = now.QuadPart;
        return 16.0;
    }
    const long long delta = now.QuadPart - m_last_frame_qpc;
    m_last_frame_qpc = now.QuadPart;
    double dt = static_cast<double>(delta) * 1000.0 / static_cast<double>(m_frame_frequency);
    if (dt <= 0.0) {
        dt = 1.0;
    }
    if (dt > 100.0) {
        dt = 100.0;  // 拖动窗口等长暂停后钳制，避免动画跳跃
    }
    return dt;
}

// 几何通知（去重）：渲染器 resize + 页面布局都由 on_resize 承担。
void AppWindow::notify_geometry(UINT width, UINT height) {
    if (width == m_last_client_width && height == m_last_client_height) {
        return;
    }
    m_last_client_width = width;
    m_last_client_height = height;
    if (on_resize) {
        on_resize(width, height);
    }
}

void AppWindow::minimize() {
    if (m_hwnd != nullptr) {
        ::ShowWindow(m_hwnd, SW_MINIMIZE);
    }
}

void AppWindow::toggle_maximize() {
    if (m_hwnd != nullptr) {
        ::ShowWindow(m_hwnd, is_maximized() ? SW_RESTORE : SW_MAXIMIZE);
    }
}

void AppWindow::close_window() {
    if (m_hwnd != nullptr) {
        ::PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    }
}

bool AppWindow::is_maximized() const {
    return m_hwnd != nullptr && ::IsZoomed(m_hwnd) != FALSE;
}

// 恢复窗口位置（D-017）。rcNormalPosition 用 WINDOWPLACEMENT 工作区坐标语义，
// 与 capture_placement 成对往返；矩形离开虚拟屏（显示器布局变化）则放弃恢复。
// ⚠ D-034：showCmd 必须为 SW_HIDE——SW_SHOWNORMAL/SW_SHOWMAXIMIZED 会让
// SetWindowPlacement **立即显示**未绘制窗口（早于渲染器初始化）→ 启动白闪。
// 最大化意图记入 m_show_maximized，由 show() 以 SW_SHOWMAXIMIZED 生效。
void AppWindow::apply_placement(int x, int y, int width, int height, bool maximized) {
    if (m_hwnd == nullptr || width <= 0 || height <= 0) {
        return;
    }
    RECT rect{x, y, x + width, y + height};
    const HMONITOR monitor = ::MonitorFromRect(&rect, MONITOR_DEFAULTTONULL);
    if (monitor == nullptr) {
        util::log_info("[ui] saved window placement off-screen; using default position");
        return;
    }
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info) == FALSE) {
        return;
    }
    // 夹到当前显示器工作区：分辨率/缩放/显示器布局变化后，旧坐标可能让窗口
    // 一部分甚至大部分落在屏幕外。至少保证整个窗口在 work area 内。
    const RECT& work = info.rcWork;
    const int work_left = static_cast<int>(work.left);
    const int work_top = static_cast<int>(work.top);
    const int work_w = static_cast<int>(work.right - work.left);
    const int work_h = static_cast<int>(work.bottom - work.top);
    if (work_w <= 0 || work_h <= 0) {
        return;
    }
    const int w = (std::min)(width, work_w);
    const int h = (std::min)(height, work_h);
    const int left = (std::max)(work_left, (std::min)(x, work_left + work_w - w));
    const int top = (std::max)(work_top, (std::min)(y, work_top + work_h - h));

    util::log_info("[ui] restore window placement " + std::to_string(left) + "," +
                   std::to_string(top) + " " + std::to_string(w) + "x" +
                   std::to_string(h) + (maximized ? " maximized" : ""));

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    placement.rcNormalPosition = RECT{left, top, left + w, top + h};
    placement.showCmd = SW_HIDE;  // 不显示（D-034）
    if (maximized) {
        placement.flags = WPF_RESTORETOMAXIMIZED;
        m_show_maximized = true;
    }
    ::SetWindowPlacement(m_hwnd, &placement);
    // 首次 ShowWindow 可能受 STARTUPINFO 影响；隐藏期再强制一次几何，确保
    // 恢复的位置/大小真正生效（D-034 的“不显示”语义不变）。
    ::SetWindowPos(m_hwnd, nullptr, left, top, w, h,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

bool AppWindow::capture_placement(int* x, int* y, int* width, int* height,
                                  bool* maximized) const {
    if (m_hwnd == nullptr || x == nullptr || y == nullptr || width == nullptr ||
        height == nullptr || maximized == nullptr) {
        return false;
    }
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (::GetWindowPlacement(m_hwnd, &placement) == FALSE) {
        return false;
    }
    const RECT& rect = placement.rcNormalPosition;
    *x = rect.left;
    *y = rect.top;
    *width = rect.right - rect.left;
    *height = rect.bottom - rect.top;
    *maximized = ::IsZoomed(m_hwnd) != FALSE ||
                 placement.showCmd == SW_SHOWMAXIMIZED ||
                 (placement.flags & WPF_RESTORETOMAXIMIZED) != 0;
    return *width > 0 && *height > 0;
}

// 自绘无边框命中测试（screen 坐标）：缩放边缘 → 系统缩放；顶栏按钮/导航胶囊 →
// HTCLIENT（自绘交互）；顶栏其余 → HTCAPTION（原生拖拽/双击最大化/贴靠）。
LRESULT AppWindow::hit_test(POINT screen_point) const {
    RECT window_rect{};
    ::GetWindowRect(m_hwnd, &window_rect);
    const int x = screen_point.x - window_rect.left;
    const int y = screen_point.y - window_rect.top;
    const int width = window_rect.right - window_rect.left;
    const int height = window_rect.bottom - window_rect.top;

    const float scale = static_cast<float>(m_dpi) / 96.0f;
    const int frame = ::GetSystemMetrics(SM_CXFRAME) + ::GetSystemMetrics(SM_CXPADDEDBORDER);

    if (!is_maximized()) {
        const bool on_left = x < frame;
        const bool on_right = x >= width - frame;
        const bool on_top = y < frame;
        const bool on_bottom = y >= height - frame;
        if (on_top && on_left) return HTTOPLEFT;
        if (on_top && on_right) return HTTOPRIGHT;
        if (on_bottom && on_left) return HTBOTTOMLEFT;
        if (on_bottom && on_right) return HTBOTTOMRIGHT;
        if (on_left) return HTLEFT;
        if (on_right) return HTRIGHT;
        if (on_top) return HTTOP;
        if (on_bottom) return HTBOTTOM;
    }

    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    if (fy < kNavHeightDip * scale) {
        // 窗口按钮（右上）与导航胶囊：HTCLIENT 交给自绘交互
        if (fx >= static_cast<float>(width) - kWindowButtonsWidthDip * scale) {
            return HTCLIENT;
        }
        if (fx >= kNavPillsLeftDip * scale && fx < kNavPillsRightDip * scale) {
            return HTCLIENT;
        }
        return HTCAPTION;
    }
    return HTCLIENT;
}

LRESULT CALLBACK AppWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    AppWindow* self = reinterpret_cast<AppWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr && msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<AppWindow*>(create->lpCreateParams);
        self->m_hwnd = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        ::EnableNonClientDpiScaling(hwnd);
    }
    if (self != nullptr) {
        return self->handle_message(hwnd, msg, wparam, lparam);
    }
    // CreateWindow 期间的早期消息（WM_NCCALCSIZE/WM_GETMINMAXINFO 早于 WM_NCCREATE）：
    // 此时 self 尚未挂接，必须在此直接按无边框处理，否则首次构帧会画出标准标题栏
    // （点击后 NCCALCSIZE 再次生效导致“标题栏突变 + 布局错位”，2026-10-24 缺陷）。
    if (msg == WM_NCCALCSIZE) {
        ::PostMessageW(hwnd, kMsgRefreshGeometry, 0, 0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT AppWindow::handle_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_DPICHANGED: {
            m_dpi = HIWORD(wparam);
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                           suggested->right - suggested->left, suggested->bottom - suggested->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            invalidate();
            return 0;
        }

        case WM_SIZE: {
            if (wparam != SIZE_MINIMIZED && on_resize) {
                RECT client{};
                ::GetClientRect(hwnd, &client);
                notify_geometry(static_cast<UINT>(client.right - client.left),
                                static_cast<UINT>(client.bottom - client.top));
            }
            return 0;
        }

        case kMsgRefreshGeometry: {
            // WM_NCCALCSIZE 改变客户区后（如首次构帧/激活重算），Windows 不保证发
            // WM_SIZE；这里显式重算几何，避免渲染目标与布局尺寸错位。
            RECT client{};
            ::GetClientRect(hwnd, &client);
            notify_geometry(static_cast<UINT>(client.right - client.left),
                            static_cast<UINT>(client.bottom - client.top));
            invalidate();
            return 0;
        }

        case WM_ERASEBKGND: {
            // D-034：以主题背景色（OLED 纯黑）GDI 填充——任何先于 D2D 首帧的系统擦除
            // 不再出白底（此前 return 1 跳过擦除，未绘制客户区显示 GDI 默认白）。
            const HDC dc = reinterpret_cast<HDC>(wparam);
            RECT client{};
            ::GetClientRect(hwnd, &client);
            const HBRUSH brush = make_background_brush();
            if (brush != nullptr) {
                ::FillRect(dc, &client, brush);
                ::DeleteObject(brush);
            }
            return 1;
        }

        // —— 无边框（D-016）——————————————————————————————
        case WM_NCCALCSIZE:
            // 客户区 = 整个窗口（含原非客户区）。
            // wParam=TRUE：rgrc[0] 保持窗口矩形（返回 0 即不收缩）；
            // wParam=FALSE：lParam 为窗口矩形，原样返回即全客户区。
            // 完成后投递几何重算（Windows 不保证补发 WM_SIZE）。
            ::PostMessageW(hwnd, kMsgRefreshGeometry, 0, 0);
            return 0;

        case WM_NCHITTEST:
            return hit_test(POINT{static_cast<short>(LOWORD(lparam)),
                                  static_cast<short>(HIWORD(lparam))});

        case WM_GETMINMAXINFO: {
            // 最大化停靠工作区（无边框窗口需显式约束，否则溢出屏幕）。
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            const HMONITOR monitor =
                ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO monitor_info{};
            monitor_info.cbSize = sizeof(monitor_info);
            if (monitor != nullptr && ::GetMonitorInfoW(monitor, &monitor_info) != FALSE) {
                const RECT& work = monitor_info.rcWork;
                const RECT& bounds = monitor_info.rcMonitor;
                info->ptMaxPosition.x = work.left - bounds.left;
                info->ptMaxPosition.y = work.top - bounds.top;
                info->ptMaxSize.x = work.right - work.left;
                info->ptMaxSize.y = work.bottom - work.top;
            }
            const float scale = static_cast<float>(m_dpi) / 96.0f;
            // 小屏/高缩放（如 1024×640 @150%）下 850×520 DIP 的最小尺寸会超过
            // 工作区，系统会把窗口撑大并挤到左上角；最小尺寸不得超过工作区。
            LONG min_w = static_cast<LONG>(850.0f * scale);
            LONG min_h = static_cast<LONG>(520.0f * scale);
            if (monitor != nullptr && ::GetMonitorInfoW(monitor, &monitor_info) != FALSE) {
                min_w = (std::min)(min_w, monitor_info.rcWork.right - monitor_info.rcWork.left);
                min_h = (std::min)(min_h, monitor_info.rcWork.bottom - monitor_info.rcWork.top);
            }
            info->ptMinTrackSize.x = (std::max)(1L, min_w);
            info->ptMinTrackSize.y = (std::max)(1L, min_h);
            return 0;
        }

        case WM_NCACTIVATE:
            // 无标准非客户区；自行绘制激活态，返回 1 避免系统闪烁。
            invalidate();
            return 1;

        // D-025：Windows 应用主题切换广播（设置切换浅/深色时 lParam = "ImmersiveColorSet"）。
        // 消息驱动监听：无轮询、无重启，主题变化即时重绘（§5.2）。
        case WM_SETTINGCHANGE: {
            const wchar_t* section = reinterpret_cast<const wchar_t*>(lparam);
            if (section != nullptr &&
                std::wstring_view(section) == L"ImmersiveColorSet") {
                if (apply_system_theme()) {
                    util::log_info("[ui] system app theme changed; recoloring");
                    update_class_background(hwnd);  // 类背景随主题换色（D-034）
                    invalidate();
                }
            }
            break;
        }

        case WM_MOUSEMOVE: {
            const float x = static_cast<float>(static_cast<short>(LOWORD(lparam)));
            const float y = static_cast<float>(static_cast<short>(HIWORD(lparam)));
            // 登记 leave 通知（TrackMouseEvent）
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            ::TrackMouseEvent(&track);
            if (on_mouse_move) {
                on_mouse_move(x, y);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            if (on_mouse_leave) {
                on_mouse_leave();
            }
            return 0;

        case WM_LBUTTONDOWN: {
            const float x = static_cast<float>(static_cast<short>(LOWORD(lparam)));
            const float y = static_cast<float>(static_cast<short>(HIWORD(lparam)));
            if (on_mouse_down) {
                on_mouse_down(x, y);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            const float x = static_cast<float>(static_cast<short>(LOWORD(lparam)));
            const float y = static_cast<float>(static_cast<short>(HIWORD(lparam)));
            if (on_mouse_up) {
                on_mouse_up(x, y);
            }
            return 0;
        }

        case WM_RBUTTONDOWN: {
            const float x = static_cast<float>(static_cast<short>(LOWORD(lparam)));
            const float y = static_cast<float>(static_cast<short>(HIWORD(lparam)));
            if (on_mouse_right_down) {
                on_mouse_right_down(x, y);
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam));
            // 滚轮需带光标位置（lParam 为屏幕坐标 → 转客户区）：页面据此限定
            // 仅滚动区内的滚轮有效（D-035：侧栏上滚动不得驱动列表）。
            POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            ::ScreenToClient(hwnd, &point);
            if (on_mouse_wheel) {
                on_mouse_wheel(static_cast<float>(point.x), static_cast<float>(point.y), delta);
            }
            return 0;
        }

        // 键盘输入：IME 合成完成后系统转为 WM_CHAR（默认过程处理合成窗口）。
        case WM_CHAR:
            if (on_char && wparam >= 0x20 && wparam != 0x7F) {
                on_char(static_cast<wchar_t>(wparam));
            }
            return 0;

        case WM_KEYDOWN:
            if (on_key_down) {
                on_key_down(static_cast<unsigned long>(wparam));
            }
            return 0;

        case kMsgUiDispatch: {
            std::vector<std::function<void()>> pending;
            {
                std::lock_guard<std::mutex> lock(m_ui_mutex);
                pending.swap(m_ui_queue);
            }
            for (auto& fn : pending) {
                if (fn) {
                    fn();
                }
            }
            return 0;
        }

        case WM_TIMER:
            // 固定 0.1s 系统状态刷新（CPU/RAM）；页面不可见时回调内部不重绘。
            if (wparam == kStatusTimerId && on_status_timer) {
                on_status_timer();
            }
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            ::BeginPaint(hwnd, &paint);

            // 动画步进在绘制前（本帧呈现步进后的状态，D-021）。
            bool more_frames = false;
            if (m_frame_loop && on_frame) {
                more_frames = on_frame(static_cast<UINT>(frame_delta_ms() + 0.5));
            }
            if (on_paint) {
                on_paint();
            }
            ::EndPaint(hwnd, &paint);

            // Present(1,0) 已 vsync 定步：立即失效重绘 = 自驱循环，帧率=显示器刷新率。
            if (m_frame_loop) {
                if (more_frames && ::IsIconic(hwnd) == FALSE) {
                    ::InvalidateRect(hwnd, nullptr, FALSE);
                } else {
                    m_frame_loop = false;  // 动画收敛/最小化：停止渲染
                }
            }
            return 0;
        }

        case WM_CLOSE:
            // D-118：子窗口（账号管理）关闭 = 隐藏，保留实例供下次复用。
            if (on_close_request && on_close_request()) {
                return 0;
            }
            ::DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (m_status_timer_started) {
                ::KillTimer(hwnd, kStatusTimerId);
                m_status_timer_started = false;
            }
            if (on_destroy) {
                on_destroy();
            }
            // D-123：此后一律丢弃回调，不再暂存（窗口不会重建）。
            m_destroyed = true;
            m_created = false;
            {
                std::lock_guard<std::mutex> lock(m_ui_mutex);
                m_ui_queue.clear();
            }
            m_hwnd = nullptr;
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

void AppWindow::post_to_ui(std::function<void()> fn) {
    if (!fn || m_destroyed) {
        return;  // 窗口已销毁：丢弃回调（后台线程收尾时避免触碰已析构对象）
    }
    {
        std::lock_guard<std::mutex> lock(m_ui_mutex);
        m_ui_queue.push_back(std::move(fn));
    }
    // D-123：窗口尚未 create() 时没有 HWND，先暂存；create() 末尾会补一条派发消息。
    // 过去这里直接 return，导致启动早期的皮肤/状态回调被永久丢弃。
    if (m_created && m_hwnd != nullptr) {
        ::PostMessageW(m_hwnd, kMsgUiDispatch, 0, 0);
    }
}

}  // namespace sscl::ui
