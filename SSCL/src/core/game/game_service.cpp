#include "core/game/game_service.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/config/build_config.hpp"
#include "core/download/adaptive_concurrency.hpp"
#include "core/game/instance_launch.hpp"
#include "core/game/pcl_version.hpp"
#include "core/java/java_downloader.hpp"
#include "core/java/java_manager.hpp"
#include "core/launch/natives.hpp"
#include "core/loader/installer_profile.hpp"
#include "core/loader/loader_installer.hpp"
#include "core/loader/processor_runner.hpp"
#include "platform/file_utils.hpp"
#include "platform/path_utils.hpp"
#include "platform/process.hpp"
#include "platform/shell_utils.hpp"
#include "platform/zip_utils.hpp"
#include "util/hash.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

namespace sscl::core {

namespace {

// D-107「辅助功能：自动设置游戏为中文（简体）」：启动前把 options.txt 的
// lang 行设为 zh_cn，保留玩家其它设置；关闭勾选时完全不改动。
void apply_game_language(const std::filesystem::path& game_dir, bool chinese) {
    if (!chinese || game_dir.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(game_dir, ec);
    const std::filesystem::path file = game_dir / "options.txt";
    const util::Result<std::string> existing = platform::read_file(file);
    const std::string content = existing.is_ok() ? existing.value() : std::string();
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < content.size()) {
        const std::size_t end = content.find('\n', pos);
        std::string line = content.substr(
            pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        if (end == std::string::npos) {
            break;
        }
        pos = end + 1;
    }
    bool found = false;
    for (std::string& line : lines) {
        if (line.rfind("lang:", 0) == 0) {
            line = "lang:zh_cn";
            found = true;
        }
    }
    if (!found) {
        lines.push_back("lang:zh_cn");
    }
    std::string out;
    for (const std::string& line : lines) {
        out += line;
        out.push_back('\n');
    }
    const util::Result<void> saved = platform::write_file_atomic(file, out);
    if (saved.is_error()) {
        util::log_warn("[game] write options.txt for zh_cn failed: " + saved.error().detail);
    }
}

}  // namespace

namespace {
StageProgress* stage_of(InstallProgress* state, InstallStage stage) {
    return stage == InstallStage::kVersion ? &state->version : &state->assets;
}

const StageProgress* stage_of(const InstallProgress& state, InstallStage stage) {
    return stage == InstallStage::kVersion ? &state.version : &state.assets;
}

std::filesystem::path find_version_java(const std::filesystem::path& version_dir) {
    const wchar_t* kCandidates[] = {
        L"jre/bin/java.exe",
        L"java/bin/java.exe",
        L"runtime/bin/java.exe",
        L"bin/java.exe",
    };
    for (const wchar_t* relative : kCandidates) {
        const std::filesystem::path candidate = version_dir / relative;
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

// D-117：未完成任务的总进度上限。左栏按 %.1f 展示，0.995 会四舍五入成 "100.0%"，
// 因此留出 1% 余量；只有 complete() 之后才允许显示 100%。
constexpr double kUnfinishedFractionCap = 0.99;

// D-117：带加载器的安装任务，总进度按固定子区间划分（原版 0~50%，加载器各步 50~100%）。
// 加载器的安装器/库/处理器总量无法事先算出，用「子区间 + 段内本地进度」保证
// 进度单调、不会在第一段结束就冲到 100%（旧实现的两个症状）。
constexpr double kVanillaSegmentEnd = 0.50;      // 原版下载占用 0~50%
constexpr double kInstallerSegmentEnd = 0.62;    // 下载安装器 jar
constexpr double kInstallerLibsSegmentEnd = 0.80;  // 安装器自带库
constexpr double kProcessorSegmentEnd = 0.95;    // 安装器处理器（步骤进度）

// 阶段权重：已知大小的字节 + 大小未知文件的"平均大小"估计（保证总进度单调、到 100% 收敛）。
struct StageWeight {
    double numerator = 0.0;
    double denominator = 0.0;
};

StageWeight stage_weight(const StageProgress& progress) {
    const std::uint64_t known_files =
        progress.files_total > progress.files_unknown
            ? progress.files_total - progress.files_unknown
            : 0;
    const double average =
        known_files > 0 ? static_cast<double>(progress.bytes_total) /
                              static_cast<double>(known_files)
                        : 0.0;
    StageWeight weight;
    weight.numerator = static_cast<double>(progress.bytes_done) +
                       static_cast<double>(progress.unknown_done) * average;
    weight.denominator = static_cast<double>(progress.bytes_total) +
                         static_cast<double>(progress.files_unknown) * average;
    return weight;
}

void recompute_stage(StageProgress* progress) {
    const StageWeight weight = stage_weight(*progress);
    double fraction = 0.0;
    if (weight.denominator > 0.0) {
        fraction = weight.numerator / weight.denominator;
    } else if (progress->files_total > 0) {
        fraction = static_cast<double>(progress->files_done) /
                   static_cast<double>(progress->files_total);
    }
    progress->fraction = static_cast<float>((std::min)(1.0, (std::max)(0.0, fraction)));
}

// D-087：列出 inheritsFrom 直接指向 version_id 的子版本。
// 删除/重命名父版本前必须调用，否则子加载器版本会永久无法启动。
std::vector<std::string> find_dependent_versions(const std::filesystem::path& versions_dir,
                                                 const std::string& version_id) {
    std::vector<std::string> dependents;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(versions_dir, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) {
            continue;
        }
        const std::string child = util::wide_to_utf8(it->path().filename().wstring());
        if (child == version_id) {
            continue;
        }
        util::Result<std::string> text =
            platform::read_file(it->path() / (it->path().filename().wstring() + L".json"));
        if (text.is_error()) {
            continue;
        }
        const nlohmann::json parsed = nlohmann::json::parse(text.value(), nullptr, false);
        if (parsed.is_object() && parsed.contains("inheritsFrom") &&
            parsed["inheritsFrom"].is_string() &&
            parsed["inheritsFrom"].get<std::string>() == version_id) {
            dependents.push_back(child);
        }
    }
    return dependents;
}

std::string join_names(const std::vector<std::string>& names) {
    std::string text;
    for (const std::string& name : names) {
        if (!text.empty()) {
            text += "、";
        }
        text += name;
    }
    return text;
}
}  // namespace

// —— ProgressReporter（D-019 进度/速度；D-045 三进度条 + 单调总量）——————

ProgressReporter::ProgressReporter(InstallProgressFn fn) : m_fn(std::move(fn)) {}

void ProgressReporter::set_phase(const std::string& phase) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state.phase = phase;
    flush_locked(true);
}

void ProgressReporter::set_detail(const std::string& detail) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state.detail = detail;
    flush_locked(true);
}

void ProgressReporter::set_stage_totals(InstallStage stage, std::uint64_t files,
                                        std::uint64_t bytes, std::uint64_t unknown_files) {
    std::lock_guard<std::mutex> lock(m_mutex);
    StageProgress* progress = stage_of(&m_state, stage);
    progress->files_total = files;
    progress->bytes_total = bytes;
    progress->files_unknown = (std::min)(unknown_files, files);
    progress->files_done = 0;
    progress->bytes_done = 0;
    progress->unknown_done = 0;
    recompute_stage(&m_state.version);
    recompute_stage(&m_state.assets);
    flush_locked(true);
}

void ProgressReporter::add_stage_totals(InstallStage stage, std::uint64_t files,
                                        std::uint64_t bytes, std::uint64_t unknown_files) {
    std::lock_guard<std::mutex> lock(m_mutex);
    StageProgress* progress = stage_of(&m_state, stage);
    progress->files_total += files;
    progress->bytes_total += bytes;
    progress->files_unknown += (std::min)(unknown_files, files);
    recompute_stage(&m_state.version);
    recompute_stage(&m_state.assets);
    flush_locked(true);
}

std::uint64_t ProgressReporter::begin_file(const std::string& name,
                                           std::uint64_t expected_total) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::uint64_t id = ++m_next_file_id;
    ActiveDownloadInfo info;
    info.id = id;
    info.name = name;
    info.total = expected_total;
    m_active[id] = info;

    DownloadEvent event;
    event.kind = DownloadEventKind::kStarted;
    event.id = id;
    event.name = name;
    event.bytes = expected_total;
    m_events.push_back(std::move(event));
    flush_locked(false);
    return id;
}

void ProgressReporter::progress(InstallStage stage, std::uint64_t id, std::uint64_t delta,
                                std::uint64_t current, std::uint64_t file_total,
                                bool count_bytes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    StageProgress* progress = stage_of(&m_state, stage);
    if (count_bytes) {
        progress->bytes_done += delta;
    }
    const auto it = m_active.find(id);
    if (it != m_active.end()) {
        it->second.bytes = current;
        if (file_total > it->second.total) {
            it->second.total = file_total;
        }
    }
    recompute_stage(&m_state.version);
    recompute_stage(&m_state.assets);
    flush_locked(false);
}

void ProgressReporter::end_file(InstallStage stage, std::uint64_t id,
                                std::uint64_t remaining_delta, bool count_bytes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    StageProgress* progress = stage_of(&m_state, stage);
    if (count_bytes) {
        progress->bytes_done += remaining_delta;
    } else if (progress->unknown_done < progress->files_unknown) {
        // D-117：只有被登记为「大小未知」的文件才消耗 unknown 配额。
        // 安装器这类按 count_bytes=false 上报、但未登记进 files_unknown 的文件，
        // 若也 +1 会凭空抬高分子（unknown_done × 平均大小），让总进度提前冲顶。
        ++progress->unknown_done;
    }
    if (progress->files_total == 0 || progress->files_done < progress->files_total) {
        ++progress->files_done;
    }

    DownloadEvent event;
    event.kind = DownloadEventKind::kFinished;
    event.id = id;
    const auto it = m_active.find(id);
    if (it != m_active.end()) {
        event.name = it->second.name;
        event.bytes = it->second.bytes + (count_bytes ? remaining_delta : 0);
        m_active.erase(it);
    }
    m_events.push_back(std::move(event));

    recompute_stage(&m_state.version);
    recompute_stage(&m_state.assets);
    flush_locked(false);
}

void ProgressReporter::drop_file(std::uint64_t id, bool failed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_active.find(id);
    if (it == m_active.end()) {
        return;
    }
    if (failed) {
        DownloadEvent event;
        event.kind = DownloadEventKind::kFailed;
        event.id = id;
        event.name = it->second.name;
        event.bytes = it->second.bytes;
        m_events.push_back(std::move(event));
    }
    m_active.erase(it);
    flush_locked(false);
}

void ProgressReporter::set_step(std::uint64_t done, std::uint64_t total, bool indeterminate) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state.step_done = done;
    m_state.step_total = total;
    m_state.step_indeterminate = indeterminate;
    flush_locked(true);
}

void ProgressReporter::clear_step() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state.step_done == 0 && m_state.step_total == 0 && !m_state.step_indeterminate) {
        return;
    }
    m_state.step_done = 0;
    m_state.step_total = 0;
    m_state.step_indeterminate = false;
    flush_locked(true);
}

void ProgressReporter::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    flush_locked(true);
}

void ProgressReporter::set_progress_segment(double begin, double end) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_segment_begin = (std::max)(0.0, (std::min)(1.0, begin));
    m_segment_end = (std::max)(m_segment_begin, (std::min)(1.0, end));
    // 进入新段：上一步的步骤进度不再参与映射（处理器 i/N 只属于它自己的区间），
    // 否则切换段的那一次 flush 会把上一段的 100% 步骤进度带进新段、瞬间冲到 99%。
    m_state.step_done = 0;
    m_state.step_total = 0;
    m_state.step_indeterminate = false;
    // 基线：已经完成的下载不再计入下一段，否则切换瞬间本地进度仍是 1.0，
    // 映射后会直接跳到段尾（加载器阶段一开始就 100%）。
    const StageWeight version_weight = stage_weight(m_state.version);
    const StageWeight assets_weight = stage_weight(m_state.assets);
    m_segment_base_numerator = version_weight.numerator + assets_weight.numerator;
    m_segment_base_denominator = version_weight.denominator + assets_weight.denominator;
    m_segment_base_files_done = m_state.version.files_done + m_state.assets.files_done;
    m_segment_base_files_total = m_state.version.files_total + m_state.assets.files_total;
    flush_locked(true);
}

void ProgressReporter::complete() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_completed = true;
    flush_locked(true);
}

std::uint64_t ProgressReporter::bytes_done() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state.bytes_done;
}

