#pragma once

// 设置页（M5，对标 PCL 2.99 PageSetup*）：左侧分类 + 右侧滚动设置列表。
// 设置项全部落到 LauncherConfig（config.json），由 AppShell 落盘并即时生效：
// - 游戏：版本隔离默认策略、当前游戏文件夹；
// - Java：java.exe 路径、额外扫描目录；
// - 外观：主题偏好（跟随系统 / 浅色 / 深色，切换立即换肤）；
// - 关于：版本号、数据/日志目录。
// 视觉沿用 V4 版本设置页：分类胶囊 + 56 高设置行 + 覆盖式输入框 + 下拉选择。

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/game/game_service.hpp"
#include "core/game/launcher_config.hpp"
#include "ui/controls/smooth_scroll.hpp"
#include "ui/controls/text_box.hpp"
#include "ui/framework/motion.hpp"
#include "ui/framework/page.hpp"
#include "ui/framework/tooltip.hpp"
#include "platform/system_monitor.hpp"

namespace sscl::ui {

class AppWindow;

// D-110：代理连通性测试在后台线程执行，需要 shared_from_this 保活页面。
class SettingsPage : public Page,
                     public std::enable_shared_from_this<SettingsPage> {
public:
    SettingsPage(AppWindow* window, std::shared_ptr<sscl::core::GameService> service,
                 sscl::core::LauncherConfig config,
                 std::function<void(const sscl::core::LauncherConfig&)> on_config_changed);

    void layout(UINT width, UINT height, UINT dpi) override;
    void draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) override;

    bool mouse_move(float x, float y) override;
    bool mouse_down(float x, float y) override;
    bool mouse_up(float x, float y) override;
    bool mouse_leave() override;
    bool mouse_wheel(float x, float y, float delta) override;
    bool handle_char(wchar_t ch) override;
    bool handle_key_down(unsigned long key) override;
    bool tick(float dt_ms) override;

    // 进入页面时由外壳同步最新配置（其它页面可能改过选中版本/账号）。
    void set_config(const sscl::core::LauncherConfig& config);
    void set_message(std::wstring message);

private:
    enum class Category { kGame, kJava, kAppearance, kNetwork, kOther, kAbout };
    enum class RowKind {
        kSection, kChoice, kText, kAction, kInfo,
        kToggle,   // D-107：勾选框（辅助功能）
        kMemory,   // D-107：内存分配条 + 自定义滑块
        kProbeLog, // D-113：网络诊断结果面板（纯底色多行文本框）
        kJavaRuntime,  // D-115：Java 运行时行（↑ ↓ − 打开）
        kScanDirEntry, // D-115：扫描位置行（来源标签 + 删除）
        kDualAction,   // D-115：一行两个动作按钮
    };

    struct TabHit {
        Category category = Category::kGame;
        D2D1_RECT_F rect{};
    };
    struct RowHit {
        RowKind kind = RowKind::kInfo;
        int key = 0;
        int index = -1;              // D-115：列表项下标（Java 运行时 / 扫描位置）
        std::wstring text;
        D2D1_RECT_F row{};
        D2D1_RECT_F control{};
        D2D1_RECT_F control2{};      // D-115：双按钮行的第二个按钮
        D2D1_RECT_F buttons[4]{};    // D-115：Java 行的 ↑ ↓ − 打开
    };

