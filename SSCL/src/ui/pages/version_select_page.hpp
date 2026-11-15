#pragma once

// 版本选择页（V2，PCL PageSelectLeft + PageSelectRight 对齐）：
// - 左侧 MC 文件夹列表（当前/官启/自定义；添加/重命名/移出/删除/刷新/切换）；
// - 右侧版本分类卡片（收藏/API/常规/不常用/愚人节/错误/隐藏）；
// - 单击版本=选中返回；齿轮/右键=打开版本详情（V3 接入）；
// - 行内星标/删除；F11 隐藏版本；空状态。
// 不再使用“管理版本”模式，也没有列表外的“切换隔离”按钮。

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include "core/game/game_service.hpp"
#include "core/game/instance_registry.hpp"
#include "ui/controls/smooth_scroll.hpp"
#include "ui/controls/text_box.hpp"
#include "ui/framework/motion.hpp"
#include "ui/framework/page.hpp"

namespace sscl::ui {

class AppWindow;

class VersionSelectPage : public Page,
                          public std::enable_shared_from_this<VersionSelectPage> {
public:
    VersionSelectPage(AppWindow* window, std::shared_ptr<sscl::core::GameService> service);

    void layout(UINT width, UINT height, UINT dpi) override;
    void draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) override;

    bool mouse_move(float x, float y) override;
    bool mouse_down(float x, float y) override;
    bool mouse_up(float x, float y) override;
    bool mouse_right_down(float x, float y) override;
    bool mouse_leave() override;
    bool mouse_wheel(float x, float y, float delta) override;
    bool handle_char(wchar_t ch) override;
    bool handle_key_down(unsigned long key) override;
    bool tick(float dt_ms) override;
    // D-102：前台等待由外壳统一绘制转圈。
    bool is_busy() const override;
    std::wstring busy_text() const override;

    void refresh(const std::string& selected_id);
    void set_folders(std::vector<sscl::core::McFolderInfo> folders);
    void set_message(std::wstring message);

    // 回调（AppShell 接线）。
    std::function<void(const std::string& version_id)> on_select;
    std::function<void(const std::string& version_id)> on_open_instance;
    std::function<void()> on_back;
    std::function<void()> on_open_downloads;
    std::function<void(const std::filesystem::path& folder)> on_switch_folder;
    std::function<void(const std::filesystem::path&, const std::string& name)> on_add_folder;
    std::function<void(const std::filesystem::path&, const std::string& name)> on_rename_folder;
    std::function<void(const std::filesystem::path&)> on_remove_folder;
    std::function<void(const std::filesystem::path&)> on_delete_folder;
    std::function<void(const std::filesystem::path&)> on_refresh_folder;

private:
    // 交互热区（轮次 115）：与首页一致，悬停/按下/选中三态各有明确配色。
    enum class Hot {
        kNone,
        kFolder,
        kFolderGear,
        kRow,
        kRowStar,
        kRowDelete,
        kRowGear,
        kCardHeader,
        kAddFolder,
        kImport,
        kBack,
        kDownload,
    };
    Hot hit_hot(float x_dip, float y_dip, std::string* row_id, int* folder_index,
                int* card_index) const;

    struct FolderHit {
        std::filesystem::path path;
        D2D1_RECT_F row{};
        D2D1_RECT_F gear{};
    };
    struct RowHit {
        std::string id;
        D2D1_RECT_F row{};
        D2D1_RECT_F star{};
        D2D1_RECT_F del{};
        D2D1_RECT_F gear{};
    };
    struct CardHit {
        int category = 0;
        D2D1_RECT_F header{};
    };
    struct CardLayout {
        int category = 0;
        bool expanded = false;
        float reveal = 1.0f;     // 0..1：展开进度（决定内容区可见高度）
        float body_bottom = 0.f;  // 内容区下边界（含展开进度），行命中不得越过它
        D2D1_RECT_F header{};
        std::vector<RowHit> rows;
    };

    void rebuild_cards();
    void rebuild_hits();
    void rebuild_folder_hits();
    void close_overlays();
    void open_input(std::wstring prompt, std::wstring initial,
                    std::function<void(const std::wstring&)> accept);
    void open_delete_version(const std::string& id, bool permanent);
    void open_delete_folder(const std::filesystem::path& path);
    void layout_overlay();
    void draw_overlay(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void draw_folder_list(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void draw_cards(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void draw_folder_menu(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    std::wstring version_info_text(const sscl::core::VersionSummary& summary) const;
    std::wstring state_text(sscl::core::InstanceState state) const;
    bool overlay_hit(float x_dip, float y_dip) const;

    AppWindow* m_window = nullptr;
    std::shared_ptr<sscl::core::GameService> m_service;

    std::vector<sscl::core::McFolderInfo> m_folders;
    std::vector<sscl::core::InstanceCard> m_cards;
    std::map<int, bool> m_expanded;
    std::string m_selected_id;
    bool m_show_hidden = false;

    std::vector<FolderHit> m_folder_hits;
    std::vector<CardLayout> m_card_layout;
    std::vector<CardHit> m_card_hits;
    D2D1_RECT_F m_add_folder_rect{};
    D2D1_RECT_F m_create_folder_rect{};
    D2D1_RECT_F m_import_rect{};
    D2D1_RECT_F m_back_rect{};
    D2D1_RECT_F m_download_rect{};
    bool m_show_create_folder = false;

    // 文件夹齿轮菜单
    bool m_folder_menu_open = false;
    std::filesystem::path m_folder_menu_path;
    D2D1_RECT_F m_folder_menu_rect{};
    int m_folder_menu_hover = -1;
    int m_folder_menu_type = 0;

    // 覆盖层：删除确认 / 输入框
    enum class OverlayKind { kNone, kDelete, kInput };
    OverlayKind m_overlay = OverlayKind::kNone;
    D2D1_RECT_F m_overlay_card{};
    D2D1_RECT_F m_overlay_ok{};
    D2D1_RECT_F m_overlay_cancel{};
    int m_overlay_hover = 0;  // 0=无 1=取消 2=确定
    std::wstring m_overlay_prompt;
    std::wstring m_overlay_text;
    bool m_overlay_permanent = false;
    bool m_delete_folder = false;
    std::string m_delete_version_id;
    std::filesystem::path m_delete_folder_path;
    std::function<void(const std::wstring&)> m_input_accept;
    std::unique_ptr<TextBox> m_input_box;

    Hot m_hover = Hot::kNone;
    Hot m_pressed = Hot::kNone;
    std::string m_hover_row_id;
    int m_hover_folder = -1;
    int m_hover_card = -1;

    std::wstring m_message;
    // D-089：整合包导入（.mrpack / CurseForge manifest）。
    void start_import_modpack();
    bool m_importing = false;
    std::stop_source m_import_stop;
    SmoothScroll m_scroll;
    // D-093：分类卡片展开/收起的 0..1 进度（按 category 存），tick 里逼近目标值。
    std::map<int, float> m_expand_anim;

    float m_to_dip = 1.0f;
    float m_width_dip = 0.0f;
    float m_height_dip = 0.0f;
    float m_list_top = 0.0f;
    float m_list_bottom = 0.0f;
};

}  // namespace sscl::ui
