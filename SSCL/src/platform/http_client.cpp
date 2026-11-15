#include "platform/http_client.hpp"

#include "platform/win_error.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

#pragma comment(lib, "winhttp.lib")

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <utility>

namespace sscl::platform {

namespace {

constexpr std::size_t kMaxBodySize = 64 * 1024 * 1024;  // 一次性请求的 body 上限

// D-109：全局代理配置的唯一存储（互斥保护：UI 线程写、下载线程读）。
std::mutex& proxy_mutex() {
    static std::mutex mutex;
    return mutex;
}

ProxyConfig& proxy_storage() {
    static ProxyConfig config;
    return config;
}

std::string trim_proxy_text(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// —— 代理访问策略（D-018：开代理也必须可用）——————————————————
// 1. 显式系统代理 → 规范化后走 NAMED_PROXY（WinHTTP 只认 `[scheme=]host:port`；
//    WinINET 里可能存 `http://host:port` 等带协议前缀形态，直接传会连接失败）；
// 2. 仅配置了自动配置 URL（PAC）→ AUTOMATIC_PROXY；
// 3. 任何代理方式失败（连接/发送/接收/407）→ **自动回退直连**；
// 4. 无任何代理配置 → 直连（不再做 WPAD 探测，避免劫持/误配快速失败）。

struct AccessCandidate {
    DWORD access_type = WINHTTP_ACCESS_TYPE_NO_PROXY;
    std::wstring proxy;
    std::wstring bypass;
    const char* label = "direct";
};

// 去掉代理条目中的协议前缀（http:// https:// socks:// socks5:// ftp://）。
std::wstring strip_proxy_scheme_prefix(std::wstring entry) {
    static const wchar_t* kSchemes[] = {L"http://", L"https://", L"socks5://", L"socks://",
                                        L"ftp://"};
    for (const wchar_t* scheme : kSchemes) {
        const std::size_t length = std::wcslen(scheme);
        if (entry.size() >= length && ::_wcsnicmp(entry.c_str(), scheme, length) == 0) {
            entry.erase(0, length);
            break;
        }
    }
    return entry;
}

// 规范化 WinINET 代理串：`host:port` 或 `scheme=host:port`，分号分段；
// `=` 右侧同样去协议前缀。
std::wstring normalize_proxy_string(const std::wstring& raw) {
    std::wstring out;
    std::size_t start = 0;
    while (start <= raw.size()) {
        std::size_t end = raw.find(L';', start);
        if (end == std::wstring::npos) {
            end = raw.size();
        }
        std::wstring entry = raw.substr(start, end - start);
        while (!entry.empty() && ::iswspace(entry.front())) {
            entry.erase(entry.begin());
        }
        while (!entry.empty() && ::iswspace(entry.back())) {
            entry.pop_back();
        }
        if (!entry.empty()) {
            std::wstring normalized;
            const std::size_t eq = entry.find(L'=');
            if (eq != std::wstring::npos) {
                normalized = entry.substr(0, eq + 1) + strip_proxy_scheme_prefix(entry.substr(eq + 1));
            } else {
                normalized = strip_proxy_scheme_prefix(entry);
            }
            if (!out.empty()) {
                out += L';';
            }
            out += normalized;
        }
        if (end == raw.size()) {
            break;
        }
        start = end + 1;
    }
    return out;
}

// 访问候选列表：代理优先（尊重系统设置），直连兜底（代理坏了也不影响）。
std::vector<AccessCandidate> access_candidates() {
    std::vector<AccessCandidate> out;
    const ProxyConfig proxy_config = current_proxy_config();

    // D-109「不使用任何代理（全部直连）」：不读系统代理、不走 PAC。
    if (proxy_config.mode == ProxyMode::kDirect) {
        AccessCandidate direct;
        direct.label = "direct(forced)";
        out.push_back(std::move(direct));
        return out;
    }
    // D-110「使用 HTTP 代理」是**强制代理**：只给一个候选，代理不可用即失败。
    // 之前追加了直连兜底，导致「端口填错仍能正常加载」，用户无法判断代理是否生效；
    // 系统代理模式仍保留直连兜底（D-018，系统代理常由 PAC/策略下发）。
    if (proxy_config.mode == ProxyMode::kManualHttp) {
        AccessCandidate named;
        named.access_type = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        named.proxy = sscl::util::utf8_to_wide(proxy_config.authority());
        named.label = "manual-http(forced)";
        out.push_back(std::move(named));
        return out;
    }

    // 「使用系统代理」：原有 WinINET/PAC 逻辑。
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
    if (::WinHttpGetIEProxyConfigForCurrentUser(&config)) {
        const std::wstring proxy =
            normalize_proxy_string(config.lpszProxy != nullptr ? config.lpszProxy : L"");
        const std::wstring bypass =
            config.lpszProxyBypass != nullptr ? config.lpszProxyBypass : L"";
        const bool has_autoconfig = config.lpszAutoConfigUrl != nullptr;
        if (config.lpszAutoConfigUrl != nullptr) ::GlobalFree(config.lpszAutoConfigUrl);
        if (config.lpszProxy != nullptr) ::GlobalFree(config.lpszProxy);
        if (config.lpszProxyBypass != nullptr) ::GlobalFree(config.lpszProxyBypass);

        if (!proxy.empty()) {
            AccessCandidate named;
            named.access_type = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
            named.proxy = proxy;
            named.bypass = bypass;
            named.label = "system-proxy";
            out.push_back(std::move(named));
        } else if (has_autoconfig) {
            AccessCandidate automatic;
            automatic.access_type = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
            automatic.label = "auto-proxy(pac)";
            out.push_back(std::move(automatic));
        }
    }
    AccessCandidate direct;
    direct.label = "direct";
    out.push_back(std::move(direct));
    return out;
}

HINTERNET open_http_session(const AccessCandidate& candidate) {
    const bool named = candidate.access_type == WINHTTP_ACCESS_TYPE_NAMED_PROXY;
    return ::WinHttpOpen(L"SSCL/0.1", candidate.access_type,
                         named ? candidate.proxy.c_str() : WINHTTP_NO_PROXY_NAME,
                         named && !candidate.bypass.empty() ? candidate.bypass.c_str()
                                                            : WINHTTP_NO_PROXY_BYPASS,
                         0);
}

// D-112：探测专用状态回调。只记录三个时间点，不做任何 I/O。
// context 经 WINHTTP_OPTION_CONTEXT_VALUE 传入，指向调用栈上的 ProbeTiming。
struct ProbeTiming {
    std::chrono::steady_clock::time_point start;
    std::atomic<long long> dns_ms{-1};
    std::atomic<long long> connect_ms{-1};
    std::atomic<long long> total_ms{-1};
};

void CALLBACK probe_status_callback(HINTERNET, DWORD_PTR context, DWORD status, LPVOID, DWORD) {
    ProbeTiming* timing = reinterpret_cast<ProbeTiming*>(context);
    if (timing == nullptr) {
        return;
    }
    const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - timing->start)
                                  .count();
    switch (status) {
        case WINHTTP_CALLBACK_STATUS_NAME_RESOLVED:
            timing->dns_ms.store(elapsed);
            break;
        case WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER:
            timing->connect_ms.store(elapsed);
            break;
        case WINHTTP_CALLBACK_STATUS_RESPONSE_RECEIVED:
            timing->total_ms.store(elapsed);
            break;
        default:
            break;
    }
}

constexpr DWORD kProbeCallbackFlags = WINHTTP_CALLBACK_FLAG_RESOLVE_NAME |
                                      WINHTTP_CALLBACK_FLAG_CONNECT_TO_SERVER |
                                      WINHTTP_CALLBACK_FLAG_RECEIVE_RESPONSE;

std::wstring build_header_block(const std::vector<std::pair<std::string, std::string>>& headers) {
    std::wstring block;
    for (const std::pair<std::string, std::string>& header : headers) {
        block += sscl::util::utf8_to_wide(header.first);
        block += L": ";
        block += sscl::util::utf8_to_wide(header.second);
        block += L"\r\n";
    }
    return block;
}

// 解析 "Name: Value\r\n..." 原始响应头（跳过 HTTP 状态行）。
void parse_header_lines(const std::wstring& raw, std::map<std::string, std::string>& out) {
    std::size_t start = 0;
    while (start < raw.size()) {
        std::size_t end = raw.find(L"\r\n", start);
        if (end == std::wstring::npos) {
            end = raw.size();
        }
        const std::wstring line = raw.substr(start, end - start);
        start = end + 2;
        if (line.empty() || line.compare(0, 5, L"HTTP/") == 0) {
            continue;
        }
        const std::size_t colon = line.find(L':');
        if (colon == std::wstring::npos) {
            continue;
        }
        std::wstring name = line.substr(0, colon);
        std::size_t value_begin = colon + 1;
        while (value_begin < line.size() && (line[value_begin] == L' ' || line[value_begin] == L'\t')) {
            ++value_begin;
        }
        std::wstring value = line.substr(value_begin);
        while (!value.empty() && (value.back() == L'\r' || value.back() == L'\n' ||
                                  value.back() == L' ')) {
            value.pop_back();
        }
        out[sscl::util::to_lower_ascii(sscl::util::wide_to_utf8(name))] =
            sscl::util::wide_to_utf8(value);
    }
}

}  // namespace

HttpStream::HttpStream(HttpStream&& other) noexcept
    : m_session(other.m_session),
      m_connect(other.m_connect),
      m_request(other.m_request),
      m_status_code(other.m_status_code),
      m_content_length(other.m_content_length),
      m_headers(std::move(other.m_headers)) {
    other.m_session = nullptr;
    other.m_connect = nullptr;
    other.m_request = nullptr;
    other.m_status_code = 0;
    other.m_content_length = -1;
}

HttpStream& HttpStream::operator=(HttpStream&& other) noexcept {
    if (this != &other) {
        close();
        m_session = other.m_session;
        m_connect = other.m_connect;
        m_request = other.m_request;
        m_status_code = other.m_status_code;
        m_content_length = other.m_content_length;
        m_headers = std::move(other.m_headers);
        other.m_session = nullptr;
        other.m_connect = nullptr;
        other.m_request = nullptr;
        other.m_status_code = 0;
        other.m_content_length = -1;
    }
    return *this;
}

HttpStream::~HttpStream() {
    close();
}

void HttpStream::close() {
    if (m_request != nullptr) {
        ::WinHttpCloseHandle(m_request);
        m_request = nullptr;
    }
    if (m_connect != nullptr) {
        ::WinHttpCloseHandle(m_connect);
        m_connect = nullptr;
    }
    if (m_session != nullptr) {
        ::WinHttpCloseHandle(m_session);
        m_session = nullptr;
    }
}

bool HttpStream::valid() const {
    return m_request != nullptr;
}

int HttpStream::status_code() const {
    return m_status_code;
}

std::int64_t HttpStream::content_length() const {
    return m_content_length;
}

const std::map<std::string, std::string>& HttpStream::headers() const {
    return m_headers;
}

util::Result<std::size_t> HttpStream::read(char* buffer, std::size_t size) {
    if (!valid()) {
        return util::Result<std::size_t>::fail(
            util::Error::make("http.not_open", "HTTP 连接未打开", {}));
    }
    if (size == 0) {
        return util::Result<std::size_t>::ok(0);
    }

    DWORD available = 0;
    if (!::WinHttpQueryDataAvailable(m_request, &available)) {
        return util::Result<std::size_t>::fail(
            util::Error::make("http.read_failed", "读取 HTTP 响应失败",
                              format_win_error(::GetLastError())));
    }
    if (available == 0) {
        return util::Result<std::size_t>::ok(0);  // EOF
    }

    DWORD to_read = available;
    if (to_read > size) {
        to_read = static_cast<DWORD>(size);
    }
    DWORD read = 0;
    if (!::WinHttpReadData(m_request, buffer, to_read, &read)) {
        return util::Result<std::size_t>::fail(
            util::Error::make("http.read_failed", "读取 HTTP 响应失败",
                              format_win_error(::GetLastError())));
    }
    return util::Result<std::size_t>::ok(static_cast<std::size_t>(read));
}

util::Result<HttpStream> open_http_stream(const HttpRequest& request) {
    if (request.url.empty()) {
        return util::Result<HttpStream>::fail(
            util::Error::make("http.bad_url", "URL 为空", {}));
    }

    const std::wstring url = sscl::util::utf8_to_wide(request.url);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(url.c_str(), 0, 0, &components)) {
        return util::Result<HttpStream>::fail(
            util::Error::make("http.bad_url", "无法解析 URL：" + request.url,
                              format_win_error(::GetLastError())));
    }
    if (components.lpszHostName == nullptr || components.dwHostNameLength == 0) {
        return util::Result<HttpStream>::fail(
            util::Error::make("http.bad_url", "URL 缺少主机名：" + request.url, {}));
    }

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path;
    if (components.lpszUrlPath != nullptr && components.dwUrlPathLength > 0) {
        path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (components.lpszExtraInfo != nullptr && components.dwExtraInfoLength > 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) {
        path = L"/";
    }

    const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    INTERNET_PORT port = components.nPort;
    if (port == 0) {
        port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    }

    // 逐访问候选尝试（D-018）：代理失败（连接/发送/接收/407）自动换下一个（直连）。
    const std::vector<AccessCandidate> candidates = access_candidates();
    util::Error last_error = util::Error::make("http.open_failed", "无法建立 HTTP 连接", {});

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const AccessCandidate& candidate = candidates[i];
        HttpStream stream;
        stream.m_session = open_http_session(candidate);
        if (stream.m_session == nullptr) {
            last_error = util::Error::make("http.open_failed", "无法初始化 HTTP 会话",
                                          format_win_error(::GetLastError()));
            continue;
        }

        stream.m_connect = ::WinHttpConnect(stream.m_session, host.c_str(), port, 0);
        if (stream.m_connect == nullptr) {
            last_error = util::Error::make("http.open_failed", "无法连接服务器",
                                          format_win_error(::GetLastError()));
            continue;
        }

        const std::wstring verb = sscl::util::utf8_to_wide(request.method);
        stream.m_request =
            ::WinHttpOpenRequest(stream.m_connect, verb.c_str(), path.c_str(), nullptr,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 secure ? WINHTTP_FLAG_SECURE : 0);
        if (stream.m_request == nullptr) {
            last_error = util::Error::make("http.open_failed", "无法创建 HTTP 请求",
                                          format_win_error(::GetLastError()));
            continue;
        }

        ::WinHttpSetTimeouts(stream.m_request, request.resolve_timeout_ms,
                             request.connect_timeout_ms, request.send_timeout_ms,
                             request.receive_timeout_ms);

        const std::wstring header_block = build_header_block(request.headers);
        const wchar_t* headers_ptr =
            header_block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_block.c_str();
        const DWORD headers_length =
            header_block.empty() ? 0 : static_cast<DWORD>(header_block.size());
        // POST 等带 body 的请求：把 body 作为 optional data 一次性提交。
        const bool has_body = !request.body.empty();
        const void* body_ptr = has_body ? static_cast<const void*>(request.body.data())
                                        : WINHTTP_NO_REQUEST_DATA;
        const DWORD body_length = has_body ? static_cast<DWORD>(request.body.size()) : 0;
        if (!::WinHttpSendRequest(stream.m_request, headers_ptr, headers_length,
                                  const_cast<void*>(body_ptr), body_length,
                                  has_body ? body_length : 0, 0)) {
            last_error = util::Error::make("http.send_failed", "发送 HTTP 请求失败",
                                          format_win_error(::GetLastError()));
            if (i + 1 < candidates.size()) {
                sscl::util::log_warn(std::string("[http] ") + candidate.label +
                                     " send failed: " + last_error.detail + "; retrying " +
                                     candidates[i + 1].label);
            }
            continue;
        }
        if (!::WinHttpReceiveResponse(stream.m_request, nullptr)) {
            last_error = util::Error::make("http.receive_failed", "接收 HTTP 响应失败",
                                          format_win_error(::GetLastError()));
            if (i + 1 < candidates.size()) {
                sscl::util::log_warn(std::string("[http] ") + candidate.label +
                                     " receive failed: " + last_error.detail + "; retrying " +
                                     candidates[i + 1].label);
            }
            continue;
        }

        DWORD status = 0;
        DWORD status_size = sizeof(status);
        if (::WinHttpQueryHeaders(stream.m_request,
                                  WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                  WINHTTP_NO_HEADER_INDEX)) {
            stream.m_status_code = static_cast<int>(status);
        }

        // 407 = 代理要求认证/拒绝转发：换下一个候选（通常即直连）。
        if (stream.m_status_code == 407 && i + 1 < candidates.size()) {
            last_error = util::Error::make("http.proxy_auth_required", "代理服务器拒绝转发（407）",
                                          candidate.label);
            sscl::util::log_warn(std::string("[http] ") + candidate.label +
                                 " returned 407; retrying " + candidates[i + 1].label);
            continue;
        }

        DWORD content_length = 0;
        DWORD length_size = sizeof(content_length);
        if (::WinHttpQueryHeaders(stream.m_request,
                                  WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &content_length, &length_size,
                                  WINHTTP_NO_HEADER_INDEX)) {
            stream.m_content_length = static_cast<std::int64_t>(content_length);
        }

        DWORD header_bytes = 0;
        ::WinHttpQueryHeaders(stream.m_request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                              WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &header_bytes,
                              WINHTTP_NO_HEADER_INDEX);
        if (::GetLastError() == ERROR_INSUFFICIENT_BUFFER && header_bytes > 0) {
            std::wstring raw(header_bytes / sizeof(wchar_t), L'\0');
            if (::WinHttpQueryHeaders(stream.m_request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                      WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &header_bytes,
                                      WINHTTP_NO_HEADER_INDEX)) {
                raw.resize(header_bytes / sizeof(wchar_t));
                parse_header_lines(raw, stream.m_headers);
            }
        }

        if (i > 0) {
            sscl::util::log_info(std::string("[http] fallback succeeded via ") + candidate.label +
                                 " (" + request.url + ")");
        }
        return util::Result<HttpStream>::ok(std::move(stream));
    }

