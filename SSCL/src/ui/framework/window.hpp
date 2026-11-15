#pragma once

// Win32 主窗口封装。
// 2026-10-24 负责人确认（OPEN_QUESTIONS #18）：切换 **无边框 + 自绘标题栏 + 磨砂玻璃**（DECISIONS D-016）：
// WS_POPUP 样式 + WM_NCCALCSIZE 消除标准边框 + WM_NCHITTEST 自定拖拽/缩放/按钮命中，
// DWM 阴影/贴靠/动画保留；背景走系统磨砂（Win11 DWMSBT_TRANSIENTWINDOW，Win10 acrylic 合成）。
// 保留：Per-Monitor V2 DPI、按需重绘消息循环（空闲不重绘、无空转）。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "util/result.hpp"

namespace sscl::ui {

class AppWindow {
public:
    AppWindow() = default;
    ~AppWindow();

    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    // 注册窗口类并创建无边框窗口（width/height 为像素）。
    util::Result<void> create(const std::wstring& title, int width, int height);

    void show(int show_command);

    // 标准消息循环；返回进程退出码。
    int run_message_loop();

    // 请求重绘（WM_PAINT 中调用 on_paint；空闲时无绘制、无 Present）。
    void invalidate();

    // 自绘标题栏按钮（§8；SC_MINIMIZE/SC_MAXIMIZE/SC_CLOSE 语义）。
    void minimize();
    void toggle_maximize();
    void close_window();
    bool is_maximized() const;

    // 窗口位置记忆（D-017；WINDOWPLACEMENT 语义，物理像素）：
    // apply 在 show 前调用（无效/离开虚拟屏则保持默认居中）；capture 在销毁前调用。
    // ⚠ apply 只恢复几何**不显示窗口**（D-034：SetWindowPlacement 的 showCmd 若为
    // SW_SHOWNORMAL/SW_SHOWMAXIMIZED 会立即显示未绘制窗口 → 启动白闪）；
    // 最大化意图由 show() 以 SW_SHOWMAXIMIZED 生效。
    void apply_placement(int x, int y, int width, int height, bool maximized);
    bool capture_placement(int* x, int* y, int* width, int* height, bool* maximized) const;

    HWND hwnd() const { return m_hwnd; }
    UINT dpi() const { return m_dpi; }

    // 隐藏但不销毁（D-118：账号管理子窗口关闭后保留实例，下次直接复用）。
    void hide_window();

    // GUI 线程回调。
    std::function<void(UINT width, UINT height)> on_resize;  // 客户区尺寸变化
    std::function<void()> on_paint;                          // 需要绘制一帧
    std::function<void()> on_destroy;                        // 窗口销毁前
    // 返回 true = 调用方已处理关闭（保留窗口，仅隐藏）；空/false = 默认销毁。
    std::function<bool()> on_close_request;
    std::function<void(float x, float y)> on_mouse_move;     // 客户区坐标（物理像素）
    std::function<void(float x, float y)> on_mouse_down;
    std::function<void(float x, float y)> on_mouse_up;
    std::function<void(float x, float y)> on_mouse_right_down;
    std::function<void()> on_mouse_leave;
    std::function<void(float x, float y, float delta)> on_mouse_wheel;  // 滚轮（客户区坐标物理像素，正=向上）
    std::function<void(wchar_t ch)> on_char;          // 字符输入（含 IME 合成结果）
    std::function<void(unsigned long key)> on_key_down;  // 按键（VK_*）
    // 动画帧（D-021）：由 Present vsync 定步（= 显示器刷新率）；
    // on_frame 返回 false 时停止渲染（画面无变化不绘制，空闲不重绘原则保持）。
    std::function<bool(UINT dt_ms)> on_frame;

    // 启动动画渲染循环（已在运行则忽略）；hover/切换/下载进度变化时调用。
    void start_frame_loop();

    // 固定 0.1s 系统状态刷新（CPU/RAM）：页面左侧简略信息使用。
    // 与动画帧循环无关，避免为了常驻数据把 GUI 锁在 60fps 重绘。
    std::function<void()> on_status_timer;
    void start_status_timer();

    // UI 线程调度（§5.1：后台任务更新 UI 必须回 UI 线程）。
    void post_to_ui(std::function<void()> fn);

private:
    static constexpr UINT kMsgUiDispatch = WM_APP + 1;      // post_to_ui 队列派发
    static constexpr UINT kMsgRefreshGeometry = WM_APP + 2;  // 客户区几何重算（NCCALCSIZE 后）
    static constexpr UINT_PTR kStatusTimerId = 1;            // 0.1s CPU/RAM 刷新

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT hit_test(POINT screen_point) const;
    void notify_geometry(UINT width, UINT height);
    double frame_delta_ms();

    HWND m_hwnd = nullptr;
    // D-123：区分「窗口尚未创建」与「窗口已销毁」。
    // AppShell 在 main.cpp 中先于 window.create() 构造，启动早期的异步回调
    // （账号皮肤、版本列表…）会落在尚未有 HWND 的窗口上：必须暂存，
    // 待 create() 成功后统一派发；销毁后则直接丢弃，避免悬挂访问。
    bool m_created = false;
    bool m_destroyed = false;
    UINT m_dpi = 96;
    UINT m_last_client_width = 0;   // 供几何刷新去重
    UINT m_last_client_height = 0;
    bool m_show_maximized = false;  // apply_placement 记录的最大化意图（show 时生效，D-034）
    bool m_frame_loop = false;      // 动画渲染循环进行中
    bool m_status_timer_started = false;  // 0.1s 系统状态定时器
    long long m_frame_frequency = 0;
    long long m_last_frame_qpc = 0;
    std::mutex m_ui_mutex;
    std::vector<std::function<void()>> m_ui_queue;
};

}  // namespace sscl::ui