void ProgressReporter::flush_locked(bool force) {
    // 汇总视图（每次上报前重算，保证 UI 拿到一致快照）
    const StageWeight version_weight = stage_weight(m_state.version);
    const StageWeight assets_weight = stage_weight(m_state.assets);
    m_state.bytes_done = m_state.version.bytes_done + m_state.assets.bytes_done;
    m_state.bytes_total = m_state.version.bytes_total + m_state.assets.bytes_total;
    m_state.completed_files = m_state.version.files_done + m_state.assets.files_done;
    m_state.total_files = m_state.version.files_total + m_state.assets.files_total;

    // D-117 段内本地进度：扣掉进入本段时的基线，避免上一段的完成量重复计入。
    const double numerator = version_weight.numerator + assets_weight.numerator;
    const double denominator = version_weight.denominator + assets_weight.denominator;
    const double segment_numerator = numerator - m_segment_base_numerator;
    const double segment_denominator = denominator - m_segment_base_denominator;
    double local = 0.0;
    if (segment_denominator > 0.0) {
        local = segment_numerator / segment_denominator;
    } else if (m_state.total_files > m_segment_base_files_total &&
               m_state.completed_files > m_segment_base_files_done) {
        local = static_cast<double>(m_state.completed_files - m_segment_base_files_done) /
                static_cast<double>(m_state.total_files - m_segment_base_files_total);
    }
    // 安装器处理器 / Java 检测：步骤进度同样推进总进度，
    // 否则加载器阶段既没有下载字节、进度条也不动，看起来像卡死。
    if (m_state.step_total > 0) {
        const double step = static_cast<double>(m_state.step_done) /
                            static_cast<double>(m_state.step_total);
        local = (std::max)(local, step);
    }
    local = (std::min)(1.0, (std::max)(0.0, local));

    double fraction = m_segment_begin + (m_segment_end - m_segment_begin) * local;
    if (m_completed) {
        fraction = 1.0;  // 任务真正结束才允许 100%
    } else {
        // 未完成绝不显示 100%：左栏按 %.1f 展示，0.995 会四舍五入成 "100.0%"。
        fraction = (std::min)(fraction, kUnfinishedFractionCap);
        // 单调护栏：同一次任务内只允许前进（分段切换/追加总量都不该让进度倒退）。
        fraction = (std::max)(fraction, static_cast<double>(m_last_fraction));
    }
    m_state.fraction = static_cast<float>((std::min)(1.0, (std::max)(0.0, fraction)));
    m_last_fraction = m_state.fraction;

    if (!m_fn) {
        m_events.clear();
        return;
    }
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (!force && m_flushed_once &&
        (now - m_last_flush) < std::chrono::milliseconds(100)) {
        return;
    }
    m_flushed_once = true;
    m_last_flush = now;

    // 活动文件/事件快照：只在真正上报时构建并搬运（节流期间不分配、不复制）。
    m_state.active.clear();
    m_state.active.reserve(m_active.size());
    for (const auto& entry : m_active) {
        m_state.active.push_back(entry.second);
    }
    m_state.events = std::move(m_events);
    m_fn(m_state);
}

namespace {

struct DownloadJob {
    const DownloadRequest* request = nullptr;
    InstallStage stage = InstallStage::kVersion;
};

// 自适应并发下载池（批次 42，D-061）：
// - 固定 max_concurrent 个 worker（≤32）阻塞等待门限，按 AdaptiveConcurrency 的目标放行；
// - 每 500ms 用 ProgressReporter 的累计字节采样吞吐，AIMD 调整目标并发；
// - 任一文件最终失败 → 取消其余下载并返回首个错误；
// - 上游 stop_token 通过 stop_callback 桥接到内部 stop_source。
class AdaptiveDownloadPool {
public:
    AdaptiveDownloadPool(DownloadScheduler& scheduler, ProgressReporter* reporter,
                         std::stop_token stop)
        : m_scheduler(scheduler),
          m_reporter(reporter),
          m_cancel(),
          m_stop_bridge(stop, std::function<void()>([this]() { m_cancel.request_stop(); })) {
        m_target = m_adaptive.target();
    }

    util::Result<void> run(const std::vector<DownloadJob>& jobs) {
        if (jobs.empty()) {
            return util::Result<void>::ok();
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_jobs = &jobs;
            m_next = 0;
            m_active = 0;
            m_stopping = false;
            m_has_error = false;
            m_target = m_adaptive.target();
        }

        const int worker_count = m_adaptive.config().max_concurrent;
        std::vector<std::thread> workers;
        workers.reserve(static_cast<std::size_t>(worker_count));
        for (int i = 0; i < worker_count; ++i) {
            workers.emplace_back([this]() { worker_loop(); });
        }
        std::thread controller([this]() { controller_loop(); });
        controller.join();

        // 让所有 worker 看到停止/完成状态并退出。
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_cv.notify_all();
        for (std::thread& worker : workers) {
            worker.join();
        }
        m_jobs = nullptr;
        if (m_reporter != nullptr) {
            m_reporter->flush();  // 收尾：确保最后一批文件事件送达 UI
        }

        if (m_has_error) {
            return util::Result<void>::fail(m_first_error);
        }
        if (m_cancel.get_token().stop_requested()) {
            return util::Result<void>::fail(
                util::Error::make("download.cancelled", "安装已取消", {}));
        }
        return util::Result<void>::ok();
    }

private:
    void worker_loop() {
        for (;;) {
            DownloadJob job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this]() {
                    return m_stopping || m_cancel.get_token().stop_requested() ||
                           m_next >= m_jobs->size() || m_active < m_target;
                });
                if (m_stopping || m_cancel.get_token().stop_requested() ||
                    m_next >= m_jobs->size()) {
                    return;
                }
                job = (*m_jobs)[m_next++];
                ++m_active;
            }

            download_one(job);

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                --m_active;
            }
            m_cv.notify_all();
        }
    }

    void download_one(const DownloadJob& job) {
        const DownloadRequest& request = *job.request;
        const InstallStage stage = job.stage;
        const bool known = request.expected_size > 0;
        const std::uint64_t expected =
            known ? static_cast<std::uint64_t>(request.expected_size) : 0;

        std::uint64_t id = 0;
        if (m_reporter != nullptr) {
            id = m_reporter->begin_file(request.display_name, expected);
        }

        std::uint64_t last = 0;
        DownloadProgressFn on_progress;
        if (m_reporter != nullptr) {
            on_progress = [this, stage, id, known, &last](std::uint64_t current,
                                                         std::uint64_t total) {
                const std::uint64_t delta = current > last ? current - last : 0;
                last = current;
                m_reporter->progress(stage, id, delta, current, total, known);
            };
        }

        util::Result<void> downloaded =
            m_scheduler.download_file(request, on_progress, m_cancel.get_token());
        if (downloaded.is_ok()) {
            if (m_reporter != nullptr) {
                const std::uint64_t remaining =
                    known && expected > last ? expected - last : 0;
                m_reporter->end_file(stage, id, remaining, known);
            }
            return;
        }

        const bool cancelled = downloaded.error().code == "download.cancelled";
        if (m_reporter != nullptr) {
            m_reporter->drop_file(id, !cancelled);
        }
        if (cancelled) {
            return;  // 上游取消：不记错误
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_has_error) {
                m_has_error = true;
                m_first_error = downloaded.error();
            }
            m_stopping = true;
            m_adaptive.on_error();
        }
        m_cancel.request_stop();
        m_cv.notify_all();
    }

    void controller_loop() {
        constexpr auto kSampleInterval = std::chrono::milliseconds(500);
        auto last_time = std::chrono::steady_clock::now();
        std::uint64_t last_bytes = m_reporter != nullptr ? m_reporter->bytes_done() : 0;

        for (;;) {
            std::uint64_t remaining = 0;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait_for(lock, kSampleInterval, [this]() {
                    return m_stopping || m_cancel.get_token().stop_requested() ||
                           all_done_locked();
                });
                if (m_stopping || m_cancel.get_token().stop_requested() || all_done_locked()) {
                    return;
                }
                remaining = m_jobs->size() - m_next;
            }

            if (m_reporter == nullptr) {
                continue;  // 没有进度回调 → 无法采样，保持起始目标
            }

            const auto now = std::chrono::steady_clock::now();
            const std::uint64_t bytes = m_reporter->bytes_done();
            const double dt = std::chrono::duration<double>(now - last_time).count();
            double speed = 0.0;
            if (dt > 0.001 && bytes >= last_bytes) {
                speed = static_cast<double>(bytes - last_bytes) / dt;
            }
            last_time = now;
            last_bytes = bytes;

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_target = m_adaptive.sample(speed, remaining);
            }
            m_cv.notify_all();
        }
    }

    bool all_done_locked() const {
        return m_jobs != nullptr && m_next >= m_jobs->size() && m_active == 0;
    }

    DownloadScheduler& m_scheduler;
    ProgressReporter* m_reporter = nullptr;
    // m_cancel 必须先于 m_stop_bridge 构造：若上游 stop_token 已请求停止，
    // stop_callback 会在注册时立即回调，此时 m_cancel 必须已经存在。
    std::stop_source m_cancel;
    std::stop_callback<std::function<void()>> m_stop_bridge;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    const std::vector<DownloadJob>* m_jobs = nullptr;
    std::size_t m_next = 0;
    int m_active = 0;
    int m_target = 8;
    bool m_stopping = false;
    bool m_has_error = false;
    util::Error m_first_error = util::Error::make("download.failed", "下载失败", {});
    AdaptiveConcurrency m_adaptive;
};

util::Result<void> run_adaptive_downloads(DownloadScheduler& scheduler,
                                          const std::vector<DownloadJob>& jobs,
                                          ProgressReporter* reporter, std::stop_token stop) {
    AdaptiveDownloadPool pool(scheduler, reporter, stop);
    return pool.run(jobs);
}

}  // namespace

GameService::GameService(InstallLayout layout, OsInfo os, SchedulerConfig config)
    : m_layout(std::move(layout)), m_os(std::move(os)), m_scheduler(std::move(config)) {}

void GameService::set_version_loader(VersionTextLoader loader) {
    m_loader = std::move(loader);
}

void GameService::set_game_folder(const std::filesystem::path& game_dir) {
    m_layout.game_dir = game_dir;
    m_layout.versions_dir = game_dir / L"versions";
    m_layout.libraries_dir = game_dir / L"libraries";
    m_layout.assets_dir = game_dir / L"assets";
    m_loader = nullptr;  // 旧 lambda 捕获了旧 versions_dir
}

util::Result<VersionJson> GameService::load_version(const std::string& version_id) {
    if (!m_loader) {
        // 默认加载器：versions/<id>/<id>.json
        const std::filesystem::path versions_dir = m_layout.versions_dir;
        m_loader = [versions_dir](const std::string& id) -> util::Result<std::string> {
            return platform::read_file(versions_dir / id / (id + ".json"));
        };
    }
    return resolve_version_chain(m_loader, version_id);
}

std::vector<DownloadRequest> GameService::plan_version_install(const VersionJson& version) const {
    std::map<std::string, bool> features;
    return plan_version_files(version, m_layout, m_os, features);
}

bool GameService::up_to_date(const DownloadRequest& request) const {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(request.destination, ec)) {
        return false;
    }
    if (request.expected_sha1.empty() && request.expected_md5.empty()) {
        return true;  // 无校验信息：存在即视为完成
    }
    const util::HashAlgorithm algorithm =
        !request.expected_sha1.empty() ? util::HashAlgorithm::kSha1 : util::HashAlgorithm::kMd5;
    const std::string& expected =
        !request.expected_sha1.empty() ? request.expected_sha1 : request.expected_md5;
    util::Result<std::string> actual = util::hash_file_hex(algorithm, request.destination);
    return actual.is_ok() && util::hash_hex_equal(actual.value(), expected);
}

util::Result<void> GameService::install_version(const VersionJson& version,
                                                const std::vector<DownloadRequest>& requests,
                                                std::stop_token stop,
                                                const InstallProgressFn& progress) {
    ProgressReporter reporter(progress);
    const PendingDownloads pending = make_pending(requests);
    util::Result<void> downloaded = download_sequential(
        pending, InstallStage::kVersion, "版本文件", stop, progress ? &reporter : nullptr);
    if (downloaded.is_error()) {
        return downloaded;
    }
    // natives 解压（§7.1）
    const std::filesystem::path version_dir = m_layout.versions_dir / version.id;
    const std::filesystem::path natives_dir = version_dir / (version.id + "-natives");
    return extract_natives(version.libraries, m_layout.libraries_dir, m_os.name, true, natives_dir);
}

