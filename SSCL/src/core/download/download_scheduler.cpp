#include "core/download/download_scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>
#include <utility>

#include "core/download/mirror_map.hpp"
#include "platform/file_utils.hpp"
#include "platform/http_client.hpp"
#include "platform/win_error.hpp"
#include "util/hash.hpp"
#include "util/log.hpp"

namespace sscl::core {

namespace {

constexpr std::size_t kIoBuffer = 64 * 1024;
constexpr int kChunkJoinPollMs = 50;
// D-100：低速窗口（单流）。窗口内收到 < kStallMinBytes 视为「准卡死」，
// 直接判失败交给上层重试/换源，避免用户干等（PCL 的「速度低于阈值即切源」）。
constexpr auto kStallWindow = std::chrono::seconds(20);
constexpr std::uint64_t kStallMinBytes = 16 * 1024;
// 是否启用低速换源由 global_source_policy().files_slow_switch() 决定（D-107）。

// 单文件分块阈值（负责人 2026-11-06）：>2MB 才分块，按大小给 2/4/6 线程。
constexpr std::int64_t kPieceTier2 = 2 * 1024 * 1024;
constexpr std::int64_t kPieceTier4 = 5 * 1024 * 1024;
constexpr std::int64_t kPieceTier6 = 10 * 1024 * 1024;
// 进程级分块线程预算：文件级并发最高 32，若每个大文件都开 6 线程会到 192 连接。
// 预算满时自动降为更少分块（最低 2），拿不到就回退单流。
constexpr int kPieceThreadBudget = 48;

std::atomic<int> g_piece_threads{0};

// 借用分块线程额度；析构归还。granted() == 0 表示完全没有额度。
class PieceThreadLease {
public:
    explicit PieceThreadLease(int requested) {
        int current = g_piece_threads.load(std::memory_order_relaxed);
        for (;;) {
            const int available = kPieceThreadBudget - current;
            if (available <= 0) {
                m_granted = 0;
                return;
            }
            m_granted = (std::min)(requested, available);
            if (g_piece_threads.compare_exchange_weak(current, current + m_granted,
                                                      std::memory_order_relaxed)) {
                return;
            }
        }
    }
    ~PieceThreadLease() {
        if (m_granted > 0) {
            g_piece_threads.fetch_sub(m_granted, std::memory_order_relaxed);
        }
    }
    PieceThreadLease(const PieceThreadLease&) = delete;
    PieceThreadLease& operator=(const PieceThreadLease&) = delete;

    int granted() const { return m_granted; }

private:
    int m_granted = 0;
};

util::Error cancelled_error() {
    return util::Error::make("download.cancelled", "下载已取消", {});
}

}  // namespace

int download_piece_threads(std::int64_t bytes) {
    if (bytes > kPieceTier6) {
        return 6;
    }
    if (bytes > kPieceTier4) {
        return 4;
    }
    if (bytes > kPieceTier2) {
        return 2;
    }
    return 1;
}

std::filesystem::path make_download_temp_path(const std::filesystem::path& destination) {
    static std::atomic<std::uint64_t> next_thread_id{0};
    // 同一线程内 download_file/try_source 必须使用同一路径；不同线程使用不同后缀。
    thread_local const std::uint64_t thread_id =
        next_thread_id.fetch_add(1, std::memory_order_relaxed) + 1;
    return destination.wstring() + L".tmp-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
           std::to_wstring(thread_id);
}

DownloadScheduler::DownloadScheduler(SchedulerConfig config)
    : m_config(config), m_limiter(config.speed_limit_bps) {}

void DownloadScheduler::set_speed_limit(std::int64_t bytes_per_second) {
    m_config.speed_limit_bps = bytes_per_second < 0 ? 0 : bytes_per_second;
    m_limiter.set_rate(m_config.speed_limit_bps);
}

std::map<std::string, DownloadScheduler::SourceState> DownloadScheduler::source_states() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_sources;
}

