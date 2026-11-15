#include "ui/account/account_window.hpp"

#include <algorithm>
#include <utility>

#include "ui/account/account_dialog.hpp"
#include "ui/framework/renderer.hpp"
#include "ui/framework/window.hpp"
#include "util/log.hpp"

namespace sscl::ui {

namespace {
constexpr int kDialogWidthDip = 880;
constexpr int kDialogHeightDip = 600;
}  // namespace

AccountWindow::AccountWindow(std::shared_ptr<sscl::core::AccountManager> accounts)
    : m_accounts(std::move(accounts)) {}

AccountWindow::~AccountWindow() {
    // 先断开对话框对宿主窗口的引用：异步登录回调可能比窗口活得更久。
    if (m_dialog) {
        m_dialog->detach_host();
    }
}

bool AccountWindow::ensure_created() {
    if (m_created) {
        return true;
    }
    if (!m_accounts) {
        return false;
    }
    m_window = std::make_unique<AppWindow>();
    util::Result<void> created =
        m_window->create(L"账号管理", kDialogWidthDip, kDialogHeightDip);
    if (created.is_error()) {
        util::log_error("[account] create window failed: " + created.error().detail);
        m_window.reset();
        return false;
    }
    HWND hwnd = m_window->hwnd();
    // 归属主窗口：不占任务栏、随主窗口最小化。
    if (m_owner != nullptr) {
        ::SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(m_owner));
    }
    const LONG_PTR ex_style = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                        (ex_style & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW)) |
                            static_cast<LONG_PTR>(WS_EX_TOOLWINDOW));

    m_renderer = std::make_unique<Renderer>();
    util::Result<void> graphics = m_renderer->initialize(hwnd, m_window->dpi());
    if (graphics.is_error()) {
        util::log_error("[account] init renderer failed: " + graphics.error().detail);
        m_renderer.reset();
        m_window.reset();
        return false;
    }

    m_dialog = std::make_shared<AccountDialog>(m_window.get(), m_accounts);
    m_dialog->on_close = [this]() { close(); };
    m_dialog->on_selected = [this](const std::string& uuid) {
        if (on_selected) {
            on_selected(uuid);
        }
    };

    m_window->on_close_request = [this]() {
        close();
        return true;  // 保留窗口实例
    };
    m_window->on_paint = [this]() {
        m_renderer->render_frame([this](ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
            m_dialog->draw(target, dwrite);
        });
    };
    m_window->on_resize = [this](UINT width, UINT height) {
        m_renderer->resize(width, height);
        const float dpi = static_cast<float>(m_window->dpi());
        const float to_dip = dpi <= 0.0f ? 1.0f : 96.0f / dpi;
        m_dialog->layout(static_cast<float>(width) * to_dip,
                         static_cast<float>(height) * to_dip, dpi);
        m_window->invalidate();
    };
    m_window->on_mouse_move = [this](float x, float y) {
        if (m_dialog->mouse_move(to_dip_x(x), to_dip_y(y))) {
            m_window->start_frame_loop();
            m_window->invalidate();
        }
    };
    m_window->on_mouse_down = [this](float x, float y) {
        // 按下只改变按钮按压态/输入焦点：一次重绘足够。
        // 这里不再启动帧循环——每帧都要 Present(vsync)，点一下白等一帧会让按压反馈发黏。
        if (m_dialog->mouse_down(to_dip_x(x), to_dip_y(y))) {
            m_window->invalidate();
        }
    };
    m_window->on_mouse_up = [this](float x, float y) {
        m_dialog->mouse_up(to_dip_x(x), to_dip_y(y));
        m_window->invalidate();
    };
    m_window->on_mouse_leave = [this]() {
        m_dialog->mouse_leave();
        m_window->invalidate();
    };
    m_window->on_mouse_wheel = [this](float x, float y, float delta) {
        if (m_dialog->mouse_wheel(to_dip_x(x), to_dip_y(y), delta)) {
            m_window->start_frame_loop();
            m_window->invalidate();
        }
    };
    m_window->on_char = [this](wchar_t ch) {
        if (m_dialog->handle_char(ch)) {
            m_window->invalidate();
        }
    };
    m_window->on_key_down = [this](unsigned long key) {
        if (m_dialog->handle_key_down(key)) {
            m_window->invalidate();
        }
    };
    m_window->on_frame = [this](UINT dt_ms) {
        const bool active = m_dialog->tick(static_cast<float>(dt_ms));
        return active;
    };

    RECT client{};
    ::GetClientRect(hwnd, &client);
    const float dpi = static_cast<float>(m_window->dpi());
    const float to_dip = dpi <= 0.0f ? 1.0f : 96.0f / dpi;
    m_dialog->layout(static_cast<float>(client.right - client.left) * to_dip,
                     static_cast<float>(client.bottom - client.top) * to_dip, dpi);

    m_created = true;
    return true;
}

void AccountWindow::center_on_owner() {
    if (!m_created || m_window == nullptr) {
        return;
    }
    HWND hwnd = m_window->hwnd();
    RECT target{};
    if (m_owner != nullptr && ::GetWindowRect(m_owner, &target) != FALSE) {
        // 用主窗口所在显示器的工作区约束。
        const HMONITOR monitor = ::MonitorFromWindow(m_owner, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (monitor != nullptr && ::GetMonitorInfoW(monitor, &info) != FALSE) {
            const RECT& work = info.rcWork;
            target.left = (std::max)(work.left, (std::min)(target.left, work.right));
            target.top = (std::max)(work.top, (std::min)(target.top, work.bottom));
        }
    } else {
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &target, 0);
    }
    RECT self{};
    if (::GetWindowRect(hwnd, &self) == FALSE) {
        return;
    }
    const int width = self.right - self.left;
    const int height = self.bottom - self.top;
    const int x = target.left + ((target.right - target.left) - width) / 2;
    const int y = target.top + ((target.bottom - target.top) - height) / 2;
    ::SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void AccountWindow::open(HWND owner) {
    m_owner = owner;
    if (!ensure_created()) {
        return;
    }
    if (m_owner != nullptr) {
        ::SetWindowLongPtrW(m_window->hwnd(), GWLP_HWNDPARENT,
                            reinterpret_cast<LONG_PTR>(m_owner));
    }
    m_dialog->refresh_from_store();
    center_on_owner();
    // show() 会先渲染并提交首帧再显示窗口（AppWindow D-034，避免白闪）。
    m_window->show(SW_SHOW);
    ::SetForegroundWindow(m_window->hwnd());
    m_open = true;
    m_window->start_frame_loop();
    m_window->invalidate();
}

void AccountWindow::close() {
    if (!m_created || m_window == nullptr) {
        m_open = false;
        return;
    }
    m_window->hide_window();
    m_open = false;
}

void AccountWindow::refresh() {
    if (m_dialog) {
        m_dialog->refresh_from_store();
        if (m_window != nullptr) {
            m_window->invalidate();
        }
    }
}

float AccountWindow::to_dip_x(float physical_x) const {
    const float dpi = (m_window != nullptr) ? static_cast<float>(m_window->dpi()) : 96.0f;
    return dpi <= 0.0f ? physical_x : physical_x * 96.0f / dpi;
}

float AccountWindow::to_dip_y(float physical_y) const {
    return to_dip_x(physical_y);
}

}  // namespace sscl::ui