GameService::PendingDownloads GameService::make_pending(
    const std::vector<DownloadRequest>& requests) const {
    PendingDownloads pending;
    pending.requests.reserve(requests.size());
    for (const DownloadRequest& request : requests) {
        if (up_to_date(request)) {
            continue;
        }
        pending.requests.push_back(request);
        ++pending.files;
        if (request.expected_size > 0) {
            pending.bytes += static_cast<std::uint64_t>(request.expected_size);
        } else {
            ++pending.unknown_files;
        }
    }
    return pending;
}

util::Result<void> GameService::download_sequential(const PendingDownloads& pending,
                                                    InstallStage stage, const std::string& phase,
                                                    std::stop_token stop,
                                                    ProgressReporter* reporter,
                                                    bool manage_totals) {
    util::log_info("[install] " + phase + ": " + std::to_string(pending.requests.size()) +
                   " file(s) to fetch");
    if (reporter != nullptr && manage_totals) {
        reporter->set_phase(phase);
        reporter->set_stage_totals(stage, pending.files, pending.bytes, pending.unknown_files);
        reporter->set_detail("待下载 " + std::to_string(pending.files) + " 个文件（" +
                             std::to_string(pending.bytes / (1024 * 1024)) + " MB 已知）");
    }
    for (const DownloadRequest& request : pending.requests) {
        if (stop.stop_requested()) {
            return util::Result<void>::fail(
                util::Error::make("download.cancelled", "安装已取消", {}));
        }
        const bool known = request.expected_size > 0;
        const std::uint64_t expected =
            known ? static_cast<std::uint64_t>(request.expected_size) : 0;
        std::uint64_t id = 0;
        if (reporter != nullptr) {
            id = reporter->begin_file(request.display_name, expected);
        }
        std::uint64_t last = 0;
        const DownloadProgressFn file_progress =
            (reporter != nullptr)
                ? DownloadProgressFn([reporter, stage, id, known, &last](
                                         std::uint64_t current, std::uint64_t file_total) {
                      const std::uint64_t delta = current > last ? current - last : 0;
                      last = current;
                      reporter->progress(stage, id, delta, current, file_total, known);
                  })
                : nullptr;
        util::Result<void> downloaded = m_scheduler.download_file(request, file_progress, stop);
        if (downloaded.is_error()) {
            if (reporter != nullptr) {
                reporter->drop_file(id, downloaded.error().code != "download.cancelled");
            }
            return downloaded;
        }
        if (reporter != nullptr) {
            reporter->end_file(stage, id, known && expected > last ? expected - last : 0,
                               known);
        }
    }
    return util::Result<void>::ok();
}

util::Result<std::vector<DownloadRequest>> GameService::plan_asset_install(
    const VersionJson& version) const {
    if (!version.has_asset_index) {
        return util::Result<std::vector<DownloadRequest>>::ok({});
    }
    const std::filesystem::path index_path =
        m_layout.assets_dir / "indexes" / (version.asset_index.id + ".json");
    util::Result<std::string> text = platform::read_file(index_path);
    if (text.is_error()) {
        return util::Result<std::vector<DownloadRequest>>::fail(util::Error::make(
            "assets.index_missing", "资产索引缺失：" + version.asset_index.id,
            util::path_to_utf8(index_path)));
    }
    util::Result<AssetIndex> index = parse_asset_index(text.value());
    if (index.is_error()) {
        return util::Result<std::vector<DownloadRequest>>::fail(std::move(index.error()));
    }
    return util::Result<std::vector<DownloadRequest>>::ok(
        plan_asset_files(index.value(), m_layout));
}

util::Result<void> GameService::install_assets(const VersionJson& version, std::stop_token stop,
                                               const InstallProgressFn& progress) {
    ProgressReporter reporter(progress);
    util::Result<std::vector<DownloadRequest>> planned = plan_asset_install(version);
    if (planned.is_error()) {
        return util::Result<void>::fail(std::move(planned.error()));
    }
    const PendingDownloads pending = make_pending(planned.value());
    ProgressReporter* reporter_ptr = progress ? &reporter : nullptr;
    if (reporter_ptr != nullptr) {
        reporter_ptr->set_phase("资源文件");
        reporter_ptr->set_stage_totals(InstallStage::kAssets, pending.files, pending.bytes,
                                       pending.unknown_files);
        reporter_ptr->set_detail("待下载 " + std::to_string(pending.files) + " 个文件（" +
                                 std::to_string(pending.bytes / (1024 * 1024)) + " MB 已知）");
    }
    std::vector<DownloadJob> jobs;
    jobs.reserve(pending.requests.size());
    for (const DownloadRequest& request : pending.requests) {
        jobs.push_back(DownloadJob{&request, InstallStage::kAssets});
    }
    return run_adaptive_downloads(m_scheduler, jobs, reporter_ptr, stop);
}

util::Result<void> GameService::fetch_version_json(const std::string& url,
                                                   const std::string& version_id,
                                                   std::stop_token stop) {
    DownloadRequest request;
    request.url = url;
    request.destination = m_layout.versions_dir / version_id / (version_id + ".json");
    request.display_name = version_id + ".json";
    return m_scheduler.download_file(request, nullptr, stop);
}

util::Result<void> GameService::install_parallel_impl(const VersionJson& version,
                                                      std::stop_token stop,
                                                      ProgressReporter* reporter,
                                                      bool reset_totals,
                                                      const std::string& phase_label) {
    // 进入下载阶段：清理上一阶段（处理器/Java 检测）的步骤条，避免 UI 残留循环动画。
    if (reporter != nullptr) {
        reporter->clear_step();
    }

    // ① 资产索引预取：资产计划依赖索引文件（很小），先单独下完，再进入并行阶段。
    if (version.has_asset_index && !version.asset_index.url.empty()) {
        const std::filesystem::path index_path =
            m_layout.assets_dir / "indexes" / (version.asset_index.id + ".json");
        std::error_code index_ec;
        if (!std::filesystem::is_regular_file(index_path, index_ec)) {
            DownloadRequest request;
            request.url = version.asset_index.url;
            request.destination = index_path;
            request.expected_sha1 = version.asset_index.sha1;
            request.display_name = "asset index " + version.asset_index.id;
            if (reporter != nullptr) {
                reporter->set_phase("准备资源索引");
            }
            util::Result<void> fetched = m_scheduler.download_file(request, nullptr, stop);
            if (fetched.is_error()) {
                return fetched;
            }
        }
    }

    // ② 两阶段计划与固定总量（之后不再变化 → 进度不倒退）。
    //    加载器等"同一任务内追加阶段"用 add_stage_totals，避免总进度从 100% 掉回去。
    const PendingDownloads version_pending = make_pending(plan_version_install(version));
    PendingDownloads asset_pending;
    if (version.has_asset_index) {
        util::Result<std::vector<DownloadRequest>> planned = plan_asset_install(version);
        if (planned.is_error()) {
            return util::Result<void>::fail(std::move(planned.error()));
        }
        asset_pending = make_pending(planned.value());
    }
    if (reporter != nullptr) {
        if (reset_totals) {
            // 新任务：一次性设定两阶段总量，之后不再变化（总进度单调）。
            reporter->set_stage_totals(InstallStage::kVersion, version_pending.files,
                                       version_pending.bytes, version_pending.unknown_files);
            reporter->set_stage_totals(InstallStage::kAssets, asset_pending.files,
                                       asset_pending.bytes, asset_pending.unknown_files);
        } else {
            // 加载器等"同一任务内追加阶段"：追加总量，不清零已完成数。
            reporter->add_stage_totals(InstallStage::kVersion, version_pending.files,
                                       version_pending.bytes, version_pending.unknown_files);
            reporter->add_stage_totals(InstallStage::kAssets, asset_pending.files,
                                       asset_pending.bytes, asset_pending.unknown_files);
        }
    }
    if (reporter != nullptr) {
        // 子状态：告诉用户"还剩多少文件"，而不是只看到百分比
        reporter->set_phase(phase_label);
        reporter->set_detail("版本文件 " + std::to_string(version_pending.files) + " 个 / 资源 " +
                             std::to_string(asset_pending.files) + " 个待下载");
    }

    // ③ 版本文件 + 资源文件进入同一个自适应下载池（批次 42，D-061）：
    //    文件级并发 4..32 自适应；任一文件失败即取消其余下载。
    std::vector<DownloadJob> jobs;
    jobs.reserve(version_pending.requests.size() + asset_pending.requests.size());
    for (const DownloadRequest& request : version_pending.requests) {
        jobs.push_back(DownloadJob{&request, InstallStage::kVersion});
    }
    for (const DownloadRequest& request : asset_pending.requests) {
        jobs.push_back(DownloadJob{&request, InstallStage::kAssets});
    }
    util::Result<void> downloaded = run_adaptive_downloads(m_scheduler, jobs, reporter, stop);
    if (downloaded.is_error()) {
        return downloaded;
    }

    // ④ natives 解压（版本文件已全部落盘；此前的"资源并行 + 版本线程解压"拆分
    //    在统一池下不再需要，解压放到下载全部完成后，失败清理语义保持不变）。
    const std::filesystem::path version_dir = m_layout.versions_dir / version.id;
    return extract_natives(version.libraries, m_layout.libraries_dir, m_os.name, true,
                           version_dir / (version.id + "-natives"));
}

util::Result<VersionJson> GameService::install_full_impl(const std::string& version_json_url,
                                                         const std::string& version_id,
                                                         std::stop_token stop,
                                                         ProgressReporter* reporter) {
    if (reporter != nullptr) {
        reporter->set_phase("获取版本信息");
        reporter->set_detail("正在下载 " + version_id + " 的版本清单…");
    }
    util::Result<void> fetched = fetch_version_json(version_json_url, version_id, stop);
    if (fetched.is_error()) {
        return util::Result<VersionJson>::fail(std::move(fetched.error()));
    }
    if (reporter != nullptr) {
        reporter->set_detail("正在解析版本继承链…");
    }
    util::Result<VersionJson> loaded = load_version(version_id);
    if (loaded.is_error()) {
        return util::Result<VersionJson>::fail(std::move(loaded.error()));
    }
    VersionJson version = std::move(loaded.value());
    util::Result<void> installed =
        install_parallel_impl(version, stop, reporter, /*reset_totals=*/true, "下载原版文件");
    if (installed.is_error()) {
        return util::Result<VersionJson>::fail(std::move(installed.error()));
    }
    ensure_instance_decision(version);
    return util::Result<VersionJson>::ok(std::move(version));
}

util::Result<VersionJson> GameService::install_full(const std::string& version_json_url,
                                                    const std::string& version_id,
                                                    std::stop_token stop,
                                                    const InstallProgressFn& progress) {
    // 跨阶段共享同一进度聚合器（字节累计/完成数不因阶段切换回零）。
    ProgressReporter reporter(progress);
    ProgressReporter* shared_reporter = progress ? &reporter : nullptr;

    // D-028 版本管理：全新安装若未完成（失败/主动取消），删除 versions/<id>/ 整目录
    // （已添加的版本 json 配置 + 半成品文件），避免被认定为可启动版本。
    // 已有安装（修复/更新失败）保持原状：下载均为原子落盘+哈希校验，旧安装未被破坏。
    const std::filesystem::path version_dir = m_layout.versions_dir / version_id;
    const std::filesystem::path json_path = version_dir / (version_id + ".json");
    std::error_code exists_ec;
    const bool fresh_install = !std::filesystem::is_regular_file(json_path, exists_ec);

    auto fail_clean = [&](util::Error error) -> util::Result<VersionJson> {
        if (fresh_install) {
            std::error_code remove_ec;
            std::filesystem::remove_all(version_dir, remove_ec);
            util::log_warn("[install] " + version_id +
                           " incomplete (" + error.code + "); removed " +
                               util::path_to_utf8(version_dir));
        }
        return util::Result<VersionJson>::fail(std::move(error));
    };

    util::Result<VersionJson> result =
        install_full_impl(version_json_url, version_id, stop, shared_reporter);
    if (result.is_error()) {
        return fail_clean(std::move(result.error()));
    }
    // D-117：下载/解压全部结束才允许总进度到 100%。
    if (shared_reporter != nullptr) {
        shared_reporter->complete();
    }
    return result;
}

