#pragma once

// 启动页（D-013 对标 PCL PageLaunchLeft）：
// 左面板 300 DIP：账号卡片（头像+名+类型，PCL 式）+ 版本选择 + 大启动按钮（54 高）+ 状态；
// 右侧卡片：快速开始 / 版本信息。
// 覆盖式「账号管理」面板（列表/离线添加/微软登录 设备码+浏览器）。
// 启动流程在后台线程调 GameService::launch_installed，状态经 post_to_ui 回 UI 线程（§5.1）。

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "core/account/account_manager.hpp"
#include "core/game/game_service.hpp"
#include "core/game/launcher_config.hpp"
#include "ui/controls/button.hpp"
#include "ui/controls/card.hpp"
#include "ui/controls/label.hpp"
#include "ui/controls/skin_avatar.hpp"
#include "ui/controls/text_box.hpp"
#include "ui/framework/page.hpp"
#include "ui/framework/ui_element.hpp"

namespace sscl::ui {

class AppWindow;

class HomePage : public Page,
                 public std::enable_shared_from_this<HomePage> {
public:
    HomePage(AppWindow* window, std::shared_ptr<sscl::core::GameService> service,
             std::shared_ptr<sscl::core::AccountManager> accounts);

    void layout(UINT width, UINT height, UINT dpi) override;
    void draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) override;

    bool mouse_move(float x, float y) override;
    bool mouse_down(float x, float y) override;
    bool mouse_up(float x, float y) override;
    bool mouse_leave() override;
    bool handle_char(wchar_t ch) override;
    bool handle_key_down(unsigned long key) override;
    bool tick(float dt_ms) override;  // 覆盖层开合/视图切换动画（D-033）+ 控件渐变
    // D-102：仅「获取微软登录代码」属于前台等待；轮询阶段要展示验证码，不遮挡。
    bool is_busy() const override;
    std::wstring busy_text() const override;

    // 外部状态注入（AppShell 在页面切换时调用）。
    void set_installed_versions(std::vector<sscl::core::InstalledVersion> versions,
                                const std::string& selected_id);
    void set_status(const std::wstring& text);
    // Java 选择（M3）：config.json 的 java_path / java_scan_dirs 透传给启动请求。
    void set_java_settings(std::string java_path, std::vector<std::string> java_scan_dirs);
    // D-104：整套启动选项（窗口标题/自定义信息/可见性/优先级/窗口大小）。
    void set_launch_config(sscl::core::LauncherConfig config);

    // 导航/配置回调（AppShell 接线）。
    std::function<void()> on_open_downloads;
    std::function<void()> on_open_version_select;
    // D-118：账号添加入口统一到独立「账号管理」子窗口。
    std::function<void()> on_open_account_manager;
    std::function<void(const std::string& uuid)> on_account_selected;
    // 有下载任务进行中（D-020：启动与下载互斥）。
    std::function<bool()> download_busy;

    // D-118：账号在子窗口里增删/切换后，刷新左面板卡片（头像/名称/类型）。
    void refresh_accounts();

private:
    enum class OverlayMode { kHidden, kList, kDeviceLogin, kBrowserLogin };

    void start_launch();
    void apply_launcher_visibility(const sscl::core::GameProcess& process);
    void update_version_caption();
    void refresh_account_card();
    void refresh_account_rows();
    void set_overlay(OverlayMode mode);
    void layout_overlay();
    // 背景线程：微软登录（设备码/浏览器）→ 保存账号 → 回 UI。
    void start_ms_device_login();
    void start_ms_browser_login();
    void finish_ms_login(sscl::util::Result<sscl::core::AccountInfo> result);
    // 背景线程：按账号类型取皮肤（微软 profile_json / 外置登录 sessionserver）。
    void fetch_skin(const sscl::core::AccountInfo& account);

    AppWindow* m_window = nullptr;
    std::shared_ptr<sscl::core::GameService> m_service;  // 后台线程保活（防 UAF）
    std::shared_ptr<sscl::core::AccountManager> m_accounts;

    Panel m_root;
    // 左面板：账号卡
    SkinAvatar* m_avatar = nullptr;
    Label* m_account_name = nullptr;
    Label* m_account_type = nullptr;
    Button* m_switch_accounts = nullptr;  // D-118：快速切换本机已有账号
    Button* m_manage_accounts = nullptr;  // D-118：打开独立账号管理子窗口
    // 左面板：版本/启动
    Button* m_version_select = nullptr;
    Label* m_version_caption = nullptr;
    Button* m_launch = nullptr;
    Button* m_cancel_launch = nullptr;  // D-028：启动中显示“取消启动”
    Label* m_status = nullptr;

    Card* m_quick_card = nullptr;
    Label* m_quick_text = nullptr;
    Button* m_quick_download = nullptr;
    Card* m_info_card = nullptr;
    Label* m_info_version = nullptr;
    Label* m_info_type = nullptr;
    Label* m_info_java = nullptr;

    // 账号管理覆盖层（C-10；最后加入 m_root = 绘制在最上层）
    Card* m_overlay_card = nullptr;
    Label* m_overlay_title = nullptr;
    static constexpr int kMaxAccountRows = 6;
    Label* m_row_names[kMaxAccountRows] = {};
    Label* m_row_types[kMaxAccountRows] = {};
    Button* m_row_select[kMaxAccountRows] = {};
    Button* m_row_delete[kMaxAccountRows] = {};
    TextBox* m_offline_box = nullptr;
    Button* m_add_offline = nullptr;
    Button* m_add_ms_device = nullptr;
    Button* m_add_ms_browser = nullptr;
    Button* m_overlay_close = nullptr;
    Button* m_overlay_manage = nullptr;  // D-118：切到独立账号管理窗口
    // 微软登录视图（设备码/浏览器共用）
    Label* m_ms_code_label = nullptr;   // 大号 user_code
    Label* m_ms_status = nullptr;
    Button* m_ms_copy = nullptr;
    Button* m_ms_open = nullptr;
    Button* m_ms_cancel = nullptr;

    OverlayMode m_overlay = OverlayMode::kHidden;
    float m_overlay_anim = 0.0f;      // 覆盖层开合 0..1（遮罩+缩放+淡入淡出，D-033）
    float m_overlay_view_anim = 1.0f; // 覆盖层内视图切换淡入 0..1（D-033）
    std::string m_device_user_code;
    std::string m_device_verify_url;
    std::string m_browser_auth_url;
    // 构造期推迟的皮肤拉取（shared_from_this 不可用），保留完整账号信息：
    // 外置登录需要 server_url + uuid 才能查 Yggdrasil 皮肤资料。
    std::optional<sscl::core::AccountInfo> m_pending_skin_account;
    std::uint64_t m_skin_seq = 0;        // 皮肤拉取代际（删除/切换账号后在途回调作废，D-032）

    std::vector<sscl::core::InstalledVersion> m_installed;
    std::string m_selected_id;
    bool m_launching = false;
    bool m_ms_code_loading = false;  // D-102：设备码请求中（拿到 user_code 即结束）
    std::stop_source m_launch_stop;  // D-028：启动取消
    std::string m_java_path;                 // M3：config.json java_path
    std::vector<std::string> m_java_scan_dirs;  // M3：config.json java_scan_dirs
    sscl::core::LauncherConfig m_launch_config;  // D-104

    float m_to_dip = 1.0f;
    float m_width_dip = 0.0f;
    float m_height_dip = 0.0f;
};

}  // namespace sscl::ui
