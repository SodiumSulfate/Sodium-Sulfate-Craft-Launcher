#pragma once

// 下载任务页（批次 42 重写）：
// - 左栏：总进度 / 下载速度 / 剩余文件 + 状态 + 取消/返回（沿用 PCL PageSpeedLeft 结构）；
// - 右栏：apt 风格终端输出（TerminalView：彩色 Get/阶段/Fetched 行）
//         + 底部实时区（当前步骤、总进度条、最多 6 条活动文件进度条、速度、ETA）。
//
// 性能约束（负责人 2026-11-01 要求）：
// - 进度回调约 10Hz（ProgressReporter 节流），只在数据变化时 request_repaint；
//   进度条不做 60fps tween，只有终端滚轮滚动才驱动帧循环；
// - 日志为有界环形缓冲（2000 行）；离开页面立即收缩到 200 行并停止追加事件；
// - 活动文件表只保存当前正在下载的文件（完成/取消即移除），不保留历史文件列表。

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include "core/game/game_service.hpp"
#include "core/game/version_list.hpp"
#include "core/task/task_center.hpp"
#include "platform/system_monitor.hpp"
#include "ui/controls/button.hpp"
#include "ui/controls/label.hpp"
#include "ui/controls/terminal_view.hpp"
#include "ui/framework/page.hpp"
#include "ui/framework/ui_element.hpp"

namespace sscl::ui {

class AppWindow;

class TaskPage : public Page, public std::enable_shared_from_this<TaskPage> {
public:
    TaskPage(AppWindow* window, std::shared_ptr<sscl::core::GameService> service);

    void layout(UINT width, UINT height, UINT dpi) override;
    void draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) override;

    bool mouse_move(float x, float y) override;
    bool mouse_down(float x, float y) override;
    bool mouse_up(float x, float y) override;
    bool mouse_leave() override;
    bool mouse_wheel(float x, float y, float delta) override;
    bool tick(float dt_ms) override;  // 终端滚动动画（进度条不做 tween）

    // 启动安装任务（后台线程）：job.loader.kind == kNone 即原版安装（D-040/D-044）。
    void start_install(const sscl::core::InstallJob& job);
    bool is_running() const { return m_running; }

    // D-098：是否有「后台任务中心」的任务在跑（附加页 Mod 下载/安装）。
    // 与 is_running() 分开：版本安装的互斥判断只认前台任务。
    bool has_background_job() const;

    // 页面可见性（AppShell 切换页面时调用）：隐藏时释放日志/活动文件表。
    void set_visible(bool visible);

    // 0.1s 系统状态（CPU/RAM）刷新；由 AppShell 从 AppWindow 定时器转发。
    void set_system_stats(const sscl::platform::SystemLoad& load);

    // 悬浮窗数据（外壳右下角悬浮按钮；D-019/PCL BtnExtraDownload）。
    float progress_fraction() const;
    std::wstring activity_title() const;   // "正在下载 <id>" / 状态文本
    std::wstring activity_detail() const;  // "42 %"

    // 返回下载页回调（AppShell 接线）。
    std::function<void()> on_back;
    // 任务状态变化（开始/结束），外壳用于悬浮窗弹出/收起动画（D-020）。
    std::function<void()> on_state_changed;
    // 取消已确认（线程返回 cancelled），返回上一页面（D-023）。
    std::function<void()> on_task_cancelled;
    // 安装成功完成：自动返回上一页面（D-047；PCL 下载完成即关闭任务页的行为）。
    std::function<void()> on_task_done;

private:
    enum class TaskState {
        kIdle,
        kRunning,
        kDone,
        kFailed,
        kCancelled,
    };

    // 活动文件（只存"正在下载"的；速度由 UI 侧按 10Hz 快照做 EMA）。
    struct ActiveFile {
        std::wstring name;
        std::uint64_t bytes = 0;
        std::uint64_t total = 0;  // 0 = 未知
        double speed_bps = 0.0;
        std::uint64_t last_bytes = 0;
        std::chrono::steady_clock::time_point last_time{};
        bool has_sample = false;
    };

    void on_progress(const sscl::core::InstallProgress& report);
    void append_log(std::wstring text, TerminalTone tone);
    void sync_active_files(const sscl::core::InstallProgress& report);
    void reset_task_state();
    void update_labels();

    // —— 后台任务（D-098）——
    // 轮询任务中心 → 合成 InstallProgress 复用既有进度/终端/速度链路。
    void poll_background();
    void adopt_background_job(const sscl::core::JobSnapshot& snapshot);
    void publish_background_progress(const sscl::core::JobSnapshot& snapshot);
    void finish_background_job(const sscl::core::JobSnapshot& snapshot);
    void update_system_labels();
    void draw_system_icons(ID2D1RenderTarget* target) const;
    // CPU/RAM 迷你历史柱状图（同一套绘制，非线性幅度让低占用也可见）。
    void draw_history_graph(ID2D1RenderTarget* target, const std::deque<float>& history,
                            const D2D1_RECT_F& rect) const;