util::Result<VersionJson> GameService::install_with_loader(const InstallJob& job,
                                                           std::stop_token stop,
                                                           const InstallProgressFn& progress) {
    ProgressReporter reporter(progress);
    ProgressReporter* shared_reporter = progress ? &reporter : nullptr;
    const bool has_loader = job.loader.kind != LoaderKind::kNone;
    if (shared_reporter != nullptr) {
        // D-117：带加载器时给后续安装器/处理器预留 50% 的进度区间，
        // 否则原版下载一结束总进度就是 100%，加载器阶段只能"停在 100%"或倒退。
        shared_reporter->set_progress_segment(0.0, has_loader ? kVanillaSegmentEnd : 1.0);
    }

    // ① 原版：未安装则安装；已安装时哈希校验跳过（失败按 D-028 清理全新安装）。
    const std::filesystem::path vanilla_dir = m_layout.versions_dir / job.minecraft.id;
    const std::filesystem::path vanilla_json =
        vanilla_dir / (job.minecraft.id + ".json");
    std::error_code vanilla_ec;
    const bool vanilla_fresh = !std::filesystem::is_regular_file(vanilla_json, vanilla_ec);
    util::Result<VersionJson> vanilla =
        install_full_impl(job.minecraft.url, job.minecraft.id, stop, shared_reporter);
    if (vanilla.is_error()) {
        if (vanilla_fresh) {
            std::error_code remove_ec;
            std::filesystem::remove_all(vanilla_dir, remove_ec);
            util::log_warn("[install] " + job.minecraft.id + " incomplete (" +
                           vanilla.error().code + "); removed " +
                               util::path_to_utf8(vanilla_dir));
        }
        return vanilla;
    }
    if (job.loader.kind == LoaderKind::kNone) {
        if (shared_reporter != nullptr) {
            shared_reporter->complete();
        }
        return vanilla;
    }
    if (shared_reporter != nullptr) {
        // 原版就绪：后续所有下载/处理器都落在 [50%, 100%] 区间。
        shared_reporter->set_progress_segment(kVanillaSegmentEnd, 1.0);
    }
    if (job.loader.entry.download_url.empty()) {
        return util::Result<VersionJson>::fail(util::Error::make(
            "loader.no_compatible_version", "加载器版本缺少下载地址",
            loader_kind_display(job.loader.kind)));
    }

    const std::string version_id =
        make_loader_version_id(job.loader.kind, job.minecraft.id, job.loader.entry);
    // D-051 防御：清单条目自带 MC 时必须与所选游戏版本一致，避免把别的 MC 的加载器装进来
    if (!job.loader.entry.minecraft_version.empty() &&
        job.loader.entry.minecraft_version != job.minecraft.id) {
        return util::Result<VersionJson>::fail(util::Error::make(
            "loader.version_mismatch", "所选加载器版本属于其它游戏版本",
            job.loader.entry.minecraft_version + " != " + job.minecraft.id));
    }
    if (job.loader.kind == LoaderKind::kForge || job.loader.kind == LoaderKind::kNeoForge) {
        util::Result<VersionJson> result =
            install_forge_like(job, vanilla.value(), version_id, stop, shared_reporter);
        if (result.is_ok() && shared_reporter != nullptr) {
            shared_reporter->complete();
        }
        return result;
    }
    if (job.loader.kind != LoaderKind::kFabric) {
        return util::Result<VersionJson>::fail(util::Error::make(
            "loader.installer_unsupported", "该加载器暂不支持安装",
            loader_kind_display(job.loader.kind)));
    }
    const std::filesystem::path version_dir = m_layout.versions_dir / version_id;
    const std::filesystem::path json_path = version_dir / (version_id + ".json");
    std::error_code exists_ec;
    const bool fresh_install = !std::filesystem::is_regular_file(json_path, exists_ec);
    const auto fail_clean = [&](util::Error error) -> util::Result<VersionJson> {
        if (fresh_install) {
            std::error_code remove_ec;
            std::filesystem::remove_all(version_dir, remove_ec);
            util::log_warn("[install] loader " + version_id + " incomplete (" + error.code +
                           "); removed " + util::path_to_utf8(version_dir));
        }
        return util::Result<VersionJson>::fail(std::move(error));
    };

    // ② 下载加载器配置（Fabric profile JSON）到临时文件
    const std::filesystem::path temp_dir = platform::data_dir() / L"cache" / L"tmp";
    util::Result<void> temp_created = ::sscl::platform::create_directories(temp_dir);
    if (temp_created.is_error()) {
        return fail_clean(temp_created.error());
    }
    const std::filesystem::path temp_file = temp_dir / (version_id + ".loader.json");
    DownloadRequest request;
    request.url = job.loader.entry.download_url;
    request.destination = temp_file;
    request.display_name = std::string(loader_kind_display(job.loader.kind)) + " " +
                           job.loader.entry.version;
    if (shared_reporter != nullptr) {
        shared_reporter->set_phase("下载加载器配置");
        shared_reporter->set_detail(std::string(loader_kind_display(job.loader.kind)) + " " +
                                    job.loader.entry.version + "：下载版本配置…");
    }
    util::Result<void> downloaded = m_scheduler.download_file(request, nullptr, stop);
    if (downloaded.is_error()) {
        std::error_code remove_ec;
        std::filesystem::remove(temp_file, remove_ec);
        return fail_clean(downloaded.error());
    }
    if (shared_reporter != nullptr) {
        shared_reporter->set_detail("正在合并加载器版本配置…");
    }

    // ③ 补丁（id/inheritsFrom/sscl 元数据）并原子写入版本 JSON
    util::Result<std::string> text = platform::read_file(temp_file);
    {
        std::error_code remove_ec;
        std::filesystem::remove(temp_file, remove_ec);
    }
    if (text.is_error()) {
        return fail_clean(text.error());
    }
    util::Result<std::string> patched =
        patch_loader_profile(text.value(), job.loader.kind, version_id, job.minecraft.id,
                             job.loader.entry);
    if (patched.is_error()) {
        return fail_clean(patched.error());
    }
    util::Result<void> dir_created = ::sscl::platform::create_directories(version_dir);
    if (dir_created.is_error()) {
        return fail_clean(dir_created.error());
    }
    util::Result<void> written = platform::write_file_atomic(json_path, patched.value());
    if (written.is_error()) {
        return fail_clean(written.error());
    }

    // ④ 加载合并后的版本（inheritsFrom 原版）→ 补全加载器库与资产（原版部分已就绪，自动跳过）
    util::Result<VersionJson> loaded = load_version(version_id);
    if (loaded.is_error()) {
        return fail_clean(loaded.error());
    }
    VersionJson version = std::move(loaded.value());
    // 同一任务内追加阶段：不清零已完成的原版进度（总进度不回退）。
    util::Result<void> installed = install_parallel_impl(
        version, stop, shared_reporter, /*reset_totals=*/false, "补全加载器文件");
    if (installed.is_error()) {
        return fail_clean(std::move(installed.error()));
    }
    util::log_info("[install] loader " + std::string(loader_kind_display(job.loader.kind)) + " " +
                   version_id + " ready");
    ensure_instance_decision(version);
    if (shared_reporter != nullptr) {
        shared_reporter->complete();  // D-117：Fabric 分支收尾
    }
    return util::Result<VersionJson>::ok(std::move(version));
}