void DownloadScheduler::record_failure(const std::string& source_url, const util::Error& error) {
    // 诊断日志：保留每次失败的错误码与详情（定位网络/TLS 问题的唯一线索）
    util::log_warn("[download] fail " + source_url + " -> " + error.code + " / " + error.detail);
    std::lock_guard<std::mutex> lock(m_state_mutex);
    SourceState& state = m_sources[source_url];
    ++state.fail_count;

    if (error.code == "download.range_unsupported") {
        state.supports_range = false;
    }

    // knowledge §3：BMCLAPI 403/429 是限频，不禁用源。
    const bool rate_limited = error.code == "download.http_status" &&
                              (error.detail.find("403") != std::string::npos ||
                               error.detail.find("429") != std::string::npos);
    if (is_bmclapi_url(source_url) && rate_limited) {
        return;
    }
    if (state.fail_count >= m_config.source_disable_threshold) {
        state.disabled = true;
        // D-086：禁用必须带冷却，否则该源在进程剩余生命周期内永远不会再被尝试，
        // 也就永远没有机会 record_success 解禁（旧实现的死锁）。
        state.disabled_until =
            std::chrono::steady_clock::now() +
            std::chrono::seconds((std::max)(1, m_config.source_disable_cooldown_s));
        util::log_warn("[download] source disabled for " +
                       std::to_string(m_config.source_disable_cooldown_s) + "s: " +
                       source_url);
    }
}

void DownloadScheduler::record_success(const std::string& source_url) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    SourceState& state = m_sources[source_url];
    state.fail_count = 0;
    state.disabled = false;
    state.disabled_until = {};
}

util::Result<void> DownloadScheduler::verify_hash(const DownloadRequest& request) {
    // 先确认文件真的落盘：否则哈希函数只会返回一个 Win32 错误码（如 2 = 文件不存在），
    // 日志里难以辨认。轮次 114 的分块下载漏掉 MoveFileExW 就是被这个错误码掩盖的。
    {
        std::error_code exists_ec;
        if (!std::filesystem::is_regular_file(request.destination, exists_ec)) {
            return util::Result<void>::fail(util::Error::make(
                "download.write_failed", "下载文件未落盘：" + request.display_name,
                request.url));
        }
    }
    if (!request.expected_sha1.empty()) {
        util::Result<std::string> actual =
            util::hash_file_hex(util::HashAlgorithm::kSha1, request.destination);
        if (actual.is_error()) {
            return util::Result<void>::fail(std::move(actual.error()));
        }
        if (!util::hash_hex_equal(actual.value(), request.expected_sha1)) {
            return util::Result<void>::fail(util::Error::make(
                "download.hash_mismatch", "文件校验失败（SHA1）：" + request.display_name,
                "expected " + request.expected_sha1 + " actual " + actual.value()));
        }
        return util::Result<void>::ok();
    }
    if (!request.expected_md5.empty()) {
        util::Result<std::string> actual =
            util::hash_file_hex(util::HashAlgorithm::kMd5, request.destination);
        if (actual.is_error()) {
            return util::Result<void>::fail(std::move(actual.error()));
        }
        if (!util::hash_hex_equal(actual.value(), request.expected_md5)) {
            return util::Result<void>::fail(util::Error::make(
                "download.hash_mismatch", "文件校验失败（MD5）：" + request.display_name,
                "expected " + request.expected_md5 + " actual " + actual.value()));
        }
    }
    return util::Result<void>::ok();
}