    // D-110：强制代理模式下所有候选（只有代理本身）都失败 → 明确告知，
    // 不要把「代理连不上」伪装成普通网络错误。
    if (current_proxy_config().mode == ProxyMode::kManualHttp) {
        const ProxyConfig proxy = current_proxy_config();
        return util::Result<HttpStream>::fail(util::Error::make(
            "http.proxy_unreachable",
            "HTTP 代理不可用（" + proxy.authority() +
                "）：请检查代理服务是否运行，或改为系统代理 / 全部直连",
            last_error.detail));
    }
    return util::Result<HttpStream>::fail(std::move(last_error));
}

util::Result<HttpResponse> http_request(const HttpRequest& request) {
    util::Result<HttpStream> opened = open_http_stream(request);
    if (opened.is_error()) {
        return util::Result<HttpResponse>::fail(std::move(opened.error()));
    }
    HttpStream stream = std::move(opened.value());

    HttpResponse response;
    response.status_code = stream.status_code();
    response.headers = stream.headers();
    if (stream.content_length() > 0 &&
        static_cast<std::size_t>(stream.content_length()) < kMaxBodySize) {
        response.body.reserve(static_cast<std::size_t>(stream.content_length()));
    }

    char buffer[16384];
    for (;;) {
        util::Result<std::size_t> read = stream.read(buffer, sizeof(buffer));
        if (read.is_error()) {
            return util::Result<HttpResponse>::fail(std::move(read.error()));
        }
        if (read.value() == 0) {
            break;
        }
        if (response.body.size() + read.value() > kMaxBodySize) {
            return util::Result<HttpResponse>::fail(
                util::Error::make("http.too_large", "HTTP 响应过大", {}));
        }
        response.body.append(buffer, read.value());
    }

    return util::Result<HttpResponse>::ok(std::move(response));
}

