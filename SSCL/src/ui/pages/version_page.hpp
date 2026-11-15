#pragma once

// 版本下载页（§3 页面层 / D-013）：左侧面板（标题/刷新/状态）+ 右侧版本清单列表（滚动）。
// 清单/安装经 GameService（DownloadScheduler 官方→BMCLAPI 回退）；后台线程安装，
// 状态更新经 AppWindow::post_to_ui 回 UI 线程（§5.1）。

#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include "core/game/game_service.hpp"
#include "core/game/version_list.hpp"
#include "core/loader/loader_catalog.hpp"
#include "core/loader/loader_types.hpp"
#include "ui/controls/button.hpp"
#include "ui/controls/card.hpp"
#include "ui/controls/label.hpp"
#include "ui/controls/smooth_scroll.hpp"
#include "ui/framework/page.hpp"
#include "ui/framework/ui_element.hpp"

namespace sscl::ui {

class AppWindow;

class VersionPage : public Page,
                    public std::enable_shared_from_this<VersionPage> {
public:
    VersionPage(AppWindow* window, std::shared_ptr<sscl::core::GameService> service);

    void layout(UINT width, UINT height, UINT dpi) override;
    void draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) override;

    bool mouse_move(float x, float y) override;
    bool mouse_down(float x, float y) override;
    bool mouse_up(float x, float y) override;
    bool mouse_leave() override;
    bool mouse_wheel(float x, float y, float delta) override;  // 仅滚动区内有效（D-035）
    bool tick(float dt_ms) override;  // 控件渐变 + 滚动趋近 + 分类切换淡入（D-036）+ 弹层（D-040）
    // D-102：前台等待由外壳统一绘制转圈。
    bool is_busy() const override;
    std::wstring busy_text() const override;

    // 后台拉取版本清单。
    void refresh();

    // 已有清单数据（外壳判断是否需要自动刷新）。
    bool loaded() const { return !m_entries.empty(); }

    // 点击行内“下载”后：原版 = 直接 install_full（既有行为）。
    std::function<void(const sscl::core::VersionListEntry& entry)> on_start_install;
    // 组件选择弹层确认（含加载器）：转任务页 install_with_loader（D-040/D-044）。
    std::function<void(const sscl::core::InstallJob& job)> on_start_loader_job;

private:
    // 版本类型分类（D-036）：稳定版=release / 快照版=snapshot（含 pre/RC）/
    // 远古版=old_beta+old_alpha；kAll=全部不过滤。
    enum class VersionCategory { kAll, kRelease, kSnapshot, kAncient };
    static constexpr int kCategoryCount = 4;

    void draw_row(ID2D1RenderTarget* target, IDWriteFactory* dwrite, std::size_t index,
                  float y);
    bool hit_download(std::size_t index, float x_dip, float y_dip) const;
    // 滚动区命中（D-035）：列表视口 + 右缘滚动条带；侧栏等区域返回 false。
    bool list_viewport_hit(float x_dip, float y_dip) const;
    // 分类切换（D-036）：重建过滤索引 + 回顶 + 内容淡入动画。
    void apply_category(VersionCategory category);
    // 依当前分类重建 m_filtered（清单原序新→旧）。
    void rebuild_filter();

    // ---- 组件选择弹层（D-040） ----
    static constexpr int kLoaderKindCount = 4;
    void open_picker(const sscl::core::VersionListEntry& entry);
    void close_picker();
    void select_loader_kind(sscl::core::LoaderKind kind);
    void refresh_loader_versions();
    void start_picker_install();
    void layout_picker();
    void draw_picker(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    bool picker_card_hit(float x_dip, float y_dip) const;
    bool picker_list_hit(float x_dip, float y_dip) const;
    bool picker_row_hit(std::size_t index, float x_dip, float y_dip) const;

    AppWindow* m_window = nullptr;
    std::shared_ptr<sscl::core::GameService> m_service;  // 后台线程保活（防 UAF）

    Panel m_sidebar;
    Label* m_heading = nullptr;
    Label* m_subtitle = nullptr;
    Button* m_refresh = nullptr;
    Button* m_category_buttons[kCategoryCount] = {};  // 分类导航（D-036）
    VersionCategory m_category = VersionCategory::kAll;

    std::vector<sscl::core::VersionListEntry> m_entries;  // 清单全量（新→旧）
    std::vector<std::size_t> m_filtered;                  // 当前分类的 m_entries 索引（D-036）
    std::wstring m_status = L"就绪";
    bool m_loading = false;
    std::size_t m_hover_row = static_cast<std::size_t>(-1);  // 过滤列表内行号

    float m_to_dip = 1.0f;
    float m_width_dip = 0.0f;
    float m_height_dip = 0.0f;
    SmoothScroll m_scroll_view;  // D-022：丝滑滚动 + 滚动条
    float m_filter_anim = 1.0f;  // 分类切换内容淡入 0..1（D-033/D-036）
    bool m_dirty = false;        // mouse 事件后置位

    // 弹层状态（D-040）
    std::unique_ptr<Card> m_picker_owner;  // 唯一所有权；m_picker_card 为裸指针视图
    Card* m_picker_card = nullptr;
    Label* m_picker_title = nullptr;
    Label* m_picker_game = nullptr;
    Label* m_picker_hint = nullptr;
    Label* m_picker_status = nullptr;
    Button* m_picker_kinds[kLoaderKindCount] = {};  // 原版 / Fabric / Forge / NeoForge
    Button* m_picker_cancel = nullptr;
    Button* m_picker_start = nullptr;
    SmoothScroll m_picker_scroll;
    std::vector<sscl::core::LoaderEntry> m_picker_entries;
    std::size_t m_picker_selected = static_cast<std::size_t>(-1);
    std::size_t m_picker_hover = static_cast<std::size_t>(-1);
    sscl::core::LoaderKind m_picker_kind = sscl::core::LoaderKind::kNone;
    sscl::core::VersionListEntry m_picker_entry;
    bool m_picker_has_entry = false;
    bool m_picker_open = false;
    bool m_picker_loading = false;
    bool m_picker_confirm = false;  // Forge/NeoForge：第三方代码二次确认（D-040）
    // 请求代际（D-051）：换游戏版本/加载器类型/关闭弹层即自增；异步清单结果必须同时匹配
    // 代际与 MC，否则旧游戏版本的结果会覆盖当前列表。
    std::uint64_t m_picker_generation = 0;
    std::wstring m_picker_message;  // 加载/失败提示
    float m_picker_anim = 0.0f;       // 开合 0..1（160ms，D-033）
    float m_picker_view_anim = 1.0f;  // 视图内容淡入（120ms，D-033）
};

}  // namespace sscl::ui
