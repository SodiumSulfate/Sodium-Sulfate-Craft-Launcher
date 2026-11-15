#pragma once

// 应用外壳（D-013：GUI 对标 PCL2；D-016 无边框+磨砂）：
// - 顶部导航条兼任自绘标题栏（启动 / 下载 / 设置 + 右上 最小化/最大化/关闭）
// - 页面管理与导航回调（首页 ↔ 版本选择 ↔ 下载）。
// 业务逻辑在 core；本类只做布局、绘制与路由。

#include <functional>
#include <memory>
#include <string>

#include <d2d1.h>
#include <wrl/client.h>

#include "core/account/account_manager.hpp"
#include "core/game/game_service.hpp"
#include "core/game/instance_registry.hpp"
#include "core/game/launcher_config.hpp"
#include "platform/system_monitor.hpp"
#include "ui/account/account_window.hpp"
#include "ui/framework/page.hpp"
#include "ui/pages/addons_page.hpp"
#include "ui/pages/home_page.hpp"
#include "ui/pages/instance_detail_page.hpp"
#include "ui/pages/settings_page.hpp"
#include "ui/pages/task_page.hpp"
#include "ui/pages/version_page.hpp"
#include "ui/pages/version_select_page.hpp"

namespace sscl::ui {

class AppWindow;

enum class ShellPage {
    kHome,
    kVersionSelect,  // 启动页的“版本选择”子页（导航仍高亮“启动”）
    kInstanceDetail, // 版本详情（V3；导航仍高亮“启动”）
    kDownloads,
    kTasks,      // 下载任务页（D-019；PCL 任务管理对照，导航不高亮）
    kAddons,     // 附加页（D-057：Mod 搜索/安装）
    kSettings,
};

class AppShell {
public:
    // initial_config：启动页初始玩家名/选中版本；on_config_changed：任一项变化时回调（存盘）。
    AppShell(AppWindow* window, std::shared_ptr<sscl::core::GameService> service,
             std::shared_ptr<sscl::core::AccountManager> accounts,
             sscl::core::LauncherConfig initial_config,
             std::function<void(const sscl::core::LauncherConfig&)> on_config_changed);
    // 任务中心是进程级单例：外壳销毁时摘掉监听，避免回调打到已释放的窗口。
    ~AppShell();

    void layout(UINT width, UINT height, UINT dpi);
    void draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite);

    // 物理像素坐标；返回是否有视觉变化。
    bool mouse_move(float x, float y);
    bool mouse_down(float x, float y);
    bool mouse_up(float x, float y);
    bool mouse_right_down(float x, float y);
    bool mouse_leave();
    bool mouse_wheel(float x, float y, float delta);
    bool handle_char(wchar_t ch);
    bool handle_key_down(unsigned long key);

    // 动画帧（D-020）：页面切换滑动 / hover 渐变 / 悬浮窗弹出；返回是否仍在动画。
    bool tick(UINT dt_ms);

    void switch_page(ShellPage page);
    ShellPage current_page() const { return m_current; }

    // 配置变更统一入口：同步到所有页面、刷新全局策略并立即落盘。
    // D-124：首次启动向导完成/跳步时也经此应用（此前只供设置页使用）。
    void apply_settings(const sscl::core::LauncherConfig& updated);

    // 固定 0.1s 系统状态刷新（CPU/RAM）；由 AppWindow 的 WM_TIMER 驱动。
    void on_status_timer();

private:
    void draw_nav_bar(ID2D1RenderTarget* target, IDWriteFactory* dwrite) const;
    void draw_title_buttons(ID2D1RenderTarget* target) const;
    void draw_float_widget(ID2D1RenderTarget* target, IDWriteFactory* dwrite) const;
    // D-102：前台等待统一转圈指示器（内容之后、导航条之前绘制）。
    void draw_busy_indicator(ID2D1RenderTarget* target, IDWriteFactory* dwrite,
                             const std::wstring& text) const;
    bool nav_hit(float x_dip, float y_dip, int* index) const;
    static int title_button_hit(float x_dip, float y_dip, float width_dip);
    bool float_widget_visible() const;
    D2D1_RECT_F float_widget_rect() const;
    bool float_widget_hit(float x_dip, float y_dip) const;
    Page* current();
    Page* page_for(ShellPage page) const;
    void notify_config_changed();
    void refresh_home_state();
    void refresh_version_folders();

    AppWindow* m_window = nullptr;
    std::shared_ptr<sscl::core::GameService> m_service;
    sscl::core::LauncherConfig m_config;
    std::function<void(const sscl::core::LauncherConfig&)> m_on_config_changed;

    ShellPage m_current = ShellPage::kHome;

    std::shared_ptr<HomePage> m_home;
    std::shared_ptr<VersionSelectPage> m_version_select;
    std::shared_ptr<InstanceDetailPage> m_instance_detail;
    std::shared_ptr<VersionPage> m_downloads;
    std::shared_ptr<TaskPage> m_tasks;
    std::shared_ptr<AddonsPage> m_addons;
    std::shared_ptr<SettingsPage> m_settings;

    // D-118：账号统一由独立子窗口管理（主页「账号管理」打开）。
    std::shared_ptr<sscl::core::AccountManager> m_accounts;
    std::unique_ptr<AccountWindow> m_account_window;
    void open_account_manager();

    // 系统负载（CPU/RAM）：0.1s 采样一次，转发给任务页左侧简略信息。
    sscl::platform::SystemMonitor m_system_monitor;
    sscl::platform::SystemLoad m_system_load;

    // D-114：采样由帧循环驱动（WM_TIMER 在 60fps 自驱渲染下会被 WM_PAINT 饿死）。
    // on_status_timer 用同一时间戳去重，仅在帧循环停止时兜底。
    void refresh_system_stats();
    float m_system_accum_ms = 0.0f;
    unsigned long long m_last_system_sample_tick = 0;

    int m_hover_nav = -1;
    int m_hover_title_button = -1;  // 0=min 1=max 2=close
    bool m_hover_float = false;     // 右下下载悬浮窗

    // 动画状态（D-020）
    ShellPage m_prev_page = ShellPage::kHome;
    float m_page_anim = 1.0f;         // 0→1：新页覆盖淡入进度
    bool m_page_anim_active = false;
    // 切换后的第一帧不推进进度：点击处理可能阻塞数十毫秒，
    // 若直接按大 dt 步进会把 180ms 的过渡一帧走完（旧页瞬间消失 → 闪黑）。
    bool m_page_anim_fresh = false;
    float m_nav_hover_anim[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float m_title_hover_anim[3] = {0.0f, 0.0f, 0.0f};
    float m_float_anim = 0.0f;        // 悬浮窗弹出
    float m_float_hover_anim = 0.0f;
    // D-102：上一帧的忙碌状态，用于「刚进入/刚结束等待」时强制刷新一帧。
    bool m_busy_prev = false;
    // D-105：输入事件后的续帧保活计数（见 AppShell::tick）。
    int m_keep_frames = 0;

    float m_to_dip = 1.0f;
    float m_width_dip = 0.0f;
    float m_height_dip = 0.0f;

    // 导航栏 logo 图标（resources/sscl.rc 的 IDR_SSCL_APP_ICON，D-030）
    mutable Microsoft::WRL::ComPtr<ID2D1Bitmap> m_logo_bitmap;
    mutable ID2D1RenderTarget* m_logo_target = nullptr;
};

}  // namespace sscl::ui
