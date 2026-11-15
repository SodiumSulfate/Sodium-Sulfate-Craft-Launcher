#pragma once

// 账号管理独立子窗口（D-118）：复用 AppWindow（无边框 + 磨砂 + DWM 圆角）与 Renderer，
// 内部承载 AccountDialog。关闭 = 隐藏（保留实例），下次「账号管理」直接复用。
// 归属主窗口（owned window，不占任务栏），打开时在主窗口上居中。

#include <functional>
#include <memory>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/account/account_manager.hpp"

namespace sscl::ui {

class AccountDialog;
class AppWindow;
class Renderer;

class AccountWindow {
public:
    explicit AccountWindow(std::shared_ptr<sscl::core::AccountManager> accounts);
    ~AccountWindow();

    AccountWindow(const AccountWindow&) = delete;
    AccountWindow& operator=(const AccountWindow&) = delete;

    // 打开（首次调用时创建窗口与渲染器）；owner 为主窗口（用于居中与归属）。
    // 重复调用只重新显示并刷新账号列表。
    void open(HWND owner);
    void close();
    bool is_open() const { return m_open; }

    // 账号列表/选中项在外部变化后刷新（例如配置重新加载）。
    void refresh();

    std::function<void(const std::string&)> on_selected;

private:
    bool ensure_created();
    void center_on_owner();
    float to_dip_x(float physical_x) const;
    float to_dip_y(float physical_y) const;

    HWND m_owner = nullptr;
    std::shared_ptr<sscl::core::AccountManager> m_accounts;
    std::unique_ptr<AppWindow> m_window;
    std::unique_ptr<Renderer> m_renderer;
    std::shared_ptr<AccountDialog> m_dialog;
    bool m_created = false;
    bool m_open = false;
};

}  // namespace sscl::ui