util::Result<void> DownloadScheduler::download_file(const DownloadRequest& request,
                                                    const DownloadProgressFn& progress,
                                                    std::stop_token stop) {
    if (request.url.empty() || request.destination.empty()) {
        return util::Result<void>::fail(
            util::Error::make("download.bad_request", "下载请求缺少 URL 或目标路径", {}));
    }

    // 并发上限（§7.4 默认 16）
    for (;;) {
        if (stop.stop_requested()) {
            return util::Result<void>::fail(cancelled_error());
        }
        int active = m_active_downloads.load();
        if (active < m_config.max_concurrent_downloads) {
            if (m_active_downloads.compare_exchange_weak(active, active + 1)) {
                break;
            }
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kChunkJoinPollMs));
    }

    struct ActiveGuard {
        std::atomic<int>* counter;
        ~ActiveGuard() { counter->fetch_sub(1); }
    } guard{&m_active_downloads};

    if (!request.destination.parent_path().empty()) {
        util::Result<void> created =
            platform::create_directories(request.destination.parent_path());
        if (created.is_error()) {
            return created;
        }
    }

    const std::filesystem::path temp = make_download_temp_path(request.destination);
    std::vector<std::string> candidates = build_source_candidates(request.url);
    util::Error last_error = util::Error::make("download.all_sources_failed", "所有下载源均失败",
                                               request.url);

    // D-117：换源/重试会让 download_single / download_ranged 从 0 重新计数。
    // 若把每次尝试的 current 原样透传，上层按 delta 累加时会把已下载的字节重复计入
    // （典型表现：文件与总进度提前冲顶）。这里统一折算成「本文件已到达的最大字节数」：
    // 进度只进不退、单个文件最多只贡献清单声明的大小，重试期间进度会短暂停住而不是回退。
    std::uint64_t reported_for_file = 0;
    const std::uint64_t file_size =
        request.expected_size > 0 ? static_cast<std::uint64_t>(request.expected_size) : 0;
    const DownloadProgressFn forward =
        progress ? DownloadProgressFn([progress, &reported_for_file, file_size](
                                          std::uint64_t current, std::uint64_t total) {
            std::uint64_t reported = (std::max)(reported_for_file, current);
            if (file_size > 0 && reported > file_size) {
                reported = file_size;  // 重试不代表文件变大
            }
            reported_for_file = reported;
            progress(reported, total);
        })
                 : nullptr;

    for (int attempt = 1; attempt <= m_config.max_retries; ++attempt) {
        for (const std::string& source : candidates) {
            if (stop.stop_requested()) {
                return util::Result<void>::fail(cancelled_error());
            }
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                SourceState& state = m_sources[source];
                if (state.disabled) {
                    // D-086：冷却到期后清零并重新尝试（半开）；未到期才跳过。
                    if (state.disabled_until != std::chrono::steady_clock::time_point{} &&
                        std::chrono::steady_clock::now() >= state.disabled_until) {
                        state.disabled = false;
                        state.fail_count = 0;
                        state.disabled_until = {};
                        util::log_info("[download] source re-enabled after cooldown: " +
                                       source);
                    } else {
                        continue;
                    }
                }
            }

            util::Result<void> result = try_source(source, request, forward, stop);
            if (result.is_ok()) {
                util::Result<void> verified = verify_hash(request);
                if (verified.is_ok()) {
                    record_success(source);
                    return util::Result<void>::ok();
                }
                // 校验失败：丢弃文件换源（§7.4）
                ::DeleteFileW(temp.c_str());
                ::DeleteFileW(request.destination.c_str());
                last_error = std::move(verified.error());
                record_failure(source, last_error);
                util::log_warn("[download] hash mismatch from " + source);
                continue;
            }

            if (result.error().code == "download.cancelled") {
                return result;
            }
            last_error = std::move(result.error());
            record_failure(source, last_error);
            ::DeleteFileW(temp.c_str());
        }
        if (stop.stop_requested()) {
            return util::Result<void>::fail(cancelled_error());
        }
        // 退避 500ms×n（knowledge §3）
        std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
    }

    return util::Result<void>::fail(util::Error::make(
        "download.all_sources_failed", "下载失败：" + request.display_name,
        last_error.code + " " + last_error.detail));
}

