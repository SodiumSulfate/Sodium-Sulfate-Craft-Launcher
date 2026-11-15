#pragma once

// WinHTTP 客户端封装（开发文档 §4.2：系统 WinHTTP + schannel；§1.7：超时/取消；§9.2：HTTPS 校验不关闭）。
// 提供一次性请求（JSON API）与流式读取（下载文件）两种用法。

// winhttp.h 不自带 windows.h，必须先包含（否则 HINTERNET 等类型全部未定义）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winhttp.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "util/result.hpp"

namespace sscl::platform {

// D-109 网络代理（设置页「其他 → 网络代理」）。
// kSystem：使用 Windows 系统代理（默认，含 PAC）；kManualHttp：手动 HTTP 代理；
// kDirect：全部直连，不读系统代理、不走 PAC、不使用任何代理。
enum class ProxyMode {
    kSystem = 0,
    kManualHttp = 1,
    kDirect = 2,
};

struct ProxyConfig {
    ProxyMode mode = ProxyMode::kSystem;
    std::string host = "127.0.0.1";  // 仅 kManualHttp 使用
    int port = 10800;                // 仅 kManualHttp 使用

    bool manual_valid() const;
    std::string authority() const;  // "host:port"
};

// 从 config.json 的整数/字符串构造；非法值回退默认（系统代理 / 127.0.0.1:10800）。
ProxyConfig make_proxy_config(int mode, const std::string& host, int port);

// 进程级全局代理配置：UI 线程写，所有 HTTP 请求（含后台下载）读。
ProxyConfig current_proxy_config();
void set_proxy_config(const ProxyConfig& config);

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;  // 请求体（POST 表单/JSON；GET 时忽略）
    int resolve_timeout_ms = 15000;
    int connect_timeout_ms = 15000;
    int send_timeout_ms = 30000;
    int receive_timeout_ms = 30000;
};

struct HttpResponse {
    int status_code = 0;
    std::map<std::string, std::string> headers;  // 键已转小写
    std::string body;
};

// 流式 HTTP 响应（RAII、可移动）。open 成功即已收到响应头。
class HttpStream {
public:
    HttpStream() = default;
    HttpStream(HttpStream&& other) noexcept;
    HttpStream& operator=(HttpStream&& other) noexcept;
    ~HttpStream();

    HttpStream(const HttpStream&) = delete;
    HttpStream& operator=(const HttpStream&) = delete;

    bool valid() const;
    int status_code() const;
    std::int64_t content_length() const;  // -1 = 未知
    const std::map<std::string, std::string>& headers() const;

    // 读入 buffer；返回实际字节数，0 表示响应结束。
    util::Result<std::size_t> read(char* buffer, std::size_t size);

private:
    friend util::Result<HttpStream> open_http_stream(const HttpRequest& request);

    void close();

    HINTERNET m_session = nullptr;
    HINTERNET m_connect = nullptr;
    HINTERNET m_request = nullptr;
    int m_status_code = 0;
    std::int64_t m_content_length = -1;
    std::map<std::string, std::string> m_headers;
};

// 打开请求并接收响应头（不读 body）。HTTP 状态码不视为错误，由调用方判断。
util::Result<HttpStream> open_http_stream(const HttpRequest& request);

// 一次性请求：读完整 body（上限 64MB），适合版本清单等 JSON API。
util::Result<HttpResponse> http_request(const HttpRequest& request);

// D-112 延迟探测：分阶段计时，供设置页「网络 → 连接测试」使用。
// 与 open_http_stream 共用同一套代理候选（因此能反映代理真实开销），但
// **禁用重定向**并强制新建连接，保证每次观测都是独立的「冷连接」样本——
// 否则 301/302 与 keep-alive 复用会明显抬高读数。
struct HttpProbeTiming {
    long long dns_ms = -1;      // DNS 解析；-1 = 未观测到
    long long connect_ms = -1;  // 起点到 TCP 握手完成（与 curl time_connect 同口径）
    long long total_ms = -1;    // 起点到响应头（含 TLS 握手与一次 HTTP 往返）
    int status_code = 0;        // 不跟随重定向，可能为 301/302
    std::string error;          // 非空 = 探测失败
};

HttpProbeTiming probe_http_timing(const std::string& url, int timeout_ms);

}  // namespace sscl::platform
