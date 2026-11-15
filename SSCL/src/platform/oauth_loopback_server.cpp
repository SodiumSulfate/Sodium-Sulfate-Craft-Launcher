#include "platform/oauth_loopback_server.hpp"

// winsock2 必须先于 windows.h（否则 winsock.h 冲突）。
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstring>
#include <mutex>
#include <string>

#include "util/log.hpp"

#pragma comment(lib, "ws2_32.lib")

namespace sscl::platform {

namespace {

constexpr int kPortFirst = 23380;  // C-5
constexpr int kPortLast = 23385;
constexpr const char* kPath = "/auth-response";

SOCKET as_socket(std::uintptr_t value) {
    return static_cast<SOCKET>(value);
}

// 从 "GET /auth-response?code=..&state=.. HTTP/1.1" 提取查询参数。
bool query_param(const std::string& request, const std::string& name, std::string* out) {
    const std::size_t qpos = request.find('?');
    if (qpos == std::string::npos) {
        return false;
    }
    const std::size_t end = request.find(' ', qpos);
    const std::string query =
        request.substr(qpos + 1, end == std::string::npos ? end : end - qpos - 1);
    std::size_t pos = 0;
    while (pos < query.size()) {
        std::size_t amp = query.find('&', pos);
        if (amp == std::string::npos) {
            amp = query.size();
        }
        const std::string pair = query.substr(pos, amp - pos);
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == name) {
            *out = pair.substr(eq + 1);
            return true;
        }
        pos = amp + 1;
    }
    return false;
}

void send_html(SOCKET client, const char* html) {
    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                           "Connection: close\r\n\r\n";
    response += html;
    ::send(client, response.c_str(), static_cast<int>(response.size()), 0);
}

}  // namespace

OAuthLoopbackServer::OAuthLoopbackServer() {
    static std::once_flag winsock_once;
    std::call_once(winsock_once, [] {
        WSADATA data;
        ::WSAStartup(MAKEWORD(2, 2), &data);
    });
}

OAuthLoopbackServer::~OAuthLoopbackServer() {
    stop();
}

util::Result<void> OAuthLoopbackServer::start(const std::string& expected_state) {
    m_expected_state = expected_state;
    for (int port = kPortFirst; port <= kPortLast; ++port) {
        SOCKET listen_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_socket == INVALID_SOCKET) {
            continue;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (::bind(listen_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
                SOCKET_ERROR ||
            ::listen(listen_socket, 1) == SOCKET_ERROR) {
            ::closesocket(listen_socket);
            continue;
        }
        m_listen = static_cast<std::uintptr_t>(listen_socket);
        m_port = port;
        m_redirect_uri = "http://localhost:" + std::to_string(port) + kPath;
        return util::Result<void>::ok();
    }
    return util::Result<void>::fail(util::Error::make(
        "http.loopback_bind_failed", "无法启动本地登录回调服务（端口 23380-23385）", {}));
}

util::Result<std::string> OAuthLoopbackServer::wait_for_code(int timeout_ms) {
    if (m_listen == 0) {
        return util::Result<std::string>::fail(
            util::Error::make("http.loopback_not_started", "本地回调服务未启动", {}));
    }
    const ULONGLONG deadline = ::GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
    while (!m_stopped.load()) {
        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline) {
            return util::Result<std::string>::fail(
                util::Error::make("http.loopback_timeout", "等待浏览器登录回调超时", {}));
        }
        fd_set read_set;
        FD_ZERO(&read_set);
        const SOCKET listen_socket = as_socket(m_listen);
        FD_SET(listen_socket, &read_set);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000;  // 200ms 轮询，便于响应 stop()
        const int ready = ::select(0, &read_set, nullptr, nullptr, &tv);
        if (ready <= 0) {
            continue;
        }
        const SOCKET client = ::accept(listen_socket, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            continue;
        }
        char buffer[4096] = {};
        const int received = ::recv(client, buffer, sizeof(buffer) - 1, 0);
        const std::string request(buffer, received > 0 ? static_cast<std::size_t>(received) : 0);

        std::string code;
        std::string state;
        const bool has_code = query_param(request, "code", &code);
        query_param(request, "state", &state);

        if (!has_code) {
            send_html(client, "<html><body>登录未完成，请返回 SSCL 重试。</body></html>");
            ::closesocket(client);
            continue;
        }
        if (!m_expected_state.empty() && state != m_expected_state) {
            send_html(client,
                      "<html><body>登录校验失败（state 不匹配），请返回 SSCL 重试。</body></html>");
            ::closesocket(client);
            return util::Result<std::string>::fail(
                util::Error::make("msa.state_mismatch", "登录校验失败，请重试", {}));
        }
        send_html(client,
                  "<html><body style=\"font-family:'Microsoft YaHei UI',sans-serif;"
                  "display:flex;align-items:center;justify-content:center;height:100vh;\">"
                  "<div><h2>登录成功！</h2><p>可以关闭此页面，回到 SSCL 继续。</p></div></body></html>");
        ::closesocket(client);
        stop();
        return util::Result<std::string>::ok(code);
    }
    return util::Result<std::string>::fail(
        util::Error::make("http.loopback_stopped", "本地回调服务已停止", {}));
}

void OAuthLoopbackServer::stop() {
    m_stopped.store(true);
    if (m_listen != 0) {
        ::closesocket(as_socket(m_listen));
        m_listen = 0;
    }
}

}  // namespace sscl::platform