util::Result<void> DownloadScheduler::try_source(const std::string& source_url,
                                                 const DownloadRequest& request,
                                                 const DownloadProgressFn& progress,
                                                 std::stop_token stop) {
    // 细碎文件仍只发一个 GET（D-061 的结论不变）；清单已声明大小的大文件走分块。
    // 总大小未知时无法切段，直接单流。D-100：Mod 资源由调用方提示 2~4 线程。
    const std::filesystem::path temp = make_download_temp_path(request.destination);
    // D-100：调用方可提示分块数（Mod 资源）；过小（<256KB）时仍走单流，
    // 切分只会增加握手开销（PCL 的 FilePieceLimit 同样是 256KB）。
    int threads = request.piece_threads > 0 ? request.piece_threads
                                            : download_piece_threads(request.expected_size);
    if (request.expected_size > 0 && request.expected_size < 256 * 1024) {
        threads = 1;
    }
    if (threads > 1) {
        PieceThreadLease lease(threads);
        if (lease.granted() >= 2) {
            util::log_info("[download] ranged " + std::to_string(lease.granted()) +
                           " thread(s) for " + std::to_string(request.expected_size) +
                           " bytes: " + source_url);
            util::Result<void> ranged =
                download_ranged(source_url, temp, request, lease.granted(), progress, stop);
            if (ranged.is_ok()) {
                return ranged;
            }
            ::DeleteFileW(temp.c_str());
            if (ranged.error().code != "download.range_unsupported") {
                return ranged;  // 网络/写失败：交给上层换源/重试
            }
            // 服务器不支持 Range（返回 200 或分段大小不符）：本文件回退单流。
            util::log_warn("[download] range unsupported, fallback to single stream: " +
                           source_url);
        } else {
            util::log_info("[download] piece thread budget exhausted, single stream: " +
                           source_url);
        }
    }
    return download_single(source_url, temp, request.destination, progress, stop);
}

util::Result<void> DownloadScheduler::download_single(const std::string& source_url,
                                                      const std::filesystem::path& temp,
                                                      const std::filesystem::path& destination,
                                                      const DownloadProgressFn& progress,
                                                      std::stop_token stop) {
    platform::HttpRequest request;
    request.method = "GET";
    request.url = source_url;
    request.receive_timeout_ms = m_config.request_timeout_ms;

    util::Result<platform::HttpStream> opened = platform::open_http_stream(request);
    if (opened.is_error()) {
        return util::Result<void>::fail(std::move(opened.error()));
    }
    platform::HttpStream stream = std::move(opened.value());
    if (stream.status_code() != 200 && stream.status_code() != 206) {
        return util::Result<void>::fail(util::Error::make(
            "download.http_status", "下载失败：服务器返回 " + std::to_string(stream.status_code()),
            source_url + " status " + std::to_string(stream.status_code())));
    }

    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
        return util::Result<void>::fail(
            util::Error::make("download.write_failed", "无法创建临时文件", source_url));
    }
    char buffer[kIoBuffer];
    std::uint64_t downloaded = 0;
    auto window_start = std::chrono::steady_clock::now();
    std::uint64_t window_bytes = 0;
    for (;;) {
        if (stop.stop_requested()) {
            out.close();
            ::DeleteFileW(temp.c_str());
            return util::Result<void>::fail(cancelled_error());
        }
        util::Result<std::size_t> read = stream.read(buffer, sizeof(buffer));
        if (read.is_error()) {
            out.close();
            ::DeleteFileW(temp.c_str());
            return util::Result<void>::fail(std::move(read.error()));
        }
        if (read.value() == 0) {
            break;
        }
        if (!m_limiter.consume(read.value(), stop)) {
            out.close();
            ::DeleteFileW(temp.c_str());
            return util::Result<void>::fail(cancelled_error());
        }
        out.write(buffer, static_cast<std::streamsize>(read.value()));
        // D-086：写失败（磁盘满/句柄错误）过去被静默忽略，随后 MoveFileExW
        // 会把不完整文件当作成功落盘。必须在每次写入后检查流状态。
        if (!out.good()) {
            out.close();
            ::DeleteFileW(temp.c_str());
            return util::Result<void>::fail(util::Error::make(
                "download.write_failed", "写入临时文件失败", source_url));
        }
        downloaded += read.value();
        window_bytes += read.value();
        const auto now = std::chrono::steady_clock::now();
        if (now - window_start >= kStallWindow) {
            // D-107：仅「官方优先，加载缓慢时换用镜像源」档启用低速中止；
            // 「尽量使用官方源」档不因慢速换源，只在真正失败时才换。
            if (global_source_policy().files_slow_switch() &&
                    window_bytes < kStallMinBytes) {
                out.close();
                ::DeleteFileW(temp.c_str());
                util::log_warn("[download] stalled (low speed), abort: " + source_url);
                return util::Result<void>::fail(util::Error::make(
                    "download.stalled", "下载速度过低，正在重试", source_url));
            }
            window_start = now;
            window_bytes = 0;
        }
        if (progress) {
            progress(downloaded, stream.content_length() > 0
                                     ? static_cast<std::uint64_t>(stream.content_length())
                                     : 0);
        }
    }
    out.close();
    if (!out.good()) {
        ::DeleteFileW(temp.c_str());
        return util::Result<void>::fail(util::Error::make(
            "download.write_failed", "关闭临时文件失败", source_url));
    }
    if (!::MoveFileExW(temp.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return util::Result<void>::fail(util::Error::make(
            "download.write_failed", "无法保存文件", platform::format_win_error(::GetLastError())));
    }
    return util::Result<void>::ok();
}