    void draw_progress_bar(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float fraction,
                           float height, bool strong) const;
    float total_fraction() const;
    std::wstring eta_text() const;
    std::size_t visible_file_rows() const;
    // 非下载步骤（安装器处理器/Java 检测）：step 条 + 循环动画。
    bool step_active() const;
    bool step_animating() const;
    void draw_step_bar(ID2D1RenderTarget* target) const;

    AppWindow* m_window = nullptr;
    std::shared_ptr<sscl::core::GameService> m_service;

    Panel m_root;
    Label* m_title = nullptr;
    Label* m_progress_caption = nullptr;
    Label* m_progress_value = nullptr;
    Label* m_speed_caption = nullptr;
    Label* m_speed_value = nullptr;
    Label* m_files_caption = nullptr;
    Label* m_files_value = nullptr;
    Label* m_status_label = nullptr;
    // 左侧简略系统信息（CPU/RAM；0.1s 固定刷新，图标自绘）
    Label* m_cpu_label = nullptr;
    Label* m_cpu_percent_label = nullptr;
    Label* m_ram_label = nullptr;
    Label* m_ram_percent_label = nullptr;
    double m_cpu_percent = 0.0;
    double m_ram_percent = 0.0;
    std::uint64_t m_ram_used_bytes = 0;
    std::uint64_t m_ram_total_bytes = 0;
    std::deque<float> m_cpu_history;  // 最近 24 次采样（0..100），用于迷你柱状图
    std::deque<float> m_ram_history;  // 内存占用历史（与 CPU 同一套统计图表）
    bool m_system_visible = false;
    float m_system_top = 0.0f;        // layout 计算；draw 复用
    Button* m_cancel = nullptr;
    Button* m_back = nullptr;

    // 右栏（终端标题 + 底部实时区文本）
    Label* m_terminal_title = nullptr;
    Label* m_terminal_meta = nullptr;
    Label* m_step_label = nullptr;
    Label* m_overall_label = nullptr;
    Label* m_speed_label = nullptr;
    Label* m_more_label = nullptr;
    Label* m_file_name[6] = {};
    Label* m_file_stats[6] = {};

    TerminalView m_terminal;
    std::map<std::uint64_t, ActiveFile> m_files;
    // 按剩余字节排序后的活动文件顺序（update_labels 维护；draw 直接复用，
    // 避免每帧重新排序/分配）。
    std::vector<std::uint64_t> m_file_order;

    sscl::core::InstallProgress m_report;
    TaskState m_state = TaskState::kIdle;
    std::wstring m_status = L"暂无下载任务";
    std::string m_entry_id;
    std::stop_source m_stop;
    bool m_running = false;
    bool m_visible = false;          // 页面是否可见（AppShell 维护）
    bool m_skipped_while_hidden = false;

    std::wstring m_last_phase;
    std::wstring m_last_detail;
    std::uint64_t m_get_seq = 0;

    // 速度采样（EMA；与 PCL 0.1s 刷新对照）
    double m_speed_bps = 0.0;
    std::uint64_t m_last_bytes = 0;
    std::chrono::steady_clock::time_point m_last_sample{};
    bool m_has_sample = false;
    std::chrono::steady_clock::time_point m_started_at{};
    // 隐藏时仅以 5Hz 驱动悬浮窗进度，避免在其它页面触发 10Hz 全页重绘。
    std::chrono::steady_clock::time_point m_last_hidden_repaint{};

    int m_file_rows = 6;
    // 步骤（处理器/Java 检测）循环动画相位，0..1400ms。
    float m_activity_phase = 0.0f;

    // 后台任务显示状态（D-098）
    std::uint64_t m_bg_job = 0;      // 正在展示的后台任务 id（0 = 未接管）
    bool m_bg_active = false;        // 该任务仍在运行（取消按钮据此启用）
    std::string m_bg_last_file;      // 上一帧的当前文件（用于补 Get: 行）
    // D-100：多文件并行后 current_file 会在文件间跳动，用「已出现过的文件」去重，
    // 每个文件只输出一行 Get:（否则终端会被交替刷屏）。
    std::vector<std::string> m_bg_files_seen;
    float m_bg_poll_ms = 0.0f;       // 轮询节流（≈16Hz）

    float m_to_dip = 1.0f;
    float m_width_dip = 0.0f;
    float m_height_dip = 0.0f;
    D2D1_RECT_F m_panel{};
    D2D1_RECT_F m_zone{};
};

}  // namespace sscl::ui
