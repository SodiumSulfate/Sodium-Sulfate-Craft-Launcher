#pragma once

// 游戏安装编排（M1 收官）：版本链解析 → 文件计划 → 下载缺失（哈希一致跳过）
// → natives 解压 → 资产对象安装（并发）。

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

#include "core/account/account.hpp"
#include "core/download/download_scheduler.hpp"
#include "core/game/file_plan.hpp"
#include "core/game/instance_paths.hpp"
#include "core/game/instance_registry.hpp"
#include "core/game/instance_types.hpp"
#include "core/launch/game_launcher.hpp"
#include "core/launch/version_json.hpp"
#include "core/loader/loader_types.hpp"
#include "util/result.hpp"

namespace sscl::core {

// 下载阶段：版本文件（jar/库/natives/索引）与资源文件（assets 对象）并行下载。
enum class InstallStage {
    kVersion = 0,
    kAssets = 1,
};

// 单阶段进度（D-045：总量预计算 + 未知大小加权，保证单调不倒退）。
struct StageProgress {
    std::uint64_t bytes_done = 0;
    std::uint64_t bytes_total = 0;   // 清单声明大小的字节总和（开始前固定，不再变化）
    std::uint64_t files_done = 0;
    std::uint64_t files_total = 0;
    std::uint64_t files_unknown = 0;  // 大小未知的文件数（用平均值加权）
    std::uint64_t unknown_done = 0;   // 已完成的大小未知文件数
    float fraction = 0.0f;
};

// 正在下载的活动文件（批次 42：任务页右侧底部逐文件进度条）。
struct ActiveDownloadInfo {
    std::uint64_t id = 0;
    std::string name;
    std::uint64_t bytes = 0;
    std::uint64_t total = 0;  // 0 = 大小未知
};

// 两次进度上报之间发生的文件级事件（批次 42：apt 风格滚动日志）。
enum class DownloadEventKind {
    kStarted,
    kFinished,
    kFailed,
};

struct DownloadEvent {
    DownloadEventKind kind = DownloadEventKind::kStarted;
    std::uint64_t id = 0;
    std::string name;
    std::uint64_t bytes = 0;  // kStarted = 预期大小；kFinished/kFailed = 已下载字节
};

// 安装/下载进度上报（D-019 任务页；D-045 三进度条；D-052 子状态；批次 42 逐文件事件）。
struct InstallProgress {
    std::string phase;         // 阶段文案（"下载原版文件" / "补全加载器文件" / "运行安装器处理器" 等）
    std::string detail;        // 子状态：当前正在做什么（安装器下载 / Java 检测 / 处理器 i/N）
    std::vector<ActiveDownloadInfo> active;  // 正在下载的文件（按开始顺序，最多约并发上限个）
    std::vector<DownloadEvent> events;       // 自上次上报以来的文件开始/结束事件
    // 非下载步骤进度（安装器处理器 / Java 检测等无法用字节衡量的阶段）：
    // step_total=0 表示总量未知；step_indeterminate=true 时页面绘制循环动画。
    std::uint64_t step_done = 0;
    std::uint64_t step_total = 0;
    bool step_indeterminate = false;
    StageProgress version;            // 版本文件阶段
    StageProgress assets;             // 资源文件阶段
    // 汇总（单调递增；fraction 为总进度）
    std::uint64_t bytes_done = 0;
    std::uint64_t bytes_total = 0;
    std::uint64_t completed_files = 0;
    std::uint64_t total_files = 0;
    float fraction = 0.0f;
};
using InstallProgressFn = std::function<void(const InstallProgress&)>;

// 进度聚合器：多线程安全的字节增量累计 + ~100ms 节流上报（PCL 0.1s 刷新对照）。
// 线程安全；stage 的总量必须在开始下载前一次性设定（set_stage_totals），之后不再改变。
class ProgressReporter {
public:
    explicit ProgressReporter(InstallProgressFn fn);

    void set_phase(const std::string& phase);
    // 子状态（当前正在做什么）；不影响进度数值，仅用于"不要干等"的文案。
    void set_detail(const std::string& detail);

    // 开始某阶段前设定总量（files/bytes = 待下载项；unknown_files = 其中大小未知的项数）。
    void set_stage_totals(InstallStage stage, std::uint64_t files, std::uint64_t bytes,
                          std::uint64_t unknown_files);
    // 追加总量（加载器等"同一任务内的后续阶段"），不清零已完成数，保证总进度不回退。
    void add_stage_totals(InstallStage stage, std::uint64_t files, std::uint64_t bytes,
                          std::uint64_t unknown_files);

    // 开始下载一个文件；返回句柄，供 progress/end_file 定位（并行时多个文件交错）。
    std::uint64_t begin_file(const std::string& name, std::uint64_t expected_total);
    // delta：本次自上次回调以来新增字节；current/file_total：当前文件进度（file_total 可 0）。
    // count_bytes=false 表示该文件大小未知（只用文件数加权，避免总量失真）。
    void progress(InstallStage stage, std::uint64_t id, std::uint64_t delta,
                  std::uint64_t current, std::uint64_t file_total, bool count_bytes);
    // 文件完成；remaining_delta = 该文件剩余未计入的字节。
    void end_file(InstallStage stage, std::uint64_t id, std::uint64_t remaining_delta,
                  bool count_bytes);
    // 文件取消/失败：移出活动列表，不计入完成数。
    // failed=false 用于用户取消/连带取消（不发 kFailed 事件，避免日志刷出 E: 行）。
    void drop_file(std::uint64_t id, bool failed = true);