bool ProxyConfig::manual_valid() const {
    return !host.empty() && port >= 1 && port <= 65535;
}

std::string ProxyConfig::authority() const {
    return host + ":" + std::to_string(port);
}

ProxyConfig make_proxy_config(int mode, const std::string& host, int port) {
    ProxyConfig config;
    config.mode = (mode >= 0 && mode <= 2) ? static_cast<ProxyMode>(mode)
                                           : ProxyMode::kSystem;
    std::string clean = trim_proxy_text(host);
    if (clean.rfind("http://", 0) == 0) {
        clean = clean.substr(7);
    } else if (clean.rfind("https://", 0) == 0) {
        clean = clean.substr(8);
    }
    while (!clean.empty() && clean.back() == '/') {
        clean.pop_back();
    }
    config.host = clean.empty() ? "127.0.0.1" : clean;
    config.port = (port >= 1 && port <= 65535) ? port : 10800;
    return config;
}

ProxyConfig current_proxy_config() {
    std::lock_guard<std::mutex> lock(proxy_mutex());
    return proxy_storage();
}

void set_proxy_config(const ProxyConfig& config) {
    // D-110：落库前统一清洗，保证 kManualHttp 永远有合法的 host:port。
    const ProxyConfig sanitized =
        make_proxy_config(static_cast<int>(config.mode), config.host, config.port);
    {
        std::lock_guard<std::mutex> lock(proxy_mutex());
        proxy_storage() = sanitized;
    }
    util::log_info("[http] proxy mode=" + std::to_string(static_cast<int>(sanitized.mode)) +
                   (sanitized.mode == ProxyMode::kManualHttp ? " " + sanitized.authority() : ""));
}

