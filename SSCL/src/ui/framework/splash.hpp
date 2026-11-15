#pragma once

// 启动画面（D-084）：主窗口出现前，在屏幕中央展示软件图标 + 名称 + 状态文字，
// 消除双击 EXE 后「什么都没有」的死等待。
//
// 设计要点：
//  ① 独立线程拥有窗口和消息循环 —— 主线程从配置/账号加载到 D2D 首帧完全串行阻塞，
//     若 splash 挂在主线程，Windows 会把不 pump 消息的窗口判为「无响应」。
//  ② WS_EX_LAYERED + LWA_ALPHA：用整体 alpha 做 150ms 淡出，仍走正常 WM_PAINT/GDI，
//     无需 DIB 预乘 alpha，也不需要 WIC/COM/D2D（启动早期这些都还没初始化）。
//  ③ show() 同步等待首帧提交 + DwmFlush 完成才返回，保证「双击后立即出现」。
//  ④ dismiss() 不阻塞主线程；析构兜底 join。任何失败/异常路径都不残留窗口。
//  ⑤ 固定 150ms 淡出，不受动效强度档位影响（负责人 2026-11-08 确认）；
//     出现是瞬时的，不做淡入，避免增加等待。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <latch>
#include <mutex>
#include <string>
#include <thread>

namespace sscl::ui {

class SplashScreen {
public:
    SplashScreen() = default;
    ~SplashScreen();

    SplashScreen(const SplashScreen&) = delete;
    SplashScreen& operator=(const SplashScreen&) = delete;

    // 创建并显示；返回时窗口已提交首帧（失败只记日志，绝不阻断启动）。
    void show(HINSTANCE instance);

    // 按当前主题语义色重取调色板并请求重绘（config.json 的主题偏好生效后调用）。
    void update_theme();

    // 请求关闭：提到最顶层后固定 150ms 淡出，主线程不等待。
    void dismiss();

private:
    static constexpr UINT kMsgRepaint = WM_APP + 11;
    static constexpr UINT kMsgDismiss = WM_APP + 12;
    static constexpr UINT_PTR kFadeInTimerId = 1;
    static constexpr UINT_PTR kFadeOutTimerId = 2;
    static constexpr unsigned long long kFadeInDurationMs = 90;
    static constexpr unsigned long long kFadeOutDurationMs = 150;
    static constexpr int kFadeInStartAlpha = 160;  // 首帧即可见，仅用 90ms 补齐剩余透明度

    struct Palette {
        COLORREF background = RGB(0, 0, 0);
        COLORREF border = RGB(0, 0, 0);
        COLORREF title = RGB(255, 255, 255);
        COLORREF status = RGB(255, 255, 255);
    };

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    void thread_main(HINSTANCE instance, std::latch& ready);
    void paint(HWND hwnd, HDC dc);
    void request_repaint();
    Palette snapshot_palette();

    std::atomic<HWND> m_hwnd{nullptr};  // splash 线程写；主线程只读后 PostMessage
    std::thread m_thread;
    std::mutex m_state_mutex;
    Palette m_palette;
    std::wstring m_status = L"正在启动…";
    std::atomic<bool> m_dismissed{false};

    // 以下成员仅 splash 线程访问。
    HICON m_icon = nullptr;
    UINT m_dpi = 96;
    int m_corner = 16;
    int m_current_alpha = kFadeInStartAlpha;
    int m_fade_out_from = kFadeInStartAlpha;
    bool m_fading_in = false;
    unsigned long long m_fade_in_start = 0;
    unsigned long long m_fade_out_start = 0;
};

}  // namespace sscl::ui
