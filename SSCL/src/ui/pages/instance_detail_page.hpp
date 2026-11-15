#pragma once

// 版本详情页（V3，PCL PageInstanceLeft/PageInstanceSetup 对齐）：
// - 标题“版本设置 - <名称>”，左侧导航：概览 / 设置 / 修改 / Mod 管理 / 导出；
// - V3 实现概览：版本头、收藏、分类、重命名、描述、图标、隔离、打开文件夹、删除；
// - 设置页 V4；修改/Mod/导出为禁用占位。

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <wrl/client.h>

#include "core/game/game_service.hpp"
#include "core/mod/mod_scanner.hpp"
#include "ui/controls/smooth_scroll.hpp"
#include "ui/controls/text_box.hpp"
#include "ui/framework/motion.hpp"
#include "ui/framework/page.hpp"

namespace sscl::ui {

class AppWindow;

class InstanceDetailPage : public Page {
public:
    InstanceDetailPage(AppWindow* window, std::shared_ptr<sscl::core::GameService> service);

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

    void set_instance(const std::string& version_id);
    void set_message(std::wstring message);

    std::function<void()> on_back;
    std::function<void(const std::string& old_id, const std::string& new_id)> on_renamed;
    std::function<void(const std::string& version_id)> on_deleted;
    std::function<void()> on_changed;
    std::function<void()> on_open_downloads;

private:
    enum class Tab { kOverview, kSettings, kModify, kMods, kExport };
    enum class Overlay { kNone, kInput, kDelete, kIsolation, kReset, kModDelete };
    enum class SettingKind { kSection, kToggle, kText, kChoice, kNumber, kAction };

    struct TabHit {
        Tab tab = Tab::kOverview;
        D2D1_RECT_F rect{};
        bool enabled = true;
    };
    struct SettingHit {
        SettingKind kind = SettingKind::kText;
        int key = 0;
        std::wstring text;
        D2D1_RECT_F row{};
        D2D1_RECT_F control{};
        D2D1_RECT_F minus{};
        D2D1_RECT_F plus{};
    };
    struct ModHit {
        std::string file;
        std::string name;
        bool enabled = false;
        D2D1_RECT_F row{};
        D2D1_RECT_F toggle{};
        D2D1_RECT_F del{};
    };

    void reload();
    bool load_summary();
    void rebuild_hits();
    void rebuild_settings_hits();
    void draw_overview(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void draw_settings(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void draw_modify(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void draw_mods(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void draw_export(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void refresh_mods();
    void rebuild_mod_hits();
    void toggle_mod(std::size_t index);
    void delete_mod(std::size_t index);
    void import_mods();
    void repair_files();
    void export_modpack();
    void draw_overlay(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void layout_overlay();
    void open_input(std::wstring prompt, std::wstring initial,
                    std::function<void(const std::wstring&)> accept);
    void open_delete();
    void open_isolation();
    void open_reset();
    void close_overlay();
    void open_choice(int key);
    void apply_choice(int key, int value);
    void open_setting_text(int key);
    void toggle_setting(int key);
    void persist_settings();
    void draw_logo(ID2D1RenderTarget* target, IDWriteFactory* dwrite,
                   const D2D1_RECT_F& rect);
    void ensure_logo_bitmap(ID2D1RenderTarget* target);
    std::wstring category_text() const;
    std::wstring state_text() const;
    std::wstring setting_value(int key) const;
    std::wstring setting_label(int key) const;
    std::wstring setting_desc(int key) const;
    // 开关类设置的当前布尔值（供绘制与动画统一取值）。
    bool setting_toggle_value(int key) const;
    // 当前 tab 下所有可交互矩形（tab / 返回 / 按钮 / 行内图标），用于 hover/pressed 命中。
    std::vector<D2D1_RECT_F> hot_rects() const;

    AppWindow* m_window = nullptr;
    std::shared_ptr<sscl::core::GameService> m_service;

    std::string m_version_id;
    sscl::core::VersionSummary m_summary;
    sscl::core::InstanceSettings m_settings;
    bool m_loaded = false;
    Tab m_tab = Tab::kOverview;

    std::vector<TabHit> m_tabs;
    std::vector<SettingHit> m_setting_hits;
    std::vector<ModHit> m_mod_hits;
    std::vector<sscl::core::LocalModEntry> m_mods;
    std::filesystem::path m_mods_dir;
    std::wstring m_mod_error;
    D2D1_RECT_F m_back_rect{};
    D2D1_RECT_F m_star_rect{};
    D2D1_RECT_F m_category_rect{};
    D2D1_RECT_F m_rename_rect{};
    D2D1_RECT_F m_desc_rect{};
    D2D1_RECT_F m_logo_rect{};
    D2D1_RECT_F m_isolation_rect{};
    D2D1_RECT_F m_open_folder_rect{};
    D2D1_RECT_F m_shortcut_rect{};
    D2D1_RECT_F m_delete_rect{};
    D2D1_RECT_F m_import_mods_rect{};
    D2D1_RECT_F m_refresh_mods_rect{};
    D2D1_RECT_F m_repair_rect{};
    D2D1_RECT_F m_download_page_rect{};
    D2D1_RECT_F m_export_rect{};
    D2D1_RECT_F m_open_folder_export_rect{};

    // 交互状态（轮次 115）：矩形相等即命中，draw_button 据此决定 hover/pressed 配色。
    D2D1_RECT_F m_hover_rect{};
    D2D1_RECT_F m_pressed_rect{};

    bool m_category_menu_open = false;
    D2D1_RECT_F m_category_menu_rect{};
    int m_category_hover = -1;

    bool m_choice_open = false;
    int m_choice_key = -1;
    int m_choice_hover = -1;
    D2D1_RECT_F m_choice_rect{};
    std::vector<int> m_choice_values;
    std::vector<std::wstring> m_choice_labels;

    Overlay m_overlay = Overlay::kNone;
    D2D1_RECT_F m_overlay_card{};
    D2D1_RECT_F m_overlay_ok{};
    D2D1_RECT_F m_overlay_cancel{};
    std::wstring m_overlay_prompt;
    std::wstring m_overlay_text;
    bool m_delete_permanent = false;
    std::filesystem::path m_mod_delete_path;
    std::function<void(const std::wstring&)> m_input_accept;
    std::unique_ptr<TextBox> m_input_box;

    bool m_busy = false;
    std::wstring m_busy_text;

    std::wstring m_message;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_logo_bitmap;
    std::string m_logo_loaded_path;

    float m_to_dip = 1.0f;
    float m_width_dip = 0.0f;
    float m_height_dip = 0.0f;
    SmoothScroll m_scroll;
    // D-093：tab 切换只淡入内容区（tab 栏保持稳定），并带上 4px 上移。
    float m_tab_fade = 1.0f;
    // D-093：开关控件的滑块/底色过渡进度（key → 0..1）。
    std::map<int, float> m_toggle_anim;
};

}  // namespace sscl::ui
