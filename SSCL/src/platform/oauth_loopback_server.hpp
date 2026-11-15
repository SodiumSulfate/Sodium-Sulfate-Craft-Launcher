#pragma once

// 本地 OAuth 回调服务（浏览器授权码流程，C-5 端口 23380-23385）：
// 监听 127.0.0.1，接收 /auth-response?code=...&state=...，返回成功页后关闭。
// 阻塞式 wait_for_code（带超时），供后台登录线程使用；线程安全关闭。
// 注意：socket 细节不进头文件（winsock2 与 windows.h 的包含序在 UI TU 中易冲突）。

#include <atomic>
#include <cstdint>
#include <string>

#include "util/result.hpp"

namespace sscl::platform {

class OAuthLoopbackServer {
public:
    OAuthLoopbackServer();
    ~OAuthLoopbackServer();

    OAuthLoopbackServer(const OAuthLoopbackServer&) = delete;
    OAuthLoopbackServer& operator=(const OAuthLoopbackServer&) = delete;

    // 绑定 127.0.0.1 首个可用端口（23380-23385）；成功后 redirect_uri() 可用。
    util::Result<void> start(const std::string& expected_state);

    // 等待回调（毫秒超时）。Ok(code)；超时/关闭返回 http.loopback_timeout 等。
    util::Result<std::string> wait_for_code(int timeout_ms);

    // 提前停止（可从任意线程调用）。
    void stop();

    // 形如 http://localhost:23380/auth-response。
    std::string redirect_uri() const { return m_redirect_uri; }
    int port() const { return m_port; }

private:
    std::uintptr_t m_listen = 0;  // SOCKET（0 = 未监听）
    std::atomic<bool> m_stopped{false};
    std::string m_expected_state;
    std::string m_redirect_uri;
    int m_port = 0;
};

}  // namespace sscl::platform