HttpProbeTiming probe_http_timing(const std::string& url, int timeout_ms) {
    HttpProbeTiming result;
    if (url.empty()) {
        result.error = "URL 为空";
        return result;
    }
    const int timeout = (std::max)(1000, timeout_ms);
    const std::wstring wide_url = sscl::util::utf8_to_wide(url);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components) ||
        components.lpszHostName == nullptr || components.dwHostNameLength == 0) {
        result.error = "无法解析 URL：" + url;
        return result;
    }

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path;
    if (components.lpszUrlPath != nullptr && components.dwUrlPathLength > 0) {
        path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (components.lpszExtraInfo != nullptr && components.dwExtraInfoLength > 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) {
        path = L"/";
    }
    const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    INTERNET_PORT port = components.nPort;
    if (port == 0) {
        port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    }

    const ProxyConfig active_proxy = current_proxy_config();
    // 与正式请求共用候选链：手动代理模式下只剩代理本身，探测结果即真实代理开销。
    for (const AccessCandidate& candidate : access_candidates()) {
        HINTERNET session = open_http_session(candidate);
        if (session == nullptr) {
            continue;
        }
        HINTERNET connect = ::WinHttpConnect(session, host.c_str(), port, 0);
        HINTERNET request =
            connect == nullptr
                ? nullptr
                : ::WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       secure ? WINHTTP_FLAG_SECURE : 0);

        bool ok = false;
        if (request != nullptr) {
            ::WinHttpSetTimeouts(request, timeout, timeout, timeout, timeout);
            // 不跟随重定向：301/302 会多出 1~2 次往返，读数就不再是握手成本。
            DWORD disable_redirects = WINHTTP_DISABLE_REDIRECTS;
            ::WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable_redirects,
                               sizeof(disable_redirects));

            ProbeTiming timing;
            timing.start = std::chrono::steady_clock::now();
            DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&timing);
            const BOOL context_ready = ::WinHttpSetOption(
                request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context));
            const bool callback_ready =
                context_ready != FALSE &&
                ::WinHttpSetStatusCallback(request, &probe_status_callback, kProbeCallbackFlags,
                                           0) != WINHTTP_INVALID_STATUS_CALLBACK;

            // Connection: close 强制新建连接（否则 keep-alive 会让 connect 阶段≈0）；
            // no-cache 避免命中本地缓存。
            static constexpr wchar_t kProbeHeaders[] =
                L"Connection: close\r\nCache-Control: no-cache\r\n";
            const DWORD headers_length =
                static_cast<DWORD>(sizeof(kProbeHeaders) / sizeof(wchar_t) - 1);
            if (::WinHttpSendRequest(request, kProbeHeaders, headers_length, nullptr, 0, 0, 0) &&
                ::WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0;
                DWORD status_size = sizeof(status);
                if (::WinHttpQueryHeaders(request,
                                          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                          WINHTTP_NO_HEADER_INDEX)) {
                    result.status_code = static_cast<int>(status);
                }
                if (callback_ready) {
                    result.dns_ms = timing.dns_ms.load();
                    result.connect_ms = timing.connect_ms.load();
                    result.total_ms = timing.total_ms.load();
                }
                if (result.total_ms < 0) {
                    // 回调未就绪（或未收到 RESPONSE_RECEIVED，例如 1xx）时用收尾时间兜底。
                    result.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - timing.start)
                                          .count();
                }
                ok = true;
            }
            if (callback_ready) {
                ::WinHttpSetStatusCallback(request, nullptr, 0, 0);
            }
        }

        if (request != nullptr) {
            ::WinHttpCloseHandle(request);
        }
        if (connect != nullptr) {
            ::WinHttpCloseHandle(connect);
        }
        ::WinHttpCloseHandle(session);
        if (ok) {
            return result;
        }
    }

    result.error = active_proxy.mode == ProxyMode::kManualHttp
                       ? "HTTP 代理不可用（" + active_proxy.authority() + "）"
                       : "连接失败";
    return result;
}

}  // namespace sscl::platform