util::Result<void> DownloadScheduler::download_ranged(const std::string& source_url,
                                                      const std::filesystem::path& temp,
                                                      const DownloadRequest& request,
                                                      int threads,
                                                      const DownloadProgressFn& progress,
                                                      std::stop_token stop) {
    const std::int64_t total = request.expected_size;
    if (total <= 0 || threads < 2) {
        return util::Result<void>::fail(util::Error::make(
            "download.range_unsupported", "文件大小未知或线程数不足，无法分块", source_url));
    }

    // 预分配到最终大小：各分块用独立 HANDLE 在固定偏移写入，无需事后合并。
    HANDLE file = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return util::Result<void>::fail(util::Error::make(
            "download.write_failed", "无法创建临时文件",
            platform::format_win_error(::GetLastError())));
    }
    LARGE_INTEGER size_offset{};
    size_offset.QuadPart = total;
    const bool allocated =
        ::SetFilePointerEx(file, size_offset, nullptr, FILE_BEGIN) != FALSE &&
        ::SetEndOfFile(file) != FALSE;
    const unsigned long allocate_error = allocated ? 0UL : ::GetLastError();
    ::CloseHandle(file);
    if (!allocated) {
        ::DeleteFileW(temp.c_str());
        return util::Result<void>::fail(util::Error::make(
            "download.write_failed", "无法预分配临时文件",
            platform::format_win_error(allocate_error)));
    }

    struct Piece {
        std::int64_t start = 0;
        std::int64_t end = 0;  // 闭区间
    };
    std::vector<Piece> pieces;
    pieces.reserve(static_cast<std::size_t>(threads));
    const std::int64_t part = total / threads;
    for (int i = 0; i < threads; ++i) {
        Piece piece;
        piece.start = part * static_cast<std::int64_t>(i);
        piece.end = (i == threads - 1) ? total - 1 : part * static_cast<std::int64_t>(i + 1) - 1;
        if (piece.end < piece.start) {
            continue;
        }
        pieces.push_back(piece);
    }
    if (pieces.size() < 2) {
        ::DeleteFileW(temp.c_str());
        return util::Result<void>::fail(util::Error::make(
            "download.range_unsupported", "文件过小，无法切分为多个分块", source_url));
    }

    std::atomic<std::uint64_t> done{0};
    std::atomic<bool> failed{false};
    std::mutex error_mutex;
    std::mutex progress_mutex;  // D-086：串行化多分块线程的进度回调
    util::Error first_error = util::Error::make("download.failed", "分块下载失败", source_url);

    std::vector<std::thread> workers;
    workers.reserve(pieces.size());
    for (const Piece& piece : pieces) {
        workers.emplace_back([this, &source_url, &temp, &progress, stop, &done, &failed,
                              &error_mutex, &progress_mutex, &first_error, piece, total]() {
            util::Result<void> result = download_piece(source_url, temp, piece.start, piece.end,
                                                       total, &done, progress, &progress_mutex,
                                                       stop, &failed);
            if (result.is_error()) {
                std::lock_guard<std::mutex> lock(error_mutex);
                if (!failed.exchange(true)) {
                    first_error = result.error();
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    if (failed.load()) {
        ::DeleteFileW(temp.c_str());
        return util::Result<void>::fail(first_error);
    }
    // 与 download_single 一致：全部完成后把临时文件原子替换到目标路径。
    // 漏掉这一步会让 verify_hash 去读 destination（不存在）→ hash.read_failed，
    // 继而误判哈希失败、删文件换源、无限重下（轮次 114 修复的回归）。
    if (!::MoveFileExW(temp.c_str(), request.destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const unsigned long move_error = ::GetLastError();
        ::DeleteFileW(temp.c_str());
        return util::Result<void>::fail(util::Error::make(
            "download.write_failed", "无法保存文件",
            platform::format_win_error(move_error)));
    }
    return util::Result<void>::ok();
}

util::Result<void> DownloadScheduler::download_piece(const std::string& source_url,
                                                     const std::filesystem::path& temp,
                                                     std::int64_t start, std::int64_t end,
                                                     std::int64_t total,
                                                     std::atomic<std::uint64_t>* done,
                                                     const DownloadProgressFn& progress,
                                                     std::mutex* progress_mutex,
                                                     std::stop_token stop,
                                                     std::atomic<bool>* failed) {
    if (stop.stop_requested() || (failed != nullptr && failed->load())) {
        return util::Result<void>::fail(cancelled_error());
    }

    platform::HttpRequest request;
    request.method = "GET";
    request.url = source_url;
    request.receive_timeout_ms = m_config.request_timeout_ms;
    request.headers.emplace_back(
        "Range", "bytes=" + std::to_string(start) + "-" + std::to_string(end));

    util::Result<platform::HttpStream> opened = platform::open_http_stream(request);
    if (opened.is_error()) {
        return util::Result<void>::fail(std::move(opened.error()));
    }
    platform::HttpStream stream = std::move(opened.value());

    // 200 = 服务器忽略了 Range（返回整个文件），必须回退单流，绝不能把全文写进一个分块。
    if (stream.status_code() == 200) {
        return util::Result<void>::fail(util::Error::make(
            "download.range_unsupported", "下载源不支持分段下载（返回 200）", source_url));
    }
    if (stream.status_code() != 206) {
        const int status = stream.status_code();
        // PCL 同款判定：416（Range Not Satisfiable）说明该源对分段请求不友好，
        // 与「不支持分块」等价，回退同源单流，而不是直接换源/失败。
        if (status == 416 || status == 405 || status == 501) {
            return util::Result<void>::fail(util::Error::make(
                "download.range_unsupported",
                "下载源拒绝分段请求（HTTP " + std::to_string(status) + "）",
                source_url + " status " + std::to_string(status)));
        }
        return util::Result<void>::fail(util::Error::make(
            "download.http_status",
            "分段下载失败：服务器返回 " + std::to_string(status),
            source_url + " status " + std::to_string(status)));
    }
    const std::int64_t expected = end - start + 1;
    const std::int64_t declared = stream.content_length();
    if (declared > 0 && declared != expected) {
        return util::Result<void>::fail(util::Error::make(
            "download.range_unsupported", "分段大小与请求不一致",
            std::to_string(declared) + " != " + std::to_string(expected)));
    }

    // 每线程独立 HANDLE（FILE_SHARE_WRITE），用 SetFilePointerEx 定位到自己的区间。
    HANDLE file = ::CreateFileW(temp.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return util::Result<void>::fail(util::Error::make(
            "download.write_failed", "无法打开临时文件",
            platform::format_win_error(::GetLastError())));
    }
    LARGE_INTEGER offset{};
    offset.QuadPart = start;
    if (!::SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) {
        const unsigned long seek_error = ::GetLastError();
        ::CloseHandle(file);
        return util::Result<void>::fail(util::Error::make(
            "download.write_failed", "无法定位临时文件", platform::format_win_error(seek_error)));
    }

    char buffer[kIoBuffer];
    std::int64_t remaining = expected;
    auto window_start = std::chrono::steady_clock::now();
    std::uint64_t window_bytes = 0;
    while (remaining > 0) {
        if (stop.stop_requested() || (failed != nullptr && failed->load())) {
            ::CloseHandle(file);
            return util::Result<void>::fail(cancelled_error());
        }
        const std::size_t want = static_cast<std::size_t>(
            (std::min)(remaining, static_cast<std::int64_t>(sizeof(buffer))));
        util::Result<std::size_t> read = stream.read(buffer, want);
        if (read.is_error()) {
            ::CloseHandle(file);
            return util::Result<void>::fail(std::move(read.error()));
        }
        if (read.value() == 0) {
            ::CloseHandle(file);
            return util::Result<void>::fail(util::Error::make(
                "download.range_unsupported", "分段提前结束（内容不足）", source_url));
        }
        if (!m_limiter.consume(read.value(), stop)) {
            ::CloseHandle(file);
            return util::Result<void>::fail(cancelled_error());
        }
        std::size_t written_total = 0;
        while (written_total < read.value()) {
            DWORD written = 0;
            const DWORD chunk = static_cast<DWORD>(read.value() - written_total);
            if (!::WriteFile(file, buffer + written_total, chunk, &written, nullptr)) {
                const unsigned long write_error = ::GetLastError();
                ::CloseHandle(file);
                return util::Result<void>::fail(util::Error::make(
                    "download.write_failed", "写入临时文件失败",
                    platform::format_win_error(write_error)));
            }
            if (written == 0) {
                ::CloseHandle(file);
                return util::Result<void>::fail(util::Error::make(
                    "download.write_failed", "写入临时文件失败", "WriteFile 写入 0 字节"));
            }
            written_total += written;
        }
        remaining -= static_cast<std::int64_t>(read.value());
        window_bytes += read.value();
        const auto now = std::chrono::steady_clock::now();
        if (now - window_start >= kStallWindow) {
            // D-107：仅「官方优先，加载缓慢时换用镜像源」档启用低速中止；
            // 「尽量使用官方源」档不因慢速换源，只在真正失败时才换。
            if (global_source_policy().files_slow_switch() &&
                    window_bytes < kStallMinBytes) {
                // D-100：分块线程同样做低速判定，避免整段卡在极慢连接上。
                ::CloseHandle(file);
                return util::Result<void>::fail(util::Error::make(
                    "download.stalled", "下载速度过低，正在重试", source_url));
            }
            window_start = now;
            window_bytes = 0;
        }
        if (done != nullptr) {
            const std::uint64_t added = static_cast<std::uint64_t>(read.value());
            const std::uint64_t current =
                done->fetch_add(added, std::memory_order_relaxed) + added;
            if (progress) {
                // D-086：分块线程并发回调会让上层按引用维护的 last 参与数据竞争；
                // 用共享 mutex 串行化，保证进度单调且无 UB。
                if (progress_mutex != nullptr) {
                    std::lock_guard<std::mutex> lock(*progress_mutex);
                    progress(current, static_cast<std::uint64_t>(total));
                } else {
                    progress(current, static_cast<std::uint64_t>(total));
                }
            }
        }
    }
    ::CloseHandle(file);
    return util::Result<void>::ok();
}

ScheduledDownloadTask::ScheduledDownloadTask(DownloadRequest request, SchedulerConfig config)
    : m_request(std::move(request)), m_scheduler(std::move(config)) {
    set_name(m_request.display_name.empty() ? m_request.url : m_request.display_name);
}

util::Result<void> ScheduledDownloadTask::do_run() {
    return m_scheduler.download_file(
        m_request,
        [this](std::uint64_t done, std::uint64_t total) { set_progress(done, total, name()); },
        m_stop_source.get_token());
}

void ScheduledDownloadTask::do_cancel() {
    m_stop_source.request_stop();
}

}  // namespace sscl::core
