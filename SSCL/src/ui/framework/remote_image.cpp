#include "ui/framework/remote_image.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <thread>
#include <utility>

#include "platform/file_utils.hpp"
#include "platform/http_client.hpp"
#include "ui/framework/image_loader.hpp"
#include "util/hash.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

namespace sscl::ui {

namespace {

constexpr std::size_t kMaxIconBytes = 2 * 1024 * 1024;
constexpr std::size_t kMaxCachedIcons = 64;
constexpr std::size_t kMaxInFlight = 6;  // 可见行有限，避免一次开几十个线程

}  // namespace

RemoteImage& RemoteImage::instance() {
    static RemoteImage image;
    return image;
}

void RemoteImage::configure(std::filesystem::path cache_dir,
                            std::function<void(std::function<void()>)> post_ui) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache_dir = std::move(cache_dir);
    m_post_ui = std::move(post_ui);
    if (!m_cache_dir.empty()) {
        (void)sscl::platform::create_directories(m_cache_dir);
    }
}

std::filesystem::path RemoteImage::cache_file(const std::string& url) const {
    const std::string name = sscl::util::hash_hex(sscl::util::HashAlgorithm::kSha1, url) + ".img";
    return m_cache_dir / sscl::util::utf8_to_wide(name);
}

std::vector<std::uint8_t> RemoteImage::fetch(const std::string& url) const {
    // 图标 URL 来自平台 API：只允许 HTTP(S) 且不含空白，避免把任意字符串塞进 WinHTTP。
    const bool https = url.compare(0, 8, "https://") == 0;
    const bool http = url.compare(0, 7, "http://") == 0;
    if ((!https && !http) || url.find_first_of(" \t\r\n") != std::string::npos) {
        return {};
    }
    if (m_cache_dir.empty()) {
        return {};
    }

    const std::filesystem::path path = cache_file(url);
    const util::Result<std::string> cached = sscl::platform::read_file(path);
    if (cached.is_ok() && !cached.value().empty() &&
        cached.value().size() <= kMaxIconBytes) {
        const std::string& data = cached.value();
        return std::vector<std::uint8_t>(data.begin(), data.end());
    }

    sscl::platform::HttpRequest request;
    request.method = "GET";
    request.url = url;
    request.headers.push_back({"User-Agent", "SSCL/0.1.0"});
    request.resolve_timeout_ms = 8000;
    request.connect_timeout_ms = 8000;
    request.send_timeout_ms = 8000;
    request.receive_timeout_ms = 8000;

    util::Result<sscl::platform::HttpStream> stream =
        sscl::platform::open_http_stream(request);
    if (stream.is_error()) {
        util::log_warn("[icons] fetch failed: " + stream.error().detail);
        return {};
    }
    if (stream.value().status_code() != 200 ||
        stream.value().content_length() > static_cast<std::int64_t>(kMaxIconBytes)) {
        return {};
    }

    std::vector<std::uint8_t> bytes;
    std::array<char, 16384> buffer{};
    for (;;) {
        util::Result<std::size_t> read = stream.value().read(buffer.data(), buffer.size());
        if (read.is_error()) {
            return {};
        }
        if (read.value() == 0) {
            break;
        }
        bytes.insert(bytes.end(), buffer.data(), buffer.data() + read.value());
        if (bytes.size() > kMaxIconBytes) {
            return {};
        }
    }
    if (bytes.empty()) {
        return {};
    }
    (void)sscl::platform::create_directories(path.parent_path());
    (void)sscl::platform::write_file_atomic(
        path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    return bytes;
}

void RemoteImage::touch(const std::string& url) {
    // 调用方必须已持有 m_mutex（避免递归加锁）。
    const auto found = std::find(m_lru.begin(), m_lru.end(), url);
    if (found != m_lru.end()) {
        m_lru.erase(found);
    }
    m_lru.push_back(url);
    while (m_lru.size() > kMaxCachedIcons) {
        const std::string oldest = m_lru.front();
        m_lru.erase(m_lru.begin());
        m_entries.erase(oldest);
    }
}

void RemoteImage::request(const std::string& url, std::function<void()> on_ready) {
    if (url.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_entries.find(url) != m_entries.end() ||
            m_inflight.find(url) != m_inflight.end() ||
            m_inflight.size() >= kMaxInFlight) {
            return;  // 每帧都会重试，空出槽位后自然继续
        }
        m_inflight.insert(url);
    }
    std::thread([this, url, ready = std::move(on_ready)]() mutable {
        std::vector<std::uint8_t> bytes = fetch(url);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            Entry& entry = m_entries[url];
            if (bytes.empty()) {
                entry.failed = true;
            } else {
                entry.bytes = std::move(bytes);
            }
            m_inflight.erase(url);
            touch(url);
        }
        if (m_post_ui && ready) {
            m_post_ui(std::move(ready));
        }
    }).detach();
}

Microsoft::WRL::ComPtr<ID2D1Bitmap> RemoteImage::bitmap(ID2D1RenderTarget* target,
                                                         const std::string& url) {
    if (target == nullptr || url.empty()) {
        return {};
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_entries.find(url);
    if (found == m_entries.end() || found->second.bytes.empty()) {
        return {};
    }
    Entry& entry = found->second;
    if (entry.target != target || !entry.image) {
        entry.image = load_bitmap_from_bytes(target, entry.bytes);
        entry.target = target;
    }
    touch(url);
    return entry.image;
}

void RemoteImage::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    m_lru.clear();
    m_inflight.clear();
}

}  // namespace sscl::ui
