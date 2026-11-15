#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <stop_token>
#include <thread>
#include <vector>

#include "core/download/adaptive_concurrency.hpp"
#include "core/download/mirror_map.hpp"
#include "core/download/rate_limiter.hpp"
#include "core/game/game_service.hpp"

namespace core = sscl::core;

namespace sscl::core {
std::filesystem::path make_download_temp_path(const std::filesystem::path& destination);
}

TEST_CASE("download.mirror: 官方 → BMCLAPI 映射") {
    CHECK(core::map_to_mirror(
              "https://piston-meta.mojang.com/mc/game/version_manifest.json") ==
          "https://bmclapi2.bangbang93.com/mc/game/version_manifest.json");
    CHECK(core::map_to_mirror(
              "https://piston-data.mojang.com/v1/products/java-runtime/abc") ==
          "https://bmclapi2.bangbang93.com/v1/products/java-runtime/abc");
    CHECK(core::map_to_mirror(
              "https://resources.download.minecraft.net/00/001234") ==
          "https://bmclapi2.bangbang93.com/assets/00/001234");
    CHECK(core::map_to_mirror(
              "https://libraries.minecraft.net/com/mojang/patchy/1.1/patchy-1.1.jar") ==
          "https://bmclapi2.bangbang93.com/maven/com/mojang/patchy/1.1/patchy-1.1.jar");
    CHECK(core::map_to_mirror("https://example.com/x").empty());
}

TEST_CASE("download.mirror: 源候选列表（官方优先 + libraries 双映射）") {
    const auto candidates = core::build_source_candidates(
        "https://libraries.minecraft.net/a/b.jar");
    REQUIRE(candidates.size() == 3);
    CHECK(candidates[0] == "https://libraries.minecraft.net/a/b.jar");
    CHECK(candidates[1] == "https://bmclapi2.bangbang93.com/maven/a/b.jar");
    CHECK(candidates[2] == "https://bmclapi2.bangbang93.com/libraries/a/b.jar");

    const auto simple = core::build_source_candidates(
        "https://piston-meta.mojang.com/mc/game/version_manifest.json");
    REQUIRE(simple.size() == 2);
    CHECK(simple[0] == "https://piston-meta.mojang.com/mc/game/version_manifest.json");
    CHECK(simple[1] == "https://bmclapi2.bangbang93.com/mc/game/version_manifest.json");
}

TEST_CASE("download.mirror: BMCLAPI 识别") {
    CHECK(core::is_bmclapi_url("https://bmclapi2.bangbang93.com/maven/x.jar"));
    CHECK_FALSE(core::is_bmclapi_url("https://libraries.minecraft.net/x.jar"));
}

// D-107：修复 *.forgecdn.net 死分支 + 补齐 Mod 文件下载镜像，锁定回归。
TEST_CASE("download.mirror: Mod 文件 CDN → MCIM（D-107）") {
    CHECK(core::map_to_mirror("https://edge.forgecdn.net/files/9318/929/jei.jar") ==
          "https://mod.mcimirror.top/files/9318/929/jei.jar");
    CHECK(core::map_to_mirror("https://cdn.modrinth.com/data/abc/versions/1.0/m.jar") ==
          "https://mod.mcimirror.top/data/abc/versions/1.0/m.jar");
    // D-055 合规红线：mediafilez 不走 MCIM。
    CHECK(core::map_to_mirror("https://mediafilez.forgecdn.net/files/1/2/a.jar").empty());
    CHECK(core::map_to_mirror("https://media.forgecdn.net/logo.png").empty());

    const auto cf = core::build_source_candidates(
        "https://edge.forgecdn.net/files/9318/929/jei.jar");
    REQUIRE(cf.size() == 2);
    CHECK(cf[0] == "https://edge.forgecdn.net/files/9318/929/jei.jar");
    CHECK(cf[1] == "https://mod.mcimirror.top/files/9318/929/jei.jar");
}

