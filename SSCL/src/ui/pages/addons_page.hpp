#pragma once

// 「附加」页（PCL 2.99 PageDownloadMod / PageResource 对齐）：
// 无左侧栏；**整页一条滚动轴**（D-096）——「搜索 Mod」筛选卡片位于内容最上方，
// 随页面一起滚动（不再吸附在顶部）；其下为全宽结果卡片 + 分页条，两种模式（搜索/本地）
// 都从筛选卡片开始。下载语义仍为 PCL：另存为 + 智能默认目录 + 前置仅展示；主题色一律走 theme_*()。
// D-098：安装/下载一律提交到 TaskCenter（后台任务），并先弹出「依赖勾选」弹层——
// 列出将要下载的 Mod 与必需前置，默认勾选尚未安装的项。
// 依据：For REA/PCL-src/PageResource.xaml、MyResourceItem.xaml、ResourceProject.vb、
//       ResourceSearcher.vb、PageDownloadMod.xaml、PageDownloadCompDetail.xaml.vb。

#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include "core/game/game_service.hpp"
#include "core/game/launcher_config.hpp"
#include "core/mod/mod_download_planner.hpp"
#include "core/mod/mod_installer.hpp"
#include "core/mod/mod_update.hpp"
#include "core/modplatform/mod_catalog.hpp"
#include "core/modplatform/mod_types.hpp"
#include "core/task/task_center.hpp"
#include "ui/controls/button.hpp"
#include "ui/controls/card.hpp"
#include "ui/controls/label.hpp"
#include "ui/controls/smooth_scroll.hpp"
#include "ui/controls/text_box.hpp"
#include "ui/framework/motion.hpp"
#include "ui/framework/page.hpp"
#include "ui/framework/ui_element.hpp"

namespace sscl::ui {

class AppWindow;

class AddonsPage : public Page, public std::enable_shared_from_this<AddonsPage> {
public:
    AddonsPage(AppWindow* window, std::shared_ptr<sscl::core::GameService> service,
               sscl::core::LauncherConfig config);

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
    // D-102：前台等待由外壳统一绘制转圈。
    bool is_busy() const override;
    std::wstring busy_text() const override;

    // 平台切换后由外壳持久化到 config。
    std::function<void(sscl::core::ModPlatform)> on_platform_changed;
    // 首次进入页面时由外壳调用：PCL 行为 = 自动加载一次结果（无搜索词即下载排行）。
    void on_enter();
    // D-106：每次进入本页重新解析 CurseForge Key（设置页可能刚改过自定义 Key）。
    void refresh_curseforge_key();
    // 由外壳 0.1s 定时器驱动：临时状态（如「已加入后台任务…」）到期自动隐藏。
    // 返回 true 表示文案有变化（外壳需要重绘一次）。
    bool tick_status(float dt_ms);

private:
    static constexpr float kRowHeight = 64.0f;   // PCL MyResourceItem Height
    static constexpr float kLogoSize = 50.0f;   // PCL MyResourceItem PathLogo
    static constexpr int kPageSize = 40;        // PCL PageResource.PAGE_SIZE

    enum class Mode { kSearch, kLocal };
    enum class Dropdown { kNone, kSource, kVersion, kLoader, kTag };