util::Result<VersionJson> GameService::install_forge_like(const InstallJob& job,
                                                          const VersionJson& vanilla,
                                                          const std::string& version_id,
                                                          std::stop_token stop,
                                                          ProgressReporter* reporter) {
    const std::filesystem::path version_dir = m_layout.versions_dir / version_id;
    const std::filesystem::path json_path = version_dir / (version_id + ".json");
    std::error_code exists_ec;
    const bool fresh_install = !std::filesystem::is_regular_file(json_path, exists_ec);
    const auto fail_clean = [&](util::Error error) -> util::Result<VersionJson> {
        if (fresh_install) {
            std::error_code remove_ec;
            std::filesystem::remove_all(version_dir, remove_ec);
            util::log_warn("[install] loader " + version_id + " incomplete (" + error.code +
                           "); removed " + util::path_to_utf8(version_dir));
        }
        return util::Result<VersionJson>::fail(std::move(error));
    };

    // ① 下载安装器 jar 到临时目录
    const std::filesystem::path temp_dir = platform::data_dir() / L"cache" / L"tmp";
    util::Result<void> temp_created = ::sscl::platform::create_directories(temp_dir);
    if (temp_created.is_error()) {
        return fail_clean(temp_created.error());
    }
    const std::filesystem::path installer = temp_dir / (version_id + "-installer.jar");
    const std::filesystem::path data_temp = temp_dir / (version_id + "-data");

    DownloadRequest installer_request;
    installer_request.url = job.loader.entry.download_url;
    installer_request.destination = installer;
    installer_request.display_name =
        std::string(loader_kind_display(job.loader.kind)) + " 安装器 " +
        job.loader.entry.version;
    std::uint64_t installer_id = 0;
    if (reporter != nullptr) {
        // D-117 子区间：安装器 jar 占用 50%~62%，下完再进入库下载区间。
        reporter->set_progress_segment(kVanillaSegmentEnd, kInstallerSegmentEnd);
        reporter->set_phase("下载安装器");
        reporter->set_detail(std::string("正在下载 ") +
                             loader_kind_display(job.loader.kind) + " 安装器 " +
                             job.loader.entry.version);
        installer_id = reporter->begin_file(installer_request.display_name, 0);
    }
    std::uint64_t installer_last = 0;
    // D-117：安装器 jar 动辄十几 MB，旧实现按 count_bytes=false 处理 —— 整个下载期间
    // 总进度与速度都不动，看起来像卡死。首次拿到总大小后把它登记进版本阶段总量，
    // 之后按真实字节计量。
    std::uint64_t installer_total = 0;
    bool installer_counted = false;
    const DownloadProgressFn installer_progress =
        (reporter != nullptr)
            ? DownloadProgressFn([reporter, installer_id, &installer_last, &installer_total,
                                  &installer_counted](std::uint64_t current,
                                                      std::uint64_t total) {
                  if (total > 0 && !installer_counted) {
                      installer_counted = true;
                      installer_total = total;
                      reporter->add_stage_totals(InstallStage::kVersion, 1, total, 0);
                  }
                  const std::uint64_t delta =
                      current > installer_last ? current - installer_last : 0;
                  installer_last = current;
                  reporter->progress(InstallStage::kVersion, installer_id, delta, current, total,
                                     installer_counted);
              })
            : nullptr;
    util::Result<void> installer_downloaded =
        m_scheduler.download_file(installer_request, installer_progress, stop);
    if (reporter != nullptr) {
        if (installer_downloaded.is_ok()) {
            if (!installer_counted) {
                // 服务器没给 Content-Length：退化为"大小未知"文件，至少让完成数前进。
                reporter->add_stage_totals(InstallStage::kVersion, 1, 0, 1);
            }
            const std::uint64_t remaining =
                installer_counted && installer_total > installer_last ? installer_total - installer_last
                                                                      : 0;
            reporter->end_file(InstallStage::kVersion, installer_id, remaining,
                               installer_counted);
        } else {
            reporter->drop_file(installer_id,
                                installer_downloaded.error().code != "download.cancelled");
        }
    }
    if (installer_downloaded.is_error()) {
        std::error_code remove_ec;
        std::filesystem::remove(installer, remove_ec);
        return fail_clean(installer_downloaded.error());
    }
    if (reporter != nullptr) {
        reporter->set_detail("正在读取安装器配置…");
    }

    // ② 解析 install_profile.json（结构判定，§5.4）
    util::Result<std::string> profile_text =
        platform::zip_read_entry(installer, "install_profile.json");
    if (profile_text.is_error()) {
        std::error_code remove_ec;
        std::filesystem::remove(installer, remove_ec);
        return fail_clean(util::Error::make("loader.installer_invalid",
                                            "安装器缺少 install_profile.json",
                                            job.loader.entry.download_url));
    }
    util::Result<InstallerProfile> parsed = parse_installer_profile(profile_text.value());
    if (parsed.is_error()) {
        std::error_code remove_ec;
        std::filesystem::remove(installer, remove_ec);
        return fail_clean(parsed.error());
    }
    const InstallerProfile profile = std::move(parsed.value());
    if (profile.minecraft != job.minecraft.id) {
        std::error_code remove_ec;
        std::filesystem::remove(installer, remove_ec);
        return fail_clean(util::Error::make(
            "loader.version_mismatch", "安装器目标版本与所选游戏版本不一致",
            profile.minecraft + " != " + job.minecraft.id));
    }

    // ③ 取出内嵌版本 JSON 并补丁落盘
    std::string version_json_text;
    if (profile.flavor == InstallerFlavor::kLegacy) {
        version_json_text = profile.embedded_version_json;
    } else {
        util::Result<std::string> embedded =
            platform::zip_read_entry(installer, profile.json_entry);
        if (embedded.is_error()) {
            std::error_code remove_ec;
            std::filesystem::remove(installer, remove_ec);
            return fail_clean(util::Error::make(
                "loader.installer_invalid", "安装器缺少版本配置：" + profile.json_entry, {}));
        }
        version_json_text = std::move(embedded.value());
    }

    util::Result<std::string> patched = patch_loader_profile(
        version_json_text, job.loader.kind, version_id, job.minecraft.id, job.loader.entry);
    if (patched.is_error()) {
        // 兜底：极旧 versionInfo 可能缺少 mainClass/libraries，只补必需字段
        nlohmann::json root = nlohmann::json::parse(version_json_text, nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
            std::error_code remove_ec;
            std::filesystem::remove(installer, remove_ec);
            return fail_clean(patched.error());
        }
        root["id"] = version_id;
        if (!root.contains("inheritsFrom") || !root["inheritsFrom"].is_string() ||
            root["inheritsFrom"].get<std::string>().empty()) {
            root["inheritsFrom"] = job.minecraft.id;
        }
        nlohmann::json metadata;
        metadata["loader"] = loader_kind_key(job.loader.kind);
        metadata["loaderVersion"] = job.loader.entry.version;
        metadata["minecraftVersion"] = job.minecraft.id;
        if (!job.loader.entry.download_url.empty()) {
            metadata["installerUrl"] = job.loader.entry.download_url;
        }
        root["sscl"] = std::move(metadata);
        patched = util::Result<std::string>::ok(root.dump(2));
    }

    util::Result<void> dir_created = ::sscl::platform::create_directories(version_dir);
    if (dir_created.is_error()) {
        return fail_clean(dir_created.error());
    }
    util::Result<void> written = platform::write_file_atomic(json_path, patched.value());
    if (written.is_error()) {
        return fail_clean(written.error());
    }

    // ④ 旧版：从安装器取出 universal jar，按 install.path 的 Maven 坐标落盘
    if (profile.flavor == InstallerFlavor::kLegacy) {
        util::Result<std::string> universal =
            platform::zip_read_entry(installer, profile.legacy_file_path);
        if (universal.is_error()) {
            const std::string fallback = util::path_to_utf8(
                util::path_from_utf8(profile.legacy_file_path).filename());
            universal = platform::zip_read_entry(installer, fallback);
        }
        if (universal.is_error()) {
            return fail_clean(util::Error::make("loader.installer_invalid",
                                                "安装器缺少 universal jar：" +
                                                    profile.legacy_file_path,
                                                {}));
        }
        const std::filesystem::path dest =
            m_layout.libraries_dir / library_relative_path(profile.legacy_install_path);
        if (profile.legacy_install_path.empty()) {
            return fail_clean(util::Error::make(
                "loader.installer_invalid", "旧版安装器缺少 install.path", version_id));
        }
        std::error_code create_ec;
        std::filesystem::create_directories(dest.parent_path(), create_ec);
        util::Result<void> wrote = platform::write_file(dest, universal.value());
        if (wrote.is_error()) {
            return fail_clean(wrote.error());
        }
    }

    // ⑤ 安装器内 maven/ 整目录复制到 libraries/（处理器 classpath 依赖其中 jar；
    //    中古分支仅靠这一步，现代/旧版也需要，缺了 classpath 会找不到 installertools 等）
    {
        util::Result<std::size_t> extracted =
            platform::zip_extract_subtree(installer, "maven/", m_layout.libraries_dir);
        if (extracted.is_error()) {
            return fail_clean(extracted.error());
        }
    }

    // ⑥ 安装器自带库（本地生成条目 local_only 已在计划阶段跳过）
    std::vector<DownloadRequest> requests;
    for (const Library& library : profile.libraries) {
        if (library.local_only) {
            continue;
        }
        const std::string relative = library_relative_file_path(library);
        if (relative.empty()) {
            continue;
        }
        const std::string url = library_download_url(library);
        if (url.empty()) {
            continue;
        }
        DownloadRequest request;
        request.url = url;
        request.destination = m_layout.libraries_dir / relative;
        request.display_name = library.name;
        if (library.has_artifact) {
            request.expected_sha1 = library.artifact.sha1;
            request.expected_size = library.artifact.size;
        }
        requests.push_back(std::move(request));
    }
    {
        const PendingDownloads pending = make_pending(requests);
        if (reporter != nullptr) {
            // D-117 子区间：安装器自带库 62%~80%。
            reporter->set_progress_segment(kInstallerSegmentEnd, kInstallerLibsSegmentEnd);
            reporter->add_stage_totals(InstallStage::kVersion, pending.files, pending.bytes,
                                       pending.unknown_files);
        }
        util::Result<void> libraries = download_sequential(
            pending, InstallStage::kVersion, "安装器依赖", stop, reporter, /*manage_totals=*/false);
        if (libraries.is_error()) {
            return fail_clean(std::move(libraries.error()));
        }
    }

    // ⑦ 现代：执行 processors（输出就绪跳过；MOJMAPS 自行下载）
    const bool runs_processors =
        profile.flavor == InstallerFlavor::kModern && !profile.processors.empty();
    if (runs_processors) {
        std::map<std::string, std::string> variables;
        for (const auto& [key, sided] : profile.data) {
            util::Result<std::string> resolved = resolve_installer_data_value(
                sided.client, installer, m_layout.libraries_dir, data_temp, stop);
            if (resolved.is_error()) {
                return fail_clean(resolved.error());
            }
            variables[key] = std::move(resolved.value());
        }
        variables["ROOT"] = util::path_to_utf8(m_layout.game_dir);
        variables["MINECRAFT_VERSION"] = job.minecraft.id;
        variables["MINECRAFT_JAR"] = util::path_to_utf8(
            m_layout.versions_dir / job.minecraft.id / (job.minecraft.id + ".jar"));
        variables["INSTALLER"] = util::path_to_utf8(installer);
        variables["LIBRARY_DIR"] = util::path_to_utf8(m_layout.libraries_dir);
        variables["SIDE"] = "client";

        JavaVersionInfo required;
        if (vanilla.has_java_version) {
            required = vanilla.java_version;
        }
        if (required.major_version <= 0) {
            required.major_version = 17;  // 现代 Forge/NeoForge 最低 17
        }
        if (reporter != nullptr) {
            // D-117 子区间：处理器与 Java 检测占 80%~95%，由步骤进度驱动。
            reporter->set_progress_segment(kInstallerLibsSegmentEnd, kProcessorSegmentEnd);
            reporter->set_phase("检查 Java 环境");
            reporter->set_detail("正在检测 Java " + std::to_string(required.major_version) +
                                 " 运行时（可能需要几秒）…");
            // Java 检测耗时不可预测：总量未知 → 页面画循环动画，避免看起来卡死。
            reporter->set_step(0, 0, true);
        }
        util::Result<JavaSelection> selection = auto_select_java_runtime(required);
        if (selection.is_error()) {
            if (reporter != nullptr) {
                reporter->clear_step();
            }
            return fail_clean(util::Error::make(
                "loader.java_required",
                "安装该加载器需要 Java " + std::to_string(required.major_version) + "，且未找到可用运行时",
                selection.error().message));
        }

        if (reporter != nullptr) {
            reporter->set_phase("运行安装器处理器");
            reporter->set_detail("Java " +
                                 std::to_string(selection.value().runtime.version.major_version) +
                                 "：" + util::path_to_utf8(
                                           selection.value().runtime.executable.filename()));
        }
        ProcessorEnvironment environment;
        environment.game_dir = m_layout.game_dir;
        environment.libraries_dir = m_layout.libraries_dir;
        environment.installer_path = installer;
        environment.minecraft_jar =
            m_layout.versions_dir / job.minecraft.id / (job.minecraft.id + ".jar");
        environment.minecraft_version = job.minecraft.id;
        environment.java_executable =
            util::path_to_utf8(selection.value().runtime.executable);
        environment.variables = std::move(variables);

        const VersionJson vanilla_snapshot = vanilla;
        util::Result<void> processed = run_processors(
            profile.processors, environment, stop,
            [](const std::string& line) { util::log_info("[loader] " + line); },
            [this, &vanilla_snapshot, stop, reporter](const std::filesystem::path& output,
                                                      const std::string& /*ignored_sha1*/)
                -> util::Result<void> {
                if (!vanilla_snapshot.has_downloads ||
                    !vanilla_snapshot.downloads.has_client_mappings ||
                    vanilla_snapshot.downloads.client_mappings.url.empty() ||
                    vanilla_snapshot.downloads.client_mappings.sha1.empty()) {
                    return util::Result<void>::fail(util::Error::make(
                        "loader.mojmaps_missing", "缺少 Mojang 官方映射（client_mappings）信息",
                        {}));
                }
                // 以版本 JSON 的 client_mappings 哈希为准（安装器里的 {MOJMAPSHA1} 不可靠，
                // 实测 1.21.11 与对象哈希不一致；HMCL/Axolotl 同款处理）。
                DownloadRequest request;
                request.url = vanilla_snapshot.downloads.client_mappings.url;
                request.destination = output;
                request.expected_sha1 = vanilla_snapshot.downloads.client_mappings.sha1;
                request.expected_size = vanilla_snapshot.downloads.client_mappings.size;
                request.display_name = "client mappings";
                // D-117：映射文件通常十几 MB，且发生在处理器阶段 —— 不计入总量的话
                // 这段时间总进度与速度完全不动；这里按普通文件计量。
                const bool known = request.expected_size > 0;
                const std::uint64_t expected =
                    known ? static_cast<std::uint64_t>(request.expected_size) : 0;
                std::uint64_t file_id = 0;
                std::uint64_t last = 0;
                DownloadProgressFn progress_fn;
                if (reporter != nullptr) {
                    reporter->add_stage_totals(InstallStage::kVersion, 1, expected,
                                               known ? 0 : 1);
                    file_id = reporter->begin_file(request.display_name, expected);
                    progress_fn = [reporter, file_id, known, &last](std::uint64_t current,
                                                                    std::uint64_t total) {
                        const std::uint64_t delta = current > last ? current - last : 0;
                        last = current;
                        reporter->progress(InstallStage::kVersion, file_id, delta, current, total,
                                           known);
                    };
                }
                util::Result<void> fetched =
                    m_scheduler.download_file(request, progress_fn, stop);
                if (reporter != nullptr) {
                    if (fetched.is_ok()) {
                        const std::uint64_t remaining =
                            known && expected > last ? expected - last : 0;
                        reporter->end_file(InstallStage::kVersion, file_id, remaining, known);
                    } else {
                        reporter->drop_file(file_id,
                                            fetched.error().code != "download.cancelled");
                    }
                }
                return fetched;
            },
            [reporter](std::size_t index, std::size_t total, const std::string& label) {
                if (reporter == nullptr) {
                    return;
                }
                // 粗进度：已完成 (index-1)/total；当前处理器内部无法取百分比 →
                // indeterminate=true，页面画循环动画直到此处理器结束/下一个回调。
                const std::uint64_t done =
                    index > 0 ? static_cast<std::uint64_t>(index - 1) : 0;
                reporter->set_step(done, static_cast<std::uint64_t>(total), true);
                reporter->set_phase("运行安装器处理器");
                reporter->set_detail("处理器 " + std::to_string(index) + "/" +
                                     std::to_string(total) + "：" + label);
            });
        if (reporter != nullptr) {
            reporter->clear_step();
        }
        if (processed.is_error()) {
            return fail_clean(processed.error());
        }
    }

    // ⑧ 合并父版本并补全剩余库/资产（处理器产物为 local_only，自动跳过下载）
    util::Result<VersionJson> loaded = load_version(version_id);
    if (loaded.is_error()) {
        return fail_clean(loaded.error());
    }
    VersionJson version = std::move(loaded.value());
    if (reporter != nullptr) {
        // D-117 子区间：最后补全 95%~100%（旧版安装器没有处理器步骤，直接从 80% 起）。
        reporter->set_progress_segment(
            runs_processors ? kProcessorSegmentEnd : kInstallerLibsSegmentEnd, 1.0);
    }
    util::Result<void> installed = install_parallel_impl(
        version, stop, reporter, /*reset_totals=*/false, "补全加载器文件");
    if (installed.is_error()) {
        return fail_clean(std::move(installed.error()));
    }

    {
        std::error_code remove_ec;
        std::filesystem::remove(installer, remove_ec);
        std::filesystem::remove_all(data_temp, remove_ec);
    }
    util::log_info("[install] loader " + std::string(loader_kind_display(job.loader.kind)) + " " +
                   version_id + " ready");
    ensure_instance_decision(version);
    return util::Result<VersionJson>::ok(std::move(version));
}