    void rebuild_tabs();
    void rebuild_rows();
    void draw_tabs(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void draw_rows(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void draw_choice(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void draw_overlay(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void layout_overlay();
    // 第三方说明 / 隐私政策：只读长文弹层（可滚动）。
    void open_doc(int key);
    void close_doc_overlay();
    void layout_doc_overlay();
    void draw_doc_overlay(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void open_input(std::wstring prompt, std::wstring initial,
                    std::function<void(const std::wstring&)> accept);
    void close_overlay();
    void open_choice(int key);
    std::vector<std::pair<int, std::wstring>> choice_items(int key) const;
    void apply_choice(int key, int value);
    void activate_row(const RowHit& hit);
    void commit();
    // D-115：Java 运行时（有序）与扫描位置。
    struct JavaEntry {
        std::string path;     // bin 目录，与 java_candidates 一一对应
        int major = 0;        // 0 = 尚未探测
        std::string version;  // 如 21.0.12.1
    };
    void probe_java_candidates();
    void refresh_java();
    void move_java(int index, int delta);
    void remove_java(int index);
    void open_java_folder(int index);
    void add_scan_dir();
    void auto_scan_dirs();
    void remove_scan_dir(int index);

    std::wstring row_label(int key) const;
    std::wstring row_desc(int key) const;
    std::wstring row_value(int key) const;
    int row_value_int(int key) const;
    std::wstring row_action(int key) const;
    std::wstring category_label(Category category) const;
    int hit_tab(float x, float y) const;
    int hit_row(float x, float y) const;

    // D-107：内存分配条与自定义滑块。
    int memory_slider_max_mb() const;
    int effective_memory_mb() const;
    void set_memory_from_slider(float x_dip);
    // D-110：用当前代理配置发一个测试请求，结果回到状态栏。
    void test_proxy();

    // D-111/D-113：一键网络诊断。entries 顺序与 cpp 的 kProbeTargets 一致。
    struct ProbeResult {
        long long connect_ms = -1;  // DNS + TCP（与 curl time_connect 同口径）
        long long total_ms = -1;    // 到响应头；<0 = 超时/失败
        bool done = false;
    };
    struct NetDiagnostics {
        std::vector<ProbeResult> entries;
        std::wstring exit_ip;
        bool exit_ip_done = false;
        bool started = false;  // 点过「开始测试」后为 true（未测试时显示占位文案）
        bool running = false;
    };
    void run_network_diagnostics();

    AppWindow* m_window = nullptr;
    std::shared_ptr<sscl::core::GameService> m_service;
    sscl::core::LauncherConfig m_config;
    std::function<void(const sscl::core::LauncherConfig&)> m_on_config_changed;

    Category m_category = Category::kGame;
    std::vector<TabHit> m_tabs;
    std::vector<RowHit> m_rows;

    // 下拉选择（沿用版本设置页范式）
    bool m_choice_open = false;
    int m_choice_key = -1;
    int m_choice_hover = -1;
    D2D1_RECT_F m_choice_rect{};
    std::vector<int> m_choice_values;
    std::vector<std::wstring> m_choice_labels;

    // 覆盖式输入框
    bool m_input_open = false;
    D2D1_RECT_F m_overlay_card{};
    D2D1_RECT_F m_overlay_ok{};
    D2D1_RECT_F m_overlay_cancel{};
    std::wstring m_overlay_prompt;
    std::function<void(const std::wstring&)> m_input_accept;
    std::unique_ptr<TextBox> m_input_box;

    // 第三方说明 / 隐私政策弹层
    bool m_doc_open = false;
    bool m_doc_mode = false;  // 淡出期间仍绘制文档内容
    std::wstring m_doc_title;
    std::wstring m_doc_text;
    D2D1_RECT_F m_doc_view{};
    D2D1_RECT_F m_doc_close{};
    float m_doc_content_height = 0.0f;
    SmoothScroll m_doc_scroll;

    int m_hover_tab = -1;
    int m_hover_row = -1;
    int m_hover_button = 0;  // 行内按钮悬停（0/1）

    std::wstring m_message;
    SmoothScroll m_scroll;
    // D-104：仅「默认版本隔离」「游戏窗口标题」两行有悬浮提示。
    Tooltip m_tooltip;

    float m_to_dip = 1.0f;
    float m_width_dip = 0.0f;
    float m_height_dip = 0.0f;
    float m_content_left = 0.0f;
    float m_content_right = 0.0f;
    float m_view_top = 0.0f;
    float m_view_bottom = 0.0f;
    std::filesystem::path m_data_dir;
    std::filesystem::path m_game_dir;

    // D-107：「其他」页每秒采样的内存状态与滑块交互。
    sscl::platform::SystemMonitor m_monitor;
    sscl::platform::SystemLoad m_load;
    float m_sample_accum_ms = 0.0f;
    D2D1_RECT_F m_memory_slider_rect{};
    bool m_memory_dragging = false;
    bool m_proxy_testing = false;  // D-110
    NetDiagnostics m_net;          // D-111
    int m_probe_spinner = 0;       // D-113：|/-\ 循环帧
    float m_probe_spinner_ms = 0.0f;

    // D-115：Java 版本缓存（与 m_config.java_candidates 同序，仅内存，不落盘）。
    std::vector<JavaEntry> m_java_entries;
    bool m_java_scanning = false;
    bool m_java_probe_started = false;
};

}  // namespace sscl::ui
