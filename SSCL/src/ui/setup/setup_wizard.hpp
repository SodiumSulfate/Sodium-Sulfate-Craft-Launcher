#pragma once

// 首次启动指引（D-124）：主窗口内的全屏覆盖层，5 步引导设置主题 / Java / CurseForge Key。
//
// 设计要点：
// - 复用主窗口的 Renderer 与输入回调，**不创建第二个窗口/D3D 设备**，
//   非首次启动时只有一个空指针判断，不给启动路径引入任何额外延迟。
// - 底部按钮：左侧「上一步」，右侧「跳过此步」+「下一步」；第一页没有上一步，
//   「跳过向导」直接结束整个向导；最后一页不显示跳过。
// - 第三页进入即在后台线程扫描 java.exe（`detect_java_in_dirs`），
//   结果按「越靠前越优先」排序写入 LauncherConfig::java_candidates；
//   列表使用 SmoothScroll（右侧滚动条 + 帧率无关平滑滚动）。
// - 所有改动经 on_apply 回调即时落盘并同步到 AppShell（设置真的已应用）。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <d2d1.h>
#include <dwrite.h>

#include "core/game/launcher_config.hpp"
#include "ui/controls/button.hpp"
#include "ui/controls/label.hpp"
#include "ui/controls/smooth_scroll.hpp"
#include "ui/controls/text_box.hpp"
#include "ui/framework/ui_element.hpp"

namespace sscl::ui {

class AppWindow;

class SetupWizard : public std::enable_shared_from_this<SetupWizard> {
public:
    SetupWizard(AppWindow* window, sscl::core::LauncherConfig config,
                std::filesystem::path secrets_file,
                std::function<void(const sscl::core::LauncherConfig&)> on_apply);
    ~SetupWizard();

    SetupWizard(const SetupWizard&) = delete;
    SetupWizard& operator=(const SetupWizard&) = delete;

    // 坐标为 DIP；dpi 用于把后续物理像素输入换算回 DIP。
    void layout(float width_dip, float height_dip, float dpi);
    void draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite);

    // 鼠标入参是物理像素（与 AppShell 一致）；返回是否需要重绘。
    bool mouse_move(float x_physical, float y_physical);
    bool mouse_down(float x_physical, float y_physical);
    bool mouse_up(float x_physical, float y_physical);
    bool mouse_wheel(float x_physical, float y_physical, float delta);
    bool mouse_leave();
    bool handle_char(wchar_t ch);
    bool handle_key_down(unsigned long key);
    bool tick(float dt_ms);

    bool active() const { return m_active; }
    void dismiss();  // 关闭覆盖层（完成 / 跳过整个向导）

private:
    enum class Step {
        kWelcome = 0,
        kTheme,
        kJava,
        kCurseForge,
        kDone,
    };
    static constexpr int kStepCount = 5;

    struct JavaEntry {
        std::string path;
        int major = 0;
        std::string version;
    };
    struct RowHit {
        D2D1_RECT_F up{};
        D2D1_RECT_F down{};
        int index = -1;
    };

    void build_controls();
    void set_step(Step step);
    void apply_visibility();
    void layout_controls();
    void draw_card(ID2D1RenderTarget* target) const;
    void draw_progress(ID2D1RenderTarget* target) const;
    void draw_java_list(ID2D1RenderTarget* target, IDWriteFactory* dwrite);

    void next_step();
    void previous_step();
    void skip_current();
    void finish();
    void commit();
    void choose_theme(const std::string& key);
    void refresh_theme_buttons();
    void start_java_scan();
    void sync_java_config();
    void move_java(int index, int delta);
    void save_curseforge_key();
    int row_hit(float x_dip, float y_dip, bool* up) const;

    AppWindow* m_window = nullptr;
    sscl::core::LauncherConfig m_config;
    std::filesystem::path m_secrets_file;
    std::function<void(const sscl::core::LauncherConfig&)> m_on_apply;

    bool m_active = true;
    Step m_step = Step::kWelcome;
    float m_width_dip = 1024.0f;
    float m_height_dip = 640.0f;
    float m_to_dip = 1.0f;
    D2D1_RECT_F m_card{};
    D2D1_RECT_F m_list_rect{};
    float m_row_height = 44.0f;
    SmoothScroll m_java_scroll;
    int m_hover_row = -1;

    // Java 扫描
    bool m_java_started = false;
    bool m_scanning = false;
    std::vector<JavaEntry> m_java_entries;
    std::vector<RowHit> m_row_hits;
    std::uint64_t m_scan_seq = 0;  // 窗口销毁后在途回调作废

    Panel m_root;
    Label* m_title = nullptr;
    Label* m_subtitle = nullptr;
    Button* m_primary = nullptr;
    Button* m_skip = nullptr;
    Button* m_back = nullptr;

    // 第 1 页
    Label* m_intro1 = nullptr;
    Label* m_intro2 = nullptr;
    Label* m_intro3 = nullptr;
    // 第 2 页
    Button* m_theme_system = nullptr;
    Button* m_theme_light = nullptr;
    Button* m_theme_dark = nullptr;
    Label* m_theme_hint = nullptr;
    // 第 3 页
    Label* m_java_status = nullptr;
    Label* m_java_hint = nullptr;
    // 第 4 页
    TextBox* m_key_box = nullptr;
    Label* m_key_emphasis = nullptr;
    Label* m_key_hint = nullptr;
    // 第 5 页
    Label* m_done1 = nullptr;
    Label* m_done2 = nullptr;
};

}  // namespace sscl::ui