void GameService::ensure_instance_decision(const VersionJson& merged) {
    util::Result<InstanceTarget> target =
        resolve_instance_target(m_layout, m_isolation_mode, merged, /*persist_decision=*/true);
    if (target.is_error()) {
        util::log_warn("[instance] " + merged.id + " 隔离决策失败：" + target.error().detail);
        return;
    }
    util::log_info("[instance] " + merged.id +
                   (target.value().isolated ? " isolated" : " shared") + " mods=" +
                   util::path_to_utf8(target.value().mods_dir));
}

util::Result<InstanceTarget> GameService::resolve_target(const std::string& version_id) {
    util::Result<VersionJson> loaded = load_version(version_id);
    if (loaded.is_error()) {
        return util::Result<InstanceTarget>::fail(std::move(loaded.error()));
    }
    return resolve_instance_target(m_layout, m_isolation_mode, loaded.value(),
                                   /*persist_decision=*/true);
}

std::vector<VersionSummary> GameService::version_summaries() {
    std::vector<VersionSummary> result;
    for (const InstalledVersion& installed : installed_versions()) {
        VersionSummary summary;
        summary.version = installed;
        util::Result<VersionJson> loaded = load_version(installed.id);
        if (loaded.is_error()) {
            summary.state = InstanceState::kError;
            summary.error = loaded.error().message;
        } else {
            const VersionJson& merged = loaded.value();
            const PclVersionInfo pcl = detect_pcl_version(merged, installed.id);
            const std::string loader = detect_loader_key(merged);
            // D-116：加载器 id 不能当原版号（否则版本列表与 Mod 筛选都会用错值）。
            summary.minecraft_version =
                is_plausible_mc_version(pcl.vanilla_name)
                    ? pcl.vanilla_name
                    : detect_minecraft_version(merged, installed.id);
            summary.loader = loader;
            summary.loader_version = merged.sscl.loader_version;
            summary.release_time = pcl.release_time;
            summary.release = pcl.is_release;
            summary.drop = pcl.drop;
            summary.moddable = loader_is_moddable(loader);
            if (pcl.is_fool) {
                summary.state = InstanceState::kFool;
            } else if (pcl.is_snapshot) {
                summary.state = InstanceState::kSnapshot;
            } else if (loader == "forge") {
                summary.state = InstanceState::kForge;
            } else if (loader == "neoforge") {
                summary.state = InstanceState::kNeoForge;
            } else if (loader == "fabric") {
                summary.state = InstanceState::kFabric;
            } else if (loader == "quilt") {
                summary.state = InstanceState::kQuilt;
            } else if (pcl.vanilla_name == "Old") {
                summary.state = InstanceState::kOld;
            } else {
                summary.state = InstanceState::kOriginal;
            }
            util::Result<InstanceTarget> target = resolve_instance_target(
                m_layout, m_isolation_mode, merged, /*persist_decision=*/false);
            if (target.is_ok()) {
                summary.isolated = target.value().isolated;
            }
        }
        util::Result<InstanceSettings> settings =
            load_instance_settings(m_layout.versions_dir / installed.id);
        if (settings.is_ok()) {
            summary.starred = settings.value().starred;
            summary.category = static_cast<InstanceCategory>(settings.value().category);
            summary.custom_info = settings.value().custom_info;
            summary.logo = settings.value().logo;
        }
        result.push_back(std::move(summary));
    }
    return result;
}

std::vector<InstanceCard> GameService::instance_cards(bool show_hidden) {
    return classify_instance_summaries(version_summaries(), show_hidden);
}

util::Result<InstanceSettings> GameService::instance_settings(
    const std::string& version_id) const {
    if (version_id.empty()) {
        return util::Result<InstanceSettings>::fail(
            util::Error::make("instance.missing_id", "版本 id 为空", {}));
    }
    return load_instance_settings(m_layout.versions_dir / version_id);
}

util::Result<void> GameService::save_instance_settings_for(
    const std::string& version_id, const InstanceSettings& settings) {
    const std::filesystem::path version_dir = m_layout.versions_dir / version_id;
    std::error_code ec;
    if (!std::filesystem::is_directory(version_dir, ec)) {
        return util::Result<void>::fail(
            util::Error::make("instance.not_found", "版本不存在：" + version_id, {}));
    }
    return save_instance_settings(version_dir, settings);
}

util::Result<void> GameService::set_instance_star(const std::string& version_id, bool starred) {
    util::Result<InstanceSettings> loaded = instance_settings(version_id);
    if (loaded.is_error()) {
        return util::Result<void>::fail(std::move(loaded.error()));
    }
    InstanceSettings settings = loaded.value();
    settings.starred = starred;
    return save_instance_settings_for(version_id, settings);
}

util::Result<void> GameService::set_instance_category(const std::string& version_id,
                                                      InstanceCategory category) {
    const int value = static_cast<int>(category);
    if (value < 0 || value > 6) {
        return util::Result<void>::fail(util::Error::make(
            "instance.invalid_category", "无效的版本分类", std::to_string(value)));
    }
    util::Result<InstanceSettings> loaded = instance_settings(version_id);
    if (loaded.is_error()) {
        return util::Result<void>::fail(std::move(loaded.error()));
    }
    InstanceSettings settings = loaded.value();
    settings.category = value;
    return save_instance_settings_for(version_id, settings);
}

util::Result<void> GameService::set_instance_description(const std::string& version_id,
                                                         const std::string& description) {
    util::Result<InstanceSettings> loaded = instance_settings(version_id);
    if (loaded.is_error()) {
        return util::Result<void>::fail(std::move(loaded.error()));
    }
    InstanceSettings settings = loaded.value();
    settings.custom_info = description;
    return save_instance_settings_for(version_id, settings);
}

util::Result<void> GameService::set_instance_logo(const std::string& version_id,
                                                  const std::string& logo_path) {
    util::Result<InstanceSettings> loaded = instance_settings(version_id);
    if (loaded.is_error()) {
        return util::Result<void>::fail(std::move(loaded.error()));
    }
    InstanceSettings settings = loaded.value();
    settings.logo = logo_path;
    return save_instance_settings_for(version_id, settings);
}

util::Result<void> GameService::rename_instance(const std::string& version_id,
                                                const std::string& new_name) {
    std::string reason;
    if (!instance_name_is_valid(new_name, &reason)) {
        return util::Result<void>::fail(
            util::Error::make("instance.invalid_name", "版本名不合法：" + reason, new_name));
    }
    if (version_id == new_name) {
        return util::Result<void>::ok();
    }
    const std::filesystem::path old_dir = m_layout.versions_dir / version_id;
    const std::filesystem::path new_dir = m_layout.versions_dir / new_name;
    std::error_code ec;
    if (!std::filesystem::is_directory(old_dir, ec)) {
        return util::Result<void>::fail(
            util::Error::make("instance.not_found", "版本不存在：" + version_id, {}));
    }
    if (std::filesystem::exists(new_dir, ec)) {
        return util::Result<void>::fail(util::Error::make(
            "instance.name_exists", "目标版本名已存在：" + new_name, {}));
    }

    // V-7：阻止重命名会破坏子版本 inheritsFrom 依赖。
    std::vector<std::string> dependents;
    for (std::filesystem::directory_iterator it(m_layout.versions_dir, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) {
            continue;
        }
        const std::string child = util::wide_to_utf8(it->path().filename().wstring());
        if (child == version_id) {
            continue;
        }
        util::Result<std::string> text =
            platform::read_file(it->path() / (it->path().filename().wstring() + L".json"));
        if (text.is_error()) {
            continue;
        }
        const nlohmann::json parsed = nlohmann::json::parse(text.value(), nullptr, false);
        if (parsed.is_object() && parsed.contains("inheritsFrom") &&
            parsed["inheritsFrom"].is_string() &&
            parsed["inheritsFrom"].get<std::string>() == version_id) {
            dependents.push_back(child);
        }
    }
    if (!dependents.empty()) {
        std::string list;
        for (const std::string& item : dependents) {
            if (!list.empty()) {
                list += "、";
            }
            list += item;
        }
        return util::Result<void>::fail(util::Error::make(
            "instance.rename_dependency", "以下版本依赖该版本，不能重命名：", list));
    }

    // 定位版本 JSON（PCL：优先 <id>.json，否则任意含 mainClass/type/id 的 json）。
    std::filesystem::path old_json = old_dir / (version_id + ".json");
    if (!std::filesystem::is_regular_file(old_json, ec)) {
        old_json.clear();
        for (std::filesystem::directory_iterator it(old_dir, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (!it->is_regular_file(ec) || it->path().extension() != L".json") {
                continue;
            }
            util::Result<std::string> text = platform::read_file(it->path());
            if (text.is_error()) {
                continue;
            }
            const nlohmann::json parsed = nlohmann::json::parse(text.value(), nullptr, false);
            if (parsed.is_object() && parsed.contains("mainClass") &&
                parsed.contains("type") && parsed.contains("id")) {
                old_json = it->path();
                break;
            }
        }
    }
    if (old_json.empty()) {
        return util::Result<void>::fail(util::Error::make(
            "instance.json_missing", "未找到版本 JSON，无法重命名：" + version_id, {}));
    }
    util::Result<std::string> original_text = platform::read_file(old_json);
    if (original_text.is_error()) {
        return util::Result<void>::fail(util::Error::make(
            "instance.json_read_failed", "读取版本 JSON 失败", original_text.error().detail));
    }

    std::filesystem::rename(old_dir, new_dir, ec);
    if (ec) {
        return util::Result<void>::fail(util::Error::make(
            "instance.rename_failed", "重命名版本目录失败", ec.message()));
    }
    const std::filesystem::path moved_old_json = new_dir / old_json.filename();
    const std::filesystem::path new_json = new_dir / (new_name + ".json");
    auto rename_if_exists = [](const std::filesystem::path& from,
                               const std::filesystem::path& to) {
        std::error_code inner;
        if (std::filesystem::is_regular_file(from, inner) ||
            std::filesystem::is_directory(from, inner)) {
            std::filesystem::rename(from, to, inner);
        }
    };
    rename_if_exists(new_dir / (version_id + ".jar"), new_dir / (new_name + ".jar"));
    rename_if_exists(new_dir / (version_id + "-natives"),
                     new_dir / (new_name + "-natives"));

    nlohmann::json updated = nlohmann::json::parse(original_text.value(), nullptr, false);
    if (!updated.is_object()) {
        updated = nlohmann::json::object();
    }
    updated["id"] = new_name;
    util::Result<void> written = platform::write_file_atomic(new_json, updated.dump(2));
    if (written.is_error()) {
        // 回滚：写回旧 JSON、恢复目录与文件名。
        (void)platform::write_file_atomic(moved_old_json, original_text.value());
        rename_if_exists(new_dir / (new_name + ".jar"), new_dir / (version_id + ".jar"));
        rename_if_exists(new_dir / (new_name + "-natives"),
                         new_dir / (version_id + "-natives"));
        std::error_code rollback;
        std::filesystem::rename(new_dir, old_dir, rollback);
        return util::Result<void>::fail(util::Error::make(
            "instance.rename_failed", "写入新版本 JSON 失败", written.error().detail));
    }
    std::filesystem::remove(moved_old_json, ec);
    return util::Result<void>::ok();
}

util::Result<void> GameService::delete_instance(const std::string& version_id, bool permanent) {
    const std::filesystem::path version_dir = m_layout.versions_dir / version_id;
    std::error_code ec;
    if (!std::filesystem::is_directory(version_dir, ec)) {
        return util::Result<void>::fail(
            util::Error::make("instance.not_found", "版本不存在：" + version_id, {}));
    }

    // D-087：磁盘层 inheritsFrom 仍被启动链依赖（详见 installed_versions 注释）。
    // 直接删除被引用的父版本会让所有子版本永久无法启动，因此必须先拦截。
    const std::vector<std::string> dependents =
        find_dependent_versions(m_layout.versions_dir, version_id);
    if (!dependents.empty()) {
        return util::Result<void>::fail(util::Error::make(
            "instance.delete_dependency", "以下版本依赖该版本，不能删除：",
            join_names(dependents)));
    }

    if (permanent) {
        std::filesystem::remove_all(version_dir, ec);
        if (ec) {
            return util::Result<void>::fail(util::Error::make(
                "instance.delete_failed", "永久删除版本失败", ec.message()));
        }
        return util::Result<void>::ok();
    }
    if (!platform::recycle_file(version_dir)) {
        return util::Result<void>::fail(util::Error::make(
            "instance.delete_failed", "移动到回收站失败：" + version_id, {}));
    }
    return util::Result<void>::ok();
}

util::Result<InstanceTarget> GameService::set_instance_isolation(const std::string& version_id,
                                                                 bool isolated) {
    util::Result<VersionJson> loaded = load_version(version_id);
    if (loaded.is_error()) {
        return util::Result<InstanceTarget>::fail(std::move(loaded.error()));
    }
    const std::filesystem::path version_dir = m_layout.versions_dir / version_id;
    util::Result<InstanceSettings> existing = load_instance_settings(version_dir);
    InstanceSettings settings;
    if (existing.is_ok()) {
        settings = existing.value();
    }
    settings.isolated = isolated;
    settings.decided = true;
    util::Result<void> saved = save_instance_settings(version_dir, settings);
    if (saved.is_error()) {
        return util::Result<InstanceTarget>::fail(std::move(saved.error()));
    }
    return resolve_instance_target(m_layout, m_isolation_mode, loaded.value(),
                                   /*persist_decision=*/false);
}

int compare_version_ids(const std::string& left, const std::string& right) {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < left.size() || j < right.size()) {
        const bool left_digit = i < left.size() && std::isdigit(static_cast<unsigned char>(left[i]));
        const bool right_digit =
            j < right.size() && std::isdigit(static_cast<unsigned char>(right[j]));
        if (left_digit && right_digit) {
            const std::size_t i0 = i;
            const std::size_t j0 = j;
            while (i < left.size() && std::isdigit(static_cast<unsigned char>(left[i]))) {
                ++i;
            }
            while (j < right.size() && std::isdigit(static_cast<unsigned char>(right[j]))) {
                ++j;
            }
            std::size_t ia = i0;
            std::size_t ja = j0;
            while (ia < i - 1 && left[ia] == '0') {
                ++ia;
            }
            while (ja < j - 1 && right[ja] == '0') {
                ++ja;
            }
            const std::size_t la = i - ia;
            const std::size_t lb = j - ja;
            if (la != lb) {
                return la < lb ? -1 : 1;
            }
            const int digits = left.compare(ia, la, right, ja, lb);
            if (digits != 0) {
                return digits < 0 ? -1 : 1;
            }
            continue;
        }
        if (i >= left.size()) {
            return -1;
        }
        if (j >= right.size()) {
            return 1;
        }
        if (left_digit != right_digit) {
            // 数字段 > 字母段（24w14a 段与 -pre 段比较时保持稳定序）。
            return left_digit ? 1 : -1;
        }
        const unsigned char ca = static_cast<unsigned char>(left[i]);
        const unsigned char cb = static_cast<unsigned char>(right[j]);
        const unsigned char na = static_cast<unsigned char>(std::tolower(ca));
        const unsigned char nb = static_cast<unsigned char>(std::tolower(cb));
        if (na != nb) {
            return na < nb ? -1 : 1;
        }
        ++i;
        ++j;
    }
    return 0;
}