    // 非下载步骤（安装器处理器/Java 检测）：done/total 提供粗进度；
    // total=0 或 indeterminate=true 时，UI 绘制循环动画表示"仍在工作"。
    void set_step(std::uint64_t done, std::uint64_t total, bool indeterminate);
    void clear_step();

    // D-117 进度分段：把「当前段」的本地进度映射到总进度的 [begin, end]，并记录切换基线
    // （进入本段前已完成的字节/文件不再重复计入）。用于总量无法一次算出的多阶段任务
    // （原版下载 → 加载器安装器/处理器）：避免第一段结束就把总进度顶到 100%，
    // 也避免后续 add_stage_totals 让进度条从 100% 掉回去。
    void set_progress_segment(double begin, double end);
    // 任务真正完成：进度锁定 100%。未调用时聚合进度上限 99%，
    // 防止"还剩一堆事没做但进度显示 100%"（安装器/处理器阶段的常见误报）。
    void complete();

    // 强制立即上报（忽略 100ms 节流）；用于任务收尾，确保最后的事件不丢失。
    void flush();

    // 已累计字节（自适应并发控制器采样用；线程安全）。
    std::uint64_t bytes_done() const;

private:
    void flush_locked(bool force);

    InstallProgressFn m_fn;
    InstallProgress m_state;
    std::map<std::uint64_t, ActiveDownloadInfo> m_active;  // 句柄 → 活动文件
    std::vector<DownloadEvent> m_events;                   // 待上报的文件事件
    std::uint64_t m_next_file_id = 0;
    mutable std::mutex m_mutex;
    std::chrono::steady_clock::time_point m_last_flush{};
    bool m_flushed_once = false;

    // D-117 分段进度：当前段在总进度中的区间 + 进入本段时的累计基线。
    double m_segment_begin = 0.0;
    double m_segment_end = 1.0;
    double m_segment_base_numerator = 0.0;
    double m_segment_base_denominator = 0.0;
    std::uint64_t m_segment_base_files_done = 0;
    std::uint64_t m_segment_base_files_total = 0;
    float m_last_fraction = 0.0f;  // 单调护栏：同一次任务内总进度不允许倒退
    bool m_completed = false;
};

// 启动请求（M1：仅离线/本地账号字段）。
struct LaunchRequest {
    std::string version_id;
    AccountInfo account;
    LaunchSettings settings;
};

struct LaunchOutcome {
    GameProcess process;
    std::string java_executable;
    std::string java_version;
    VersionJson version;
};

// 版本 id 自然比较（数字段按数值，1.20.10 > 1.20.4；1.7.10 < 1.12.2）。
// 左 < 右返回负数，相等 0，左 > 右返回正数。
int compare_version_ids(const std::string& left, const std::string& right);

class GameService {
public:
    GameService(InstallLayout layout, OsInfo os, SchedulerConfig config = {});

    void set_version_loader(VersionTextLoader loader);

    // 解析 inheritsFrom 链并合并（环检测）。
    util::Result<VersionJson> load_version(const std::string& version_id);

    // 版本文件安装计划。
    std::vector<DownloadRequest> plan_version_install(const VersionJson& version) const;

    // 下载缺失版本文件 + natives 解压（已有且哈希一致的跳过）。
    util::Result<void> install_version(const VersionJson& version,
                                       const std::vector<DownloadRequest>& requests,
                                       std::stop_token stop = {},
                                       const InstallProgressFn& progress = {});

    // 资产安装：读取已下载的资产索引 → 并发下载缺失对象。
    util::Result<void> install_assets(const VersionJson& version, std::stop_token stop = {},
                                      const InstallProgressFn& progress = {});

    // 下载版本 JSON（清单 entry 的 url）到 versions/<id>/<id>.json。
    util::Result<void> fetch_version_json(const std::string& url, const std::string& version_id,
                                          std::stop_token stop = {});

    // 一站式安装：下载版本 JSON → 解析继承链 → 版本文件与资源文件**并行**下载 → natives。
    util::Result<VersionJson> install_full(const std::string& version_json_url,
                                           const std::string& version_id,
                                           std::stop_token stop = {},
                                           const InstallProgressFn& progress = {});

    // 安装原版（如未就绪）+ 可选加载器（M3；D-037/D-038）。kNone 等价于 install_full。
    // 落盘：versions/<loader-version-id>/<id>.json（inheritsFrom 原版）。
    util::Result<VersionJson> install_with_loader(const InstallJob& job, std::stop_token stop = {},
                                                 const InstallProgressFn& progress = {});