TEST_CASE("download.mirror: 源策略三档（D-107）") {
    const std::string url =
        "https://piston-meta.mojang.com/mc/game/version_manifest.json";
    const auto official_first = core::make_source_policy(2, 2);
    const auto mirror_first = core::make_source_policy(0, 0);
    CHECK(core::build_source_candidates(url, official_first)[0] == url);
    CHECK(core::build_source_candidates(url, mirror_first)[0] ==
          "https://bmclapi2.bangbang93.com/mc/game/version_manifest.json");
    // 非法值回退默认（1 / 2）。
    const auto fallback = core::make_source_policy(99, -1);
    CHECK(fallback.files_slow_switch());
    CHECK(fallback.community == core::SourcePreference::kPreferOfficial);
}

TEST_CASE("download.limiter: 不限速直接通过") {
    core::RateLimiter limiter(0);
    CHECK(limiter.consume(1024, {}));
    CHECK(limiter.rate() == 0);
}

TEST_CASE("download.limiter: 令牌桶限速生效") {
    core::RateLimiter limiter(1024);  // 1KB/s
    // 桶容量 1 秒 = 1024；首笔 512 应立即通过
    CHECK(limiter.consume(512, {}));
    limiter.set_rate(0);  // 重置为不限速，避免测试阻塞
    CHECK(limiter.consume(512, {}));
}

TEST_CASE("download.adaptive: 吞吐提升加档，持续拥塞降档") {
    core::AdaptiveConcurrencyConfig config;
    config.start_concurrent = 8;
    config.min_concurrent = 4;
    config.max_concurrent = 32;
    core::AdaptiveConcurrency adaptive(config);
    CHECK(adaptive.target() == 8);

    double speed = 1.0e6;
    adaptive.sample(speed, 1000);  // 建立基线
    for (int i = 0; i < 20; ++i) {
        speed *= 1.10;  // 每次比历史最佳快 10% → 应持续加档
        adaptive.sample(speed, 1000);
    }
    CHECK(adaptive.target() == 32);  // 封顶

    adaptive.sample(speed * 0.40, 1000);  // 第一次拥塞：只记 streak
    CHECK(adaptive.target() == 32);
    adaptive.sample(speed * 0.30, 1000);  // 连续第二次：降档
    CHECK(adaptive.target() < 32);
    CHECK(adaptive.target() >= 4);
}

TEST_CASE("download.adaptive: 错误立即减半且不低于下限") {
    core::AdaptiveConcurrencyConfig config;
    config.start_concurrent = 16;
    config.min_concurrent = 4;
    config.max_concurrent = 32;
    core::AdaptiveConcurrency adaptive(config);
    CHECK(adaptive.on_error() == 8);
    CHECK(adaptive.on_error() == 4);
    CHECK(adaptive.on_error() == 4);
}

TEST_CASE("download.piece: 分块线程数按文件大小分级（2MB/5MB/10MB）") {
    constexpr std::int64_t kMb = 1024 * 1024;
    CHECK(core::download_piece_threads(-1) == 1);       // 大小未知：单流
    CHECK(core::download_piece_threads(0) == 1);
    CHECK(core::download_piece_threads(kMb) == 1);
    CHECK(core::download_piece_threads(2 * kMb) == 1);  // 恰好 2MB 不分块（要求「大于」）
    CHECK(core::download_piece_threads(2 * kMb + 1) == 2);
    CHECK(core::download_piece_threads(5 * kMb) == 2);  // 恰好 5MB 仍是 2
    CHECK(core::download_piece_threads(5 * kMb + 1) == 4);
    CHECK(core::download_piece_threads(10 * kMb) == 4); // 恰好 10MB 仍是 4
    CHECK(core::download_piece_threads(10 * kMb + 1) == 6);
    CHECK(core::download_piece_threads(100 * kMb) == 6);
}