std::vector<InstalledVersion> GameService::installed_versions() const {
    // PCL 2.99 对齐（负责人 2026-11-06）：版本列表只显示用户真正下载的版本。
    // 安装带加载器的版本时，install_with_loader 会先落一份原版到 versions/<mc>/，
    // 再创建 inheritsFrom 指向它的加载器版本；那份原版是**安装流程的内部基础**，
    // 不应作为独立实例列出，也不应与加载器版本产生 UI 上的父子关联。
    // 磁盘层的 inheritsFrom 保留（启动器依赖它合并版本），仅在版本发现层解耦。
    struct ScannedVersion {
        std::filesystem::path dir;
        std::string id;
        std::string type = "release";
        std::string jar_name;
        std::string inherits_from;
        bool has_inherits = false;
    };

    std::vector<ScannedVersion> scanned;
    std::error_code ec;
    std::filesystem::directory_iterator it(m_layout.versions_dir, ec);
    if (ec) {
        return {};
    }
    for (const auto& entry : it) {
        std::error_code entry_ec;
        if (!entry.is_directory(entry_ec)) {
            continue;
        }
        const std::string id = util::path_to_utf8(entry.path().filename());
        if (id.empty()) {
            continue;
        }
        // PCL InitMcInstanceListWithoutCache 的特殊目录；SSCL 内部目录统一以 .sscl 开头。
        if (id == "cache" || id == "BLClient" || id == "PCL" || id.rfind(".sscl", 0) == 0) {
            continue;
        }
        const std::filesystem::path json_path = entry.path() / (id + ".json");
        if (!std::filesystem::is_regular_file(json_path, entry_ec)) {
            continue;
        }
        ScannedVersion item;
        item.dir = entry.path();
        item.id = id;
        item.jar_name = id;
        util::Result<std::string> text = platform::read_file(json_path);
        if (text.is_ok()) {
            try {
                const nlohmann::json parsed = nlohmann::json::parse(text.value(), nullptr, false);
                if (parsed.is_object()) {
                    if (parsed.contains("type") && parsed["type"].is_string()) {
                        const std::string type = parsed["type"].get<std::string>();
                        if (!type.empty()) {
                            item.type = type;
                        }
                    }
                    if (parsed.contains("inheritsFrom") && parsed["inheritsFrom"].is_string()) {
                        item.inherits_from = parsed["inheritsFrom"].get<std::string>();
                        item.has_inherits = !item.inherits_from.empty();
                    }
                    if (parsed.contains("jar") && parsed["jar"].is_string()) {
                        const std::string jar = parsed["jar"].get<std::string>();
                        if (!jar.empty()) {
                            item.jar_name = jar;
                        }
                    }
                }
            } catch (...) {
                // 损坏的 JSON：按未完成处理（下方 jar 校验过滤）
            }
        }
        scanned.push_back(std::move(item));
    }

    // 第一遍：收集所有被继承的父版本 id。
    std::unordered_set<std::string> parents;
    for (const ScannedVersion& item : scanned) {
        if (!item.inherits_from.empty()) {
            parents.insert(item.inherits_from);
        }
    }

    // 父版本若已被用户真正使用（有 Mod / 存档 / 收藏 / 自定义设置），
    // 仍作为独立实例显示；否则视为内部基础隐藏。这样历史安装无需迁移脚本也能收敛。
    // D-087：覆盖 InstanceSettings 的全部「用户主动设置」字段。
    // 旧实现只看 9 个字段，导致用户仅调过内存/GC/隔离/启动参数的原版仍被误隐藏。
    // isolated/decided 不算用户数据——它们是自动隔离判定的产物。
    auto has_user_data = [](const std::filesystem::path& dir) {
        if (version_dir_has_mods(dir) || version_dir_has_saves(dir)) {
            return true;
        }
        util::Result<InstanceSettings> settings = load_instance_settings(dir);
        if (settings.is_ok()) {
            const InstanceSettings& s = settings.value();
            return s.starred || s.category != 0 || !s.custom_info.empty() || !s.logo.empty() ||
                   !s.window_title.empty() || !s.version_info.empty() ||
                   s.java_mode != 0 || !s.java_range.empty() || !s.java_path.empty() ||
                   s.ram_type != 2 || s.ram_optimize != 0 ||
                   !s.server_nide.empty() || !s.server_auth_url.empty() ||
                   !s.server_auth_register.empty() || !s.server_auth_name.empty() ||
                   !s.server_enter.empty() || !s.jvm_args.empty() || !s.game_args.empty() ||
                   s.gc_mode != 0 || !s.pre_launch.empty() || s.pre_launch_wait ||
                   s.disable_mod_update || s.disable_file_check || s.disable_jlw ||
                   s.disable_lua;
        }
        return false;
    };

    // D-087：子版本必须能沿 inheritsFrom 链走到真实存在的客户端 jar。
    // 父版本被删除/改名后，旧实现仍把子版本列为可启动，启动必然失败。
    std::unordered_map<std::string, const ScannedVersion*> by_id;
    by_id.reserve(scanned.size());
    for (const ScannedVersion& item : scanned) {
        by_id[item.id] = &item;
    }
    auto chain_complete = [&](const ScannedVersion& version) {
        std::unordered_set<std::string> visited;
        const ScannedVersion* current = &version;
        while (current->has_inherits) {
            if (!visited.insert(current->id).second) {
                return false;  // inheritsFrom 环
            }
            const auto parent = by_id.find(current->inherits_from);
            if (parent == by_id.end()) {
                return false;  // 父版本 JSON 缺失
            }
            current = parent->second;
        }
        std::error_code jar_ec;
        const std::filesystem::path jar_path =
            m_layout.versions_dir / current->jar_name / (current->jar_name + ".jar");
        return std::filesystem::is_regular_file(jar_path, jar_ec);
    };

    std::vector<InstalledVersion> result;
    for (const ScannedVersion& item : scanned) {
        if (parents.find(item.id) != parents.end() && !has_user_data(item.dir)) {
            util::log_info("[instance] " + item.id +
                           " hidden: loader parent (internal base, PCL parity)");
            continue;
        }
        // D-087：加载器子版本先验证整条父链，链断（父 JSON/最终 jar 缺失）即视为损坏。
        if (item.has_inherits && !chain_complete(item)) {
            util::log_warn("[install] " + item.id +
                           " hidden: inheritsFrom chain incomplete (" + item.inherits_from + ")");
            continue;
        }
        // D-028 完整性过滤：非 inheritsFrom 版本必须存在客户端 jar，
        // 否则视为**未完成下载**（中断/崩溃残留），不认定为可启动版本。
        if (!item.has_inherits) {
            std::error_code jar_ec;
            const std::filesystem::path jar_path =
                m_layout.versions_dir / item.jar_name / (item.jar_name + ".jar");
            if (!std::filesystem::is_regular_file(jar_path, jar_ec)) {
                util::log_warn("[install] " + item.id +
                               " hidden: client jar missing (incomplete)");
                continue;
            }
        }
        InstalledVersion installed;
        installed.id = item.id;
        installed.type = item.type;
        result.push_back(std::move(installed));
    }
    std::sort(result.begin(), result.end(),
              [](const InstalledVersion& a, const InstalledVersion& b) {
                  return compare_version_ids(a.id, b.id) > 0;  // 新版本在前
              });
    return result;
}