    // 扫描 versions/<id>/<id>.json；compare_version_ids 降序（新版本在前）。
    // PCL 对齐：被其它版本 inheritsFrom 引用、且无用户数据（mods/saves/收藏/自定义设置）
    // 的父版本是加载器安装的内部基础，不作为独立版本列出（版本之间无父子关联）。
    std::vector<InstalledVersion> installed_versions() const;

    // 版本管理（PCL 2.99 对齐）：列表摘要/卡片分组、每版本设置与操作。
    std::vector<VersionSummary> version_summaries();
    std::vector<InstanceCard> instance_cards(bool show_hidden);
    util::Result<InstanceSettings> instance_settings(const std::string& version_id) const;
    util::Result<void> save_instance_settings_for(const std::string& version_id,
                                                  const InstanceSettings& settings);
    util::Result<void> set_instance_star(const std::string& version_id, bool starred);
    util::Result<void> set_instance_category(const std::string& version_id,
                                             InstanceCategory category);
    util::Result<void> set_instance_description(const std::string& version_id,
                                                const std::string& description);
    util::Result<void> set_instance_logo(const std::string& version_id,
                                         const std::string& logo_path);
    util::Result<void> rename_instance(const std::string& version_id,
                                       const std::string& new_name);
    util::Result<void> delete_instance(const std::string& version_id, bool permanent);

    // 版本隔离（D-056）：全局策略；只影响之后新安装的版本。
    void set_isolation_mode(IsolationMode mode) { m_isolation_mode = mode; }
    IsolationMode isolation_mode() const { return m_isolation_mode; }

    // 解析某版本的运行目录/mods 目录（读每版本覆盖；首次判定会持久化）。
    util::Result<InstanceTarget> resolve_target(const std::string& version_id);

    // 版本管理：逐版本切换隔离（写入 sscl-instance.json）。
    util::Result<InstanceTarget> set_instance_isolation(const std::string& version_id,
                                                        bool isolated);

    // M1 启动编排（§7.1）：补全缺失文件+natives → 资产 → Java 自动选择
    // → 拼启动参数 → 创建游戏进程。Java 选择：javaVersion 缺省按 8。
    // progress 用于启动前补全/检测阶段的状态提示（D-052：避免"干等"）。
    util::Result<LaunchOutcome> launch_installed(const LaunchRequest& request,
                                                 std::stop_token stop = {},
                                                 const InstallProgressFn& progress = {});

    DownloadScheduler& scheduler() { return m_scheduler; }
    const InstallLayout& layout() const { return m_layout; }

    // 切换 MC 文件夹（PCL McFolderSelected）：重新指向 versions/libraries/assets；
    // 清空版本 JSON 加载器缓存，后续安装/启动使用新目录。
    void set_game_folder(const std::filesystem::path& game_dir);

private:
    bool up_to_date(const DownloadRequest& request) const;
    util::Result<std::vector<DownloadRequest>> plan_asset_install(const VersionJson& version) const;

    // 新安装/首次解析时写入版本隔离决策（D-056）；已有决策不覆盖。
    void ensure_instance_decision(const VersionJson& merged);

    // 待下载集合（已过滤就绪项；含总量预计算，供进度条使用）。
    struct PendingDownloads {
        std::vector<DownloadRequest> requests;
        std::uint64_t files = 0;
        std::uint64_t bytes = 0;          // 已知大小的字节总和
        std::uint64_t unknown_files = 0;  // 大小未知的文件数
    };
    PendingDownloads make_pending(const std::vector<DownloadRequest>& requests) const;

    util::Result<void> download_sequential(const PendingDownloads& pending, InstallStage stage,
                                           const std::string& phase, std::stop_token stop,
                                           ProgressReporter* reporter, bool manage_totals = true);

    // 版本文件 + 资源文件统一进入自适应下载池（批次 42；索引预取 → 并发下载 → natives 解压）。
    // reset_totals=false 用于"同一任务内追加阶段"（加载器安装），总进度不回退。
    // phase_label：阶段文案（原版安装 = "下载原版文件"，加载器 = "补全加载器文件"）。
    util::Result<void> install_parallel_impl(const VersionJson& version, std::stop_token stop,
                                             ProgressReporter* reporter, bool reset_totals = true,
                                             const std::string& phase_label =
                                                 "版本文件 + 资源文件");
    util::Result<VersionJson> install_full_impl(const std::string& version_json_url,
                                                const std::string& version_id,
                                                std::stop_token stop,
                                                ProgressReporter* reporter);

    // Forge/NeoForge：下载 installer → 解析 install_profile → 落盘版本 JSON → 安装器依赖
    // → processors（现代）→ 补全库/资产（M3-4/M3-5）。
    util::Result<VersionJson> install_forge_like(const InstallJob& job, const VersionJson& vanilla,
                                                 const std::string& version_id,
                                                 std::stop_token stop,
                                                 ProgressReporter* reporter);

    InstallLayout m_layout;
    OsInfo m_os;
    VersionTextLoader m_loader;
    DownloadScheduler m_scheduler;
    IsolationMode m_isolation_mode = IsolationMode::kAll;
};

}  // namespace sscl::core