TEST_CASE("game.progress: 活动文件与文件事件快照") {
    core::InstallProgress last;
    std::vector<core::DownloadEvent> events;
    core::ProgressReporter reporter([&last, &events](const core::InstallProgress& report) {
        last = report;
        events.insert(events.end(), report.events.begin(), report.events.end());
    });
    reporter.set_phase("测试阶段");
    reporter.set_stage_totals(core::InstallStage::kAssets, 2, 200, 0);

    const std::uint64_t a = reporter.begin_file("a.jar", 100);
    const std::uint64_t b = reporter.begin_file("b.jar", 100);
    reporter.progress(core::InstallStage::kAssets, a, 40, 40, 100, true);
    reporter.end_file(core::InstallStage::kAssets, a, 60, true);
    reporter.flush();

    REQUIRE(last.active.size() == 1);
    CHECK(last.active[0].id == b);
    CHECK(last.active[0].name == "b.jar");
    CHECK(last.completed_files == 1);
    CHECK(last.bytes_done == 100);

    bool started_a = false;
    bool finished_a = false;
    for (const core::DownloadEvent& event : events) {
        if (event.name != "a.jar") {
            continue;
        }
        if (event.kind == core::DownloadEventKind::kStarted) {
            started_a = true;
        } else if (event.kind == core::DownloadEventKind::kFinished) {
            finished_a = true;
        }
    }
    CHECK(started_a);
    CHECK(finished_a);

    // 非下载步骤（安装器处理器）：粗进度 + 不确定标记经 100ms 节流仍能立即送达。
    reporter.set_step(2, 5, true);
    REQUIRE(last.step_total == 5);
    CHECK(last.step_done == 2);
    CHECK(last.step_indeterminate);
    reporter.clear_step();
    CHECK(last.step_total == 0);
    CHECK_FALSE(last.step_indeterminate);
}

// D-117：原版下载 → 加载器安装器/处理器是「总量无法一次算出」的多阶段任务。
// 旧实现第一段结束总进度就是 100%，后续 add_stage_totals 又让它掉回去。
TEST_CASE("game.progress: 分段进度不提前 100%（D-117）") {
    core::InstallProgress last;
    core::ProgressReporter reporter([&last](const core::InstallProgress& report) {
        last = report;
    });

    // 第一段：原版下载映射到 0~50%
    reporter.set_progress_segment(0.0, 0.5);
    reporter.set_stage_totals(core::InstallStage::kVersion, 1, 100, 0);
    const std::uint64_t a = reporter.begin_file("a.jar", 100);
    reporter.progress(core::InstallStage::kVersion, a, 100, 100, 100, true);
    reporter.end_file(core::InstallStage::kVersion, a, 0, true);
    reporter.flush();
    CHECK(last.fraction >= 0.499f);
    CHECK(last.fraction <= 0.501f);  // 段尾 = 50%，而不是 100%

    // 切段：基线归零，从 50% 继续；不能因为上一段已完成而直接跳到 100%
    reporter.set_progress_segment(0.5, 1.0);
    CHECK(last.fraction <= 0.501f);
    CHECK(last.fraction < 0.99f);

    reporter.add_stage_totals(core::InstallStage::kVersion, 1, 100, 0);
    const std::uint64_t b = reporter.begin_file("b.jar", 100);
    reporter.progress(core::InstallStage::kVersion, b, 50, 50, 100, true);
    reporter.flush();
    CHECK(last.fraction > 0.50f);
    CHECK(last.fraction < 0.80f);  // 50% + 50% × 50%

    // 段内下载做完，但任务尚未 complete：上限 99%（否则左栏会四舍五入成 100.0%）
    reporter.progress(core::InstallStage::kVersion, b, 50, 100, 100, true);
    reporter.end_file(core::InstallStage::kVersion, b, 0, true);
    reporter.flush();
    CHECK(last.fraction < 1.0f);
    CHECK(last.fraction >= 0.989f);

    reporter.complete();
    CHECK(last.fraction >= 0.999f);
}

TEST_CASE("game.progress: 处理器步骤推进总进度（D-117）") {
    core::InstallProgress last;
    core::ProgressReporter reporter([&last](const core::InstallProgress& report) {
        last = report;
    });
    reporter.set_progress_segment(0.80, 0.95);
    reporter.set_step(0, 4, true);
    CHECK(last.fraction >= 0.799f);
    CHECK(last.fraction <= 0.801f);

    reporter.set_step(2, 4, true);
    CHECK(last.fraction > 0.85f);  // 80% + 15% × 50%
    CHECK(last.fraction < 0.90f);

    // 没有下载字节时，速度/ETA 不会误报，但进度仍然前进
    reporter.set_step(4, 4, true);
    CHECK(last.fraction >= 0.949f);
    CHECK(last.fraction < 0.99f);
}