    // —— 布局与绘制 ——
    void layout_filter();
    void layout_search();   // 结果区滚动布局（本地管理链路沿用此名）
    void layout_cards();
    void draw_filter_card(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void draw_dropdown(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void draw_pager(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void draw_hint(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void draw_status(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void draw_row(ID2D1RenderTarget* target, IDWriteFactory* dwrite, std::size_t index, float y);
    void draw_local_row(ID2D1RenderTarget* target, IDWriteFactory* dwrite, std::size_t index,
                        float y);

    // —— 筛选 ——
    void reset_filter();
    void open_dropdown(Dropdown kind);
    void rebuild_dropdown_items();
    void apply_dropdown(std::size_t index);
    void close_dropdown();
    void start_search();
    void change_page(int delta);
    // ttl_ms > 0：消息在到期后自动恢复默认提示（D-099，避免「已加入后台任务」常驻）。
    void set_status(std::wstring text, float ttl_ms = 0.0f);
    std::wstring default_status_text() const;
    void set_mode(Mode mode);
    bool ensure_target(std::wstring* error);
    bool resolve_selected(sscl::core::InstanceTarget* target, std::wstring* error);

    // —— 命中 ——
    // D-096：整页滚动后，屏幕坐标 → 内容坐标（含滚动偏移）。
    float content_y(float y_dip) const { return y_dip + m_scroll.offset(); }
    bool list_viewport_hit(float x_dip, float y_dip) const;
    bool row_hit(float x_dip, float y_dip, std::size_t* index) const;
    bool local_row_hit(float x_dip, float y_dip, std::size_t* index) const;
    int filter_hit(float x_dip, float y_dip) const;  // 1..9 = 顶部控件
    int pager_hit(float x_dip, float y_dip) const;   // 1 上一页 / 2 下一页

    // —— 本地 Mod 管理 ——
    void refresh_local();
    void toggle_selected_mod();
    void delete_selected_mod();
    void import_local_mods();
    void check_local_updates();
    void apply_local_updates();

    // —— 详情 / 另存为 ——
    void open_detail(std::size_t index);
    void rebuild_version_rows();
    void select_detail_version(std::size_t index);
    void draw_detail_extras(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void start_prepare_install();
    // D-088：解析 required 依赖 → build_install_plan → 打开确认弹层（此前 m_plan 恒空）。
    void start_install_to_instance();
    void confirm_install();
    void execute_install();
    void cancel_install();
    void close_overlay();
    void close_detail();

    // —— 依赖勾选弹层 / 后台任务（D-098）——
    void open_dependency_dialog(sscl::core::ModInstallPlan plan, std::wstring warnings,
                                std::string target_id);
    void layout_dependency_dialog();
    void draw_dependency_dialog(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void draw_dependency_rows(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    int dependency_row_hit(float x_dip, float y_dip) const;  // -1 = 未命中
    void toggle_dependency(std::size_t index);
    void set_all_dependencies(bool checked);
    void update_dependency_summary();
    void start_dependency_install();
    void close_dependency_dialog();
    // 提交到任务中心：安装计划 / 单文件下载（另存为）。
    void submit_mod_install(const sscl::core::ModInstallPlan& plan, const std::string& target_id);

    AppWindow* m_window = nullptr;
    std::shared_ptr<sscl::core::GameService> m_service;
    sscl::core::LauncherConfig m_config;
    std::unique_ptr<sscl::core::ModCatalog> m_catalog;
    std::string m_catalog_key;  // D-106：最近一次建 catalog 时使用的 Key

    std::string m_target_version_id;
    sscl::core::ModPlatform m_platform = sscl::core::ModPlatform::kModrinth;
    Mode m_mode = Mode::kSearch;
    bool m_auto_loaded = false;
    std::wstring m_target_text;
    std::wstring m_status = L"输入关键词搜索 Mod，或点击「下载排行」。";

    // 保留：本地管理链路与状态标签依赖（不再作为左侧栏使用）
    Panel m_sidebar;             // 仅承载本地 Mod 操作按钮，本地模式显示于顶部操作条
    TextBox m_search;            // PCL「名称」输入框
    Label* m_status_label = nullptr;
    Label* m_target_label = nullptr;
    Button* m_local_toggle = nullptr;
    Button* m_local_delete = nullptr;
    Button* m_local_import = nullptr;
    Button* m_local_check = nullptr;
    Button* m_local_apply = nullptr;

    // PCL 筛选条件
    TextBox m_version;
    std::wstring m_source_text = L"Modrinth";
    std::wstring m_loader_text = L"任意 Mod 加载器";
    std::wstring m_tag_text = L"全部";
    sscl::core::ModLoader m_filter_loader = sscl::core::ModLoader::kUnknown;
    std::string m_category;     // Modrinth slug
    std::string m_category_cf;  // CurseForge categoryId
    int m_source_index = 0;
    int m_loader_index = 0;
    int m_tag_index = 0;
    Dropdown m_dropdown = Dropdown::kNone;
    std::vector<std::wstring> m_dropdown_labels;
    std::vector<D2D1_RECT_F> m_dropdown_items;
    D2D1_RECT_F m_dropdown_rect{};
    float m_dropdown_offset = 0.0f;
    int m_dropdown_hover = -1;

    // 结果
    std::vector<sscl::core::ModProject> m_results;
    std::size_t m_hover_row = static_cast<std::size_t>(-1);
    SmoothScroll m_scroll;
    std::uint64_t m_search_generation = 0;
    bool m_searching = false;
    bool m_ranking = true;  // 无搜索词 = PCL 默认下载量排行
    std::wstring m_warning;
    int m_page = 0;
    bool m_has_more = false;

    // 顶部控件矩形
    D2D1_RECT_F m_card_rect{};
    D2D1_RECT_F m_name_rect{};
    D2D1_RECT_F m_version_rect{};
    D2D1_RECT_F m_source_rect{};
    D2D1_RECT_F m_loader_rect{};
    D2D1_RECT_F m_tag_rect{};
    D2D1_RECT_F m_btn_search{};
    D2D1_RECT_F m_btn_reset{};
    D2D1_RECT_F m_btn_ranking{};
    D2D1_RECT_F m_btn_local{};
    D2D1_RECT_F m_results_rect{};
    D2D1_RECT_F m_pager_prev{};
    D2D1_RECT_F m_pager_next{};

    // 本地 Mod 管理
    std::vector<sscl::core::ModUpdateInfo> m_local_items;
    std::size_t m_local_selected = static_cast<std::size_t>(-1);
    bool m_local_loading = false;
    std::wstring m_local_status;
    std::stop_source m_local_stop;

    // 详情弹层
    std::unique_ptr<Card> m_detail_owner;
    Card* m_detail_card = nullptr;
    Label* m_detail_title = nullptr;
    Label* m_detail_summary = nullptr;
    Label* m_detail_meta = nullptr;
    Label* m_detail_message = nullptr;
    Button* m_detail_install = nullptr;        // 另存为…（PCL 原始语义，保留）
    Button* m_detail_install_to_instance = nullptr;  // 安装到实例（D-088：P0 死链路修复）
    Button* m_detail_close = nullptr;
    sscl::core::ModProject m_detail_project;
    sscl::core::ModVersion m_detail_version;
    sscl::core::ModPlatform m_detail_platform = sscl::core::ModPlatform::kModrinth;
    std::vector<sscl::core::ModVersion> m_detail_versions;
    std::size_t m_detail_version_index = 0;
    int m_hover_version = -1;
    std::vector<D2D1_RECT_F> m_version_rows;
    D2D1_RECT_F m_detail_icon_rect{};
    bool m_detail_open = false;
    bool m_detail_loading = false;
    std::uint64_t m_detail_generation = 0;

    // 安装确认弹层（保留既有链路）
    std::unique_ptr<Card> m_confirm_owner;
    Card* m_confirm_card = nullptr;
    Label* m_confirm_title = nullptr;
    Label* m_confirm_detail = nullptr;
    Label* m_confirm_message = nullptr;
    Button* m_confirm_ok = nullptr;
    Button* m_confirm_cancel = nullptr;
    sscl::core::ModInstallPlan m_plan;
    sscl::core::ModInstallOptions m_plan_options;
    bool m_confirm_open = false;
    bool m_confirm_closing = false;  // 关闭淡出期间为 true（D-088）
    bool m_preparing = false;

    bool m_installing = false;
    std::stop_source m_install_stop;
    std::wstring m_install_message;
    std::string m_cached_folder;

    // —— 依赖勾选弹层（D-098）——
    // 一行 = 一个将要下载的 Mod/前置；勾选决定实际安装哪些。
    struct DepRow {
        std::wstring title;
        std::wstring version;
        std::wstring tag;      // 前置 / 已安装 / 同名文件 / 不可用
        std::uint64_t size = 0;
        bool checked = true;
        bool enabled = true;   // 不可用项禁用勾选
        std::size_t plan_index = 0;
    };
    std::unique_ptr<Card> m_dep_owner;
    Card* m_dep_card = nullptr;
    Label* m_dep_title = nullptr;
    Label* m_dep_hint = nullptr;
    Button* m_dep_all = nullptr;      // 全选 / 全不选
    Button* m_dep_start = nullptr;    // 开始下载（后台任务）
    Button* m_dep_cancel = nullptr;
    std::vector<DepRow> m_dep_rows;
    SmoothScroll m_dep_scroll;
    D2D1_RECT_F m_dep_list_rect{};
    bool m_dep_open = false;
    bool m_dep_closing = false;
    float m_dep_close_ms = 0.0f;  // 关闭动画计时（兜底：动画状态异常也能收尾）
    bool m_dep_all_checked = true;
    std::wstring m_dep_warnings;
    std::string m_dep_target_id;

    float m_to_dip = 1.0f;
    float m_width_dip = 0.0f;
    float m_height_dip = 0.0f;
    float m_hover_anim = 0.0f;
    bool m_dirty = false;
    // 状态消息自动隐藏（D-099）：ttl=0 表示常驻。
    float m_status_ttl_ms = 0.0f;
    float m_status_elapsed_ms = 0.0f;
};

}  // namespace sscl::ui
