#pragma once

// 账号管理子窗口内容（D-118）：
// - 左侧：本机账号列表（自定义行 + 滚轮滚动），底部「离线 / 正版 / 第三方」添加入口；
// - 右侧：选中账号详情 / 三种添加表单（离线、微软设备码+浏览器、外置登录）；
// - 所有账号经 AccountManager 持久化到 accounts.dat（DPAPI），切换只改选中项。
// 主题：背景 theme_background，列表 theme_container，右侧 Card(theme_surface)，
// 强调 theme_primary/primary_container，与主界面同一套语义色。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <d2d1.h>
#include <dwrite.h>

#include "core/account/account_manager.hpp"
#include "ui/controls/button.hpp"
#include "ui/controls/card.hpp"
#include "ui/controls/label.hpp"
#include "ui/controls/skin_avatar.hpp"
#include "ui/controls/text_box.hpp"
#include "ui/framework/ui_element.hpp"
#include "util/result.hpp"

namespace sscl::ui {

class AppWindow;

class AccountDialog : public std::enable_shared_from_this<AccountDialog> {
public:
    AccountDialog(AppWindow* host, std::shared_ptr<sscl::core::AccountManager> accounts);

    void layout(float width_dip, float height_dip, float dpi);
    void draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite);

    bool mouse_move(float x, float y);
    bool mouse_down(float x, float y);
    bool mouse_up(float x, float y);
    bool mouse_leave();
    bool mouse_wheel(float x, float y, float delta);
    bool handle_char(wchar_t ch);
    bool handle_key_down(unsigned long key);
    bool tick(float dt_ms);

    // 每次窗口打开时调用：重新读取账号列表并回到「详情」。
    void refresh_from_store();

    // 窗口销毁前由 AccountWindow 调用：之后所有异步回调静默丢弃（防悬挂宿主指针）。
    void detach_host();

    // ESC / 关闭按钮 / 点外部 → 由 AccountWindow 隐藏窗口。
    std::function<void()> on_close;
    // 选中账号变化（切换/新增/删除）→ 外壳持久化 config.selected_account_uuid。
    std::function<void(const std::string&)> on_selected;

private:
    enum class Pane { kDetail, kOffline, kMicrosoft, kExternal };
    enum class Field { kNone, kOfflineName, kExtServer, kExtUser, kExtPassword };

    void build_controls();
    void set_pane(Pane pane);
    void apply_visibility();
    void layout_controls();
    void layout_pane_detail(float left, float top, float right, float bottom);
    void layout_pane_offline(float left, float top, float right, float bottom);
    void layout_pane_microsoft(float left, float top, float right, float bottom);
    void layout_pane_external(float left, float top, float right, float bottom);

    void refresh_list();
    void refresh_detail();
    // D-121：后台批量拉取本机账号的皮肤 PNG（微软 / 外置登录），结果按 uuid 缓存。
    void load_account_skins();
    void set_status(const std::wstring& text, bool error = false);
    void set_field(Field field);
    void clamp_scroll();
    // 后台线程 → UI 线程的安全投递（宿主已销毁时丢弃）。
    void post(std::function<void()> fn);

    int row_hit(float x, float y) const;  // -1 = 未命中
    void select_index(int index);
    void activate_row(int index);

    void submit_offline();
    void submit_external();
    void start_ms_device_login();
    void start_ms_browser_login();
    void finish_ms_login(sscl::util::Result<sscl::core::AccountInfo> result);
    void finish_external_login(sscl::util::Result<sscl::core::AccountInfo> result);
    void notify_selected();

    void draw_header(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void draw_list(ID2D1RenderTarget* target, IDWriteFactory* dwrite);
    void draw_status(ID2D1RenderTarget* target, IDWriteFactory* dwrite);

    AppWindow* m_host = nullptr;
    std::shared_ptr<sscl::core::AccountManager> m_accounts;

    Panel m_root;
    Label* m_title = nullptr;
    Label* m_subtitle = nullptr;
    Button* m_close = nullptr;

    Label* m_list_title = nullptr;
    Button* m_add_offline = nullptr;
    Button* m_add_microsoft = nullptr;
    Button* m_add_external = nullptr;
    Label* m_list_hint = nullptr;

    Card* m_pane_card = nullptr;

    // 详情
    Label* m_detail_name = nullptr;
    Label* m_detail_type = nullptr;
    Label* m_detail_uuid = nullptr;
    Label* m_detail_server = nullptr;
    Label* m_detail_hint = nullptr;
    Button* m_detail_use = nullptr;
    Button* m_detail_relogin = nullptr;
    Button* m_detail_delete = nullptr;

    // 离线
    Label* m_offline_title = nullptr;
    Label* m_offline_hint = nullptr;
    TextBox* m_offline_name = nullptr;
    Button* m_offline_create = nullptr;
    Button* m_offline_back = nullptr;

    // 微软
    Label* m_ms_title = nullptr;
    Label* m_ms_hint = nullptr;
    Button* m_ms_device = nullptr;
    Button* m_ms_browser = nullptr;
    Label* m_ms_code = nullptr;
    Label* m_ms_status = nullptr;
    Label* m_ms_hint2 = nullptr;
    Button* m_ms_copy = nullptr;
    Button* m_ms_open = nullptr;
    Button* m_ms_cancel = nullptr;

    // 外置登录
    Label* m_ext_title = nullptr;
    Label* m_ext_hint = nullptr;
    TextBox* m_ext_server = nullptr;
    TextBox* m_ext_user = nullptr;
    TextBox* m_ext_password = nullptr;
    Button* m_ext_login = nullptr;
    Button* m_ext_back = nullptr;

    Pane m_pane = Pane::kDetail;
    Field m_field = Field::kNone;
    int m_hover_row = -1;
    int m_selected_row = -1;  // 列表里被点开查看的账号（与「启动账号」不同）
    bool m_detail_relogin_available = false;  // 仅微软/外置登录可重新登录

    float m_width_dip = 880.0f;
    float m_height_dip = 600.0f;
    float m_to_dip = 1.0f;

    // 列表几何（draw 与命中共用）
    D2D1_RECT_F m_list_rect{};
    float m_row_height = 56.0f;
    float m_scroll = 0.0f;
    float m_scroll_target = 0.0f;

    std::wstring m_status;
    bool m_status_error = false;

    // D-121：列表行头像（uuid → 皮肤 PNG 控件）。异步拉取，缓存命中即秒显；
    // 每次 refresh_from_store 递增代际，作废上一次打开窗口的在途回调。
    std::unordered_map<std::string, std::unique_ptr<SkinAvatar>> m_row_avatars;
    std::uint64_t m_skin_seq = 0;

    bool m_ms_busy = false;
    bool m_ext_busy = false;
    std::string m_device_user_code;
    std::string m_device_verify_url;
    std::string m_browser_auth_url;
    std::uint64_t m_login_seq = 0;  // 离开面板/重新发起时作废在途回调
};

}  // namespace sscl::ui