util::Result<LaunchOutcome> GameService::launch_installed(const LaunchRequest& request,
                                                          std::stop_token stop,
                                                          const InstallProgressFn& progress) {
    util::log_info("[launch] prepare " + request.version_id);
    ProgressReporter reporter(progress);
    ProgressReporter* reporter_ptr = progress ? &reporter : nullptr;

    // 1. 版本链解析
    util::Result<VersionJson> loaded = load_version(request.version_id);
    if (loaded.is_error()) {
        return util::Result<LaunchOutcome>::fail(std::move(loaded.error()));
    }
    VersionJson version = std::move(loaded.value());
    const std::filesystem::path version_dir = m_layout.versions_dir / version.id;

    // V5：读取每版本启动设置，合并到本次启动（不修改全局 request）。
    InstanceSettings launch_instance_settings;
    util::Result<InstanceSettings> launch_settings_loaded =
        load_instance_settings(version_dir);
    if (launch_settings_loaded.is_ok()) {
        launch_instance_settings = launch_settings_loaded.value();
    }
    const InstanceLaunchOverrides overrides =
        merge_instance_launch(launch_instance_settings, request.settings.memory_mb);

    // 运行目录（D-056）：隔离版本用 versions/<id>，否则用全局游戏根。
    util::Result<InstanceTarget> instance = resolve_instance_target(
        m_layout, m_isolation_mode, version, /*persist_decision=*/true);
    if (instance.is_error()) {
        return util::Result<LaunchOutcome>::fail(std::move(instance.error()));
    }

    // 2. 补全缺失文件 + natives + 资产（并行；哈希一致跳过）
    if (reporter_ptr != nullptr) {
        reporter_ptr->set_phase("补全游戏文件");
        reporter_ptr->set_detail("正在检查本地文件（缺失项会自动下载）…");
    }
    util::Result<void> installed =
        install_parallel_impl(version, stop, reporter_ptr, /*reset_totals=*/true,
                              "补全游戏文件");
    if (installed.is_error()) {
        return util::Result<LaunchOutcome>::fail(std::move(installed.error()));
    }
    // 2.5 加载器产物校验（D-038/D-039）：本地生成库（processor 输出）缺失 ⇒ 明确报错，
    //     而不是把不存在的 jar 拼进 classpath 后让 Java 抛 ClassNotFoundException。
    const std::vector<std::string> missing_local =
        missing_local_libraries(version, m_layout, m_os, {});
    if (!missing_local.empty()) {
        std::string names;
        for (const std::string& name : missing_local) {
            if (!names.empty()) {
                names += ", ";
            }
            names += name;
        }
        return util::Result<LaunchOutcome>::fail(util::Error::make(
            "launch.library_generated_missing",
            "加载器安装不完整（缺少安装器生成的文件），请重新安装该加载器版本", names));
    }
    // 3. 资产已在 install_parallel_impl 内并行完成（D-045/D-052），无需重复。

    // 4. Java 选择（V5）：每版本模式优先；否则全局手动/自动。
    //    javaVersion 缺省按 8（HMCL JavaCandidates 行为）。
    JavaVersionInfo required = version.has_java_version ? version.java_version : JavaVersionInfo{};
    if (required.major_version <= 0) {
        required.major_version = 8;
    }
    std::vector<std::filesystem::path> java_scan_roots;
    for (const std::string& dir : request.settings.java_scan_dirs) {
        if (!dir.empty()) {
            java_scan_roots.push_back(std::filesystem::path(util::utf8_to_wide(dir)));
        }
    }
    util::Result<JavaSelection> java =
        util::Result<JavaSelection>::fail(
            util::Error::make("java.not_selected", "尚未选择 Java", {}));
    bool java_resolved = false;

    auto make_selection = [&required](JavaRuntime runtime, int target_major) {
        JavaSelection selection;
        selection.runtime = std::move(runtime);
        selection.match_kind = selection.runtime.version.major_version == target_major
                                   ? JavaMatchKind::kExact
                                   : JavaMatchKind::kCompatibleHigher;
        return selection;
    };
    auto try_java_path = [&](const std::filesystem::path& path) -> util::Error {
        util::Result<JavaRuntime> runtime = validate_java_path(path);
        if (runtime.is_error()) {
            return runtime.error();
        }
        if (runtime.value().version.major_version < required.major_version) {
            return util::Error::make(
                "java.too_old", "该 Java 版本低于此游戏版本的要求",
                runtime.value().version.version_string + " < " +
                    std::to_string(required.major_version));
        }
        java = util::Result<JavaSelection>::ok(
            make_selection(std::move(runtime.value()), required.major_version));
        java_resolved = true;
        return util::Error{};
    };

    if (overrides.java_mode == 3) {
        if (overrides.java_path.empty()) {
            return util::Result<LaunchOutcome>::fail(util::Error::make(
                "java.not_selected", "该版本设置为“指定 Java”，但路径为空", {}));
        }
        const util::Error error = try_java_path(
            std::filesystem::path(util::utf8_to_wide(overrides.java_path)));
        if (!java_resolved) {
            return util::Result<LaunchOutcome>::fail(error);
        }
    } else if (overrides.java_mode == 2) {
        const std::filesystem::path bundled = find_version_java(version_dir);
        if (bundled.empty()) {
            return util::Result<LaunchOutcome>::fail(util::Error::make(
                "java.version_folder_missing",
                "该版本设置为“使用版本文件夹中的 Java”，但未找到 jre/java/runtime/bin",
                util::wide_to_utf8(version_dir.wstring())));
        }
        const util::Error error = try_java_path(bundled);
        if (!java_resolved) {
            return util::Result<LaunchOutcome>::fail(error);
        }
    } else if (overrides.java_mode == 1) {
        int min_major = 0;
        int max_major = 0;
        bool max_inclusive = false;
        if (!parse_java_range(overrides.java_range, &min_major, &max_major, &max_inclusive)) {
            return util::Result<LaunchOutcome>::fail(util::Error::make(
                "java.invalid_range", "Java 版本范围格式无效", overrides.java_range));
        }
        if (reporter_ptr != nullptr) {
            reporter_ptr->set_phase("检查 Java 环境");
            reporter_ptr->set_detail("正在检测符合 " + overrides.java_range + " 的 Java…");
        }
        const std::vector<JavaRuntime> runtimes = detect_java_runtimes(java_scan_roots);
        int best_major = 0;
        JavaRuntime best{};
        bool found = false;
        for (const JavaRuntime& runtime : runtimes) {
            const int major = runtime.version.major_version;
            if (major < min_major) {
                continue;
            }
            if (max_inclusive ? major > max_major : major >= max_major) {
                continue;
            }
            if (!found || major < best_major) {
                best = runtime;
                best_major = major;
                found = true;
            }
        }
        if (!found) {
            return util::Result<LaunchOutcome>::fail(util::Error::make(
                "java.range_no_match", "没有找到符合范围的 Java", overrides.java_range));
        }
        java = util::Result<JavaSelection>::ok(make_selection(std::move(best), min_major));
        java_resolved = true;
    } else {
        // mode 0：跟随全局。
        // D-115：先按设置页的 Java 优先级列表逐项尝试 —— 用户排序即优先级，
        // 第一个满足版本要求的运行时被选中；低于要求的会被 try_java_path 跳过。
        for (const std::string& candidate : request.settings.java_candidates) {
            if (java_resolved) {
                break;
            }
            if (candidate.empty()) {
                continue;
            }
            (void)try_java_path(std::filesystem::path(util::utf8_to_wide(candidate)));
        }
        // 旧字段兜底（兼容升级前的 java_path），不可用则回退自动检测。
        if (!java_resolved && !request.settings.java_path.empty()) {
            const util::Error error = try_java_path(
                std::filesystem::path(util::utf8_to_wide(request.settings.java_path)));
            if (!java_resolved) {
                util::log_warn("[launch] 配置的 Java 路径不可用：" + error.message +
                               "，回退自动检测");
            }
        }
        if (!java_resolved) {
            if (reporter_ptr != nullptr) {
                reporter_ptr->set_phase("检查 Java 环境");
                reporter_ptr->set_detail("正在检测 Java " +
                                         std::to_string(required.major_version) +
                                         " 运行时（首次可能较慢）…");
            }
            java = auto_select_java_runtime(required, java_scan_roots);
            java_resolved = java.is_ok();
        }
    }
    // D-091：自动模式下仍找不到 Java → 下载匹配主版本的 JRE 到 <data>/runtime，再复核。
    // 仅在 mode 0（跟随全局）触发；用户显式指定路径/版本文件夹时不擅自下载。
    if (!java_resolved && overrides.java_mode == 0 && required.major_version > 0) {
        const std::filesystem::path runtime_root =
            m_layout.game_dir.parent_path() / L"runtime";
        if (reporter_ptr != nullptr) {
            reporter_ptr->set_phase("下载 Java 运行时");
            reporter_ptr->set_detail(
                "未找到 Java " + std::to_string(required.major_version) +
                "，正在自动下载（首次约 40MB）…");
        }
        util::log_info("[java] auto download jre " + std::to_string(required.major_version));
        JavaDownloadOptions download_options;
        download_options.runtime_root = runtime_root;
        util::Result<std::filesystem::path> installed = ensure_java_runtime(
            required.major_version, download_options, m_scheduler, nullptr, stop);
        if (installed.is_ok()) {
            util::Result<JavaRuntime> runtime =
                validate_java_path(installed.value() / L"bin" / L"java.exe");
            if (runtime.is_ok() &&
                runtime.value().version.major_version >= required.major_version) {
                java = util::Result<JavaSelection>::ok(
                    make_selection(std::move(runtime.value()), required.major_version));
                java_resolved = true;
            } else {
                util::log_warn("[java] downloaded runtime failed validation");
            }
        } else {
            util::log_warn("[java] auto download failed: " + installed.error().code + " / " +
                           installed.error().detail);
        }
    }
    if (!java_resolved || java.is_error()) {
        return util::Result<LaunchOutcome>::fail(std::move(java.error()));
    }
    util::log_info("[launch] java " + java.value().runtime.version.version_string + " -> " +
                   util::path_to_utf8(java.value().runtime.executable));
    if (!launch_instance_settings.pre_launch.empty()) {
        const std::string command = launch_instance_settings.pre_launch;
        if (launch_instance_settings.pre_launch_wait) {
            auto result = platform::run_process(L"cmd.exe", {"/c", command},
                                                instance.value().run_dir, -1);
            if (result.is_error()) {
                return util::Result<LaunchOutcome>::fail(util::Error::make(
                    "launch.pre_launch_failed", "启动前命令执行失败",
                    result.error().detail));
            }
        } else {
            std::thread([command, run_dir = instance.value().run_dir]() {
                (void)platform::run_process(L"cmd.exe", {"/c", command}, run_dir, -1);
            }).detach();
        }
    }
    if (reporter_ptr != nullptr) {
        reporter_ptr->set_phase("启动游戏");
        reporter_ptr->set_detail("正在组装启动参数并创建进程…");
    }

    // 5. 组装 LaunchContext（knowledge/launch-args.md §1/§2）
    LaunchContext context;
    context.account.player_name = request.account.username;
    context.account.uuid_compact = request.account.uuid_compact;
    context.account.access_token = request.account.access_token;
    context.account.user_type = request.account.user_type;
    context.account.user_properties = request.account.user_properties;
    // C-7：--clientId = 微软应用 Client ID；--xuid 无 xuid 时 "0"。
    context.client_id = std::string(microsoft_client_id());
    context.auth_xuid = "0";

    const std::string jar_name = version.jar.empty() ? version.id : version.jar;
    context.paths.java_exe = java.value().runtime.executable;
    context.paths.game_dir = instance.value().run_dir;
    // D-107：语言设置写在启动前（隔离版本用各自的 run_dir）。
    apply_game_language(context.paths.game_dir, request.settings.auto_chinese_language);
    context.paths.natives_dir = version_dir / (version.id + "-natives");
    context.paths.assets_dir = m_layout.assets_dir;
    context.paths.libraries_dir = m_layout.libraries_dir;
    context.paths.version_jar = m_layout.versions_dir / jar_name / (jar_name + ".jar");
    if (version.has_logging) {
        const std::filesystem::path log4j = version_dir / "log4j2.xml";
        std::error_code ec;
        if (std::filesystem::is_regular_file(log4j, ec)) {
            context.paths.has_log4j = true;
            context.paths.log4j_config = log4j;
        }
    }

    context.settings = request.settings;
    context.settings.memory_mb = overrides.memory_mb;
    // D-118：启动请求自带的 JVM 参数（外置登录 -javaagent）与版本级追加参数合并，
    // 不能直接覆盖，否则实例里的自定义 JVM 参数会把登录注入挤掉。
    context.settings.jvm_args = request.settings.jvm_args;
    if (!overrides.jvm_args.empty()) {
        if (!context.settings.jvm_args.empty()) {
            context.settings.jvm_args += " ";
        }
        context.settings.jvm_args += overrides.jvm_args;
    }
    context.settings.game_args = overrides.game_args;
    context.settings.gc_mode = overrides.gc_mode;
    // D-104：版本级留空时回退设置页的全局「自定义信息 / 游戏窗口标题」。
    context.settings.version_info = overrides.version_info.empty()
                                        ? request.settings.version_info
                                        : overrides.version_info;
    context.settings.window_title = overrides.window_title.empty()
                                        ? request.settings.window_title
                                        : overrides.window_title;
    context.settings.server_enter = overrides.server_enter;
    context.settings.java_major_version = java.value().runtime.version.major_version;
    context.version_name = version.id;
    context.version_type =
        context.settings.version_info.empty() ? version.type : context.settings.version_info;
    context.os = m_os;

    // D-028：启动可取消——创建进程前最终检查（补全阶段的取消也在此收口）。
    if (stop.stop_requested()) {
        return util::Result<LaunchOutcome>::fail(
            util::Error::make("launch.cancelled", "启动已取消", {}));
    }

    // 6. 拼参 → 创建进程（可见窗口）
    util::Result<LaunchPlan> plan = build_launch_plan(version, context);
    if (plan.is_error()) {
        return util::Result<LaunchOutcome>::fail(std::move(plan.error()));
    }
    util::Result<GameProcess> process = launch_game(plan.value(), context.paths.game_dir);
    if (process.is_error()) {
        return util::Result<LaunchOutcome>::fail(std::move(process.error()));
    }

    util::log_info("[launch] started " + version.id + " pid=" +
                   std::to_string(process.value().pid));
    LaunchOutcome outcome;
    outcome.process = std::move(process.value());
    outcome.java_executable = util::path_to_utf8(java.value().runtime.executable);
    outcome.java_version = java.value().runtime.version.version_string;
    outcome.version = std::move(version);
    return util::Result<LaunchOutcome>::ok(std::move(outcome));
}

}  // namespace sscl::core
