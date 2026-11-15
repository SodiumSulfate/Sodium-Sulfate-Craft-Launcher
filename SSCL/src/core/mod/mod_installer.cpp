#include "core/mod/mod_installer.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <vector>

#include "core/mod/mod_name.hpp"
#include "platform/file_utils.hpp"
#include "util/hash.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

namespace sscl::core {

namespace {

std::string lower_ascii(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char c : text) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string project_key(const ModResolved& resolved) {
    if (!resolved.project.id.empty()) {
        return resolved.project.id;
    }
    return resolved.version.project_id;
}

std::string display_name(const ModResolved& resolved) {
    if (!resolved.project.title.empty()) {
        return resolved.project.title;
    }
    return resolved.version.name;
}

}  // namespace

ModInstallPlan build_install_plan(const InstanceTarget& target,
                                  const std::vector<ModResolved>& resolved,
                                  const ModInstallOptions& options,
                                  ModHashCache& hash_cache) {
    ModInstallPlan plan;
    (void)options;
    if (resolved.empty()) {
        return plan;
    }

    const std::vector<LocalModEntry> locals = list_local_mods(target.mods_dir, false);
    std::map<std::string, LocalModEntry> by_name;
    for (const LocalModEntry& entry : locals) {
        by_name[lower_ascii(entry.file_name)] = entry;
    }

    std::set<std::string> seen_projects;
    std::map<std::string, std::string> planned_names;  // lower name -> project id

    for (const ModResolved& item : resolved) {
        if (item.role == ModPlanRole::kOptional || item.role == ModPlanRole::kEmbedded) {
            ModPlanNotice notice;
            notice.project_id = project_key(item);
            notice.title = display_name(item);
            notice.role = item.role;
            notice.reason = item.role == ModPlanRole::kOptional ? "可选依赖，未自动安装"
                                                                 : "内置依赖，未重复安装";
            plan.notices.push_back(std::move(notice));
            continue;
        }

        const std::string key = project_key(item);
        if (!key.empty() && !seen_projects.insert(key).second) {
            continue;
        }

        ModPlanItem planned;
        planned.project = item.project;
        planned.version = item.version;
        planned.role = item.role;

        const ModFile* selected = select_primary_file(item.version);
        if (selected == nullptr) {
            planned.action = ModPlanAction::kUnavailable;
            planned.note = "没有可下载的文件";
            ++plan.unavailable_count;
            plan.items.push_back(std::move(planned));
            continue;
        }
        planned.file = *selected;
        if (!item.version.downloadable || planned.file.url.empty()) {
            planned.action = ModPlanAction::kUnavailable;
            planned.note = "作者未允许第三方下载，请前往项目页面下载";
            ++plan.unavailable_count;
            plan.items.push_back(std::move(planned));
            continue;
        }

        util::Result<std::string> safe = sanitize_mod_file_name(planned.file.filename);
        if (safe.is_error()) {
            planned.action = ModPlanAction::kUnavailable;
            planned.note = safe.error().detail;
            ++plan.unavailable_count;
            plan.items.push_back(std::move(planned));
            continue;
        }
        planned.file_name = safe.value();
        planned.target_path = target.mods_dir / std::filesystem::path(
                                                   util::utf8_to_wide(planned.file_name));

        const std::string lower_name = lower_ascii(planned.file_name);
        const auto planned_it = planned_names.find(lower_name);
        if (planned_it != planned_names.end()) {
            planned.action = ModPlanAction::kConflict;
            planned.note = "同一安装计划里已有同名文件（" + planned_it->second + "）";
            ++plan.conflict_count;
            plan.items.push_back(std::move(planned));
            continue;
        }
        planned_names[lower_name] = key;

        const auto local_it = by_name.find(lower_name);
        const auto disabled_it = by_name.find(lower_name + ".disabled");
        const LocalModEntry* existing = nullptr;
        bool existing_disabled = false;
        if (local_it != by_name.end()) {
            existing = &local_it->second;
        } else if (disabled_it != by_name.end()) {
            existing = &disabled_it->second;
            existing_disabled = true;
        }

        if (existing != nullptr) {
            util::Result<std::string> old_hash = hash_cache.sha1(existing->full_path);
            if (!existing_disabled && old_hash.is_ok() && !planned.file.sha1.empty() &&
                util::hash_hex_equal(old_hash.value(), planned.file.sha1)) {
                planned.action = ModPlanAction::kSkipSame;
                planned.note = "已安装相同文件（SHA1 一致）";
                ++plan.skip_count;
            } else {
                planned.action = ModPlanAction::kConflict;
                planned.note = existing_disabled ? "同名文件已被禁用（.disabled）"
                                                 : "同名文件内容不同，需要确认覆盖";
                ++plan.conflict_count;
            }
            plan.items.push_back(std::move(planned));
            continue;
        }

        planned.action = ModPlanAction::kInstall;
        ++plan.install_count;
        plan.items.push_back(std::move(planned));
    }
    return plan;
}

util::Result<void> install_mod_plan(const ModInstallPlan& plan, const ModInstallOptions& options,
                                    DownloadScheduler& scheduler, ModHashCache& hash_cache,
                                    const std::filesystem::path& temp_dir,
                                    const ModInstallProgressFn& progress, std::stop_token stop) {
    if (plan.has_conflicts() && !options.overwrite_conflicts) {
        return util::Result<void>::fail(util::Error::make(
            "mod.conflict", "存在同名 Mod，需要确认是否覆盖",
            std::to_string(plan.conflict_count) + " 个冲突"));
    }

    std::vector<const ModPlanItem*> installs;
    std::uint64_t declared_total = 0;
    for (const ModPlanItem& item : plan.items) {
        if (item.action == ModPlanAction::kInstall ||
            (item.action == ModPlanAction::kConflict && options.overwrite_conflicts)) {
            installs.push_back(&item);
            if (item.file.size > 0) {
                declared_total += item.file.size;
            }
        }
    }
    if (installs.empty()) {
        return util::Result<void>::ok();
    }

    util::Result<void> created = ::sscl::platform::create_directories(temp_dir);
    if (created.is_error()) {
        return created;
    }

    // D-100：多文件并行（PCL/HMCL/Axolotl 同款思路）。
    // 过去逐个文件串行下载，3 个 Mod 就是 3 次串行的握手 + 传输；
    // 现在最多 4 个文件同时下，进度按「已完成字节 + 各活动文件字节」汇总，
    // 结束前再补一次 100% 的最终上报（此前最后一份文件不上报，UI 永远停在 <100%）。
    // 注意：不能用 (std::min)<std::size_t>(...) —— 加括号后不再是模板名，
    // <std::size_t> 会被当成比较运算符（C2275）。这里直接写分支，也不受 min/max 宏影响。
    std::size_t worker_count = installs.size();
    if (worker_count > 4) {
        worker_count = 4;
    }
    if (worker_count == 0) {
        worker_count = 1;
    }
    std::vector<std::uint64_t> file_done(installs.size(), 0);
    std::uint64_t completed_bytes = 0;
    int completed_files = 0;
    std::atomic<std::size_t> next_index{0};
    std::atomic<bool> failed{false};
    std::mutex error_mutex;
    util::Error first_error;
    std::mutex report_mutex;
    std::mutex cache_mutex;
    ModInstallProgress current;
    current.total_files = static_cast<int>(installs.size());
    current.total_total = declared_total;

    const auto report_progress = [&](const std::string& file, std::uint64_t current_done,
                                     std::uint64_t current_total) {
        if (!progress) {
            return;
        }
        std::lock_guard<std::mutex> lock(report_mutex);
        std::uint64_t active_bytes = 0;
        for (const std::uint64_t value : file_done) {
            active_bytes += value;
        }
        current.current_file = file;
        current.current_done = current_done;
        current.current_total = current_total;
        current.completed_files = completed_files;
        current.total_done = completed_bytes + active_bytes;
        progress(current);
    };

    const auto run_one = [&](const std::size_t index) {
        const ModPlanItem& item = *installs[index];
        const std::filesystem::path temp =
            temp_dir / (std::to_string(index) + "-" + item.file_name + ".part");
        std::error_code ec;
        std::filesystem::remove(temp, ec);

        DownloadRequest request;
        request.url = item.file.url;
        request.destination = temp;
        request.expected_sha1 = item.file.sha1;
        request.expected_size = item.file.size > 0 ? static_cast<std::int64_t>(item.file.size) : -1;
        request.display_name = item.project.title.empty() ? item.file_name : item.project.title;
        // D-100：Mod 文件普遍 0.3~3MB，按默认分级只有 0~1 线程；这里提示 2~4 线程，
        // 与 PCL/HMCL「256KB 起分段」一致（服务器不支持 Range 会自动回退单流）。
        if (item.file.size >= 2 * 1024 * 1024) {
            request.piece_threads = 4;
        } else if (item.file.size >= 256 * 1024) {
            request.piece_threads = 2;
        }

        report_progress(item.file_name, 0, item.file.size);
        const DownloadProgressFn on_bytes = [&, index, name = item.file_name](
                                                std::uint64_t done, std::uint64_t total) {
            {
                std::lock_guard<std::mutex> lock(report_mutex);
                file_done[index] = done;
            }
            report_progress(name, done, total > 0 ? total : item.file.size);
        };
        util::Result<void> downloaded = scheduler.download_file(request, on_bytes, stop);
        if (downloaded.is_error()) {
            std::filesystem::remove(temp, ec);
            {
                std::lock_guard<std::mutex> lock(error_mutex);
                if (!failed.exchange(true)) {
                    first_error = downloaded.error();
                }
            }
            return;
        }

        util::Result<void> mods_created =
            ::sscl::platform::create_directories(item.target_path.parent_path());
        if (mods_created.is_error()) {
            std::filesystem::remove(temp, ec);
            std::lock_guard<std::mutex> lock(error_mutex);
            if (!failed.exchange(true)) {
                first_error = mods_created.error();
            }
            return;
        }

        if (std::filesystem::exists(item.target_path, ec)) {
            std::filesystem::path backup = item.target_path;
            backup += std::wstring(L".old");
            std::filesystem::remove(backup, ec);
            std::filesystem::rename(item.target_path, backup, ec);
            if (ec) {
                std::filesystem::remove(temp, ec);
                std::lock_guard<std::mutex> lock(error_mutex);
                if (!failed.exchange(true)) {
                    first_error = util::Error::make("mod.install_failed", "无法备份原有 Mod",
                                                    util::path_to_utf8(backup));
                }
                return;
            }
        }

        std::filesystem::rename(temp, item.target_path, ec);
        if (ec) {
            std::filesystem::remove(temp, ec);
            std::lock_guard<std::mutex> lock(error_mutex);
            if (!failed.exchange(true)) {
                first_error = util::Error::make("mod.install_failed", "无法写入 Mod 文件",
                                                util::path_to_utf8(item.target_path));
            }
            return;
        }

        {
            // 已落盘：把该文件计入「已完成字节」，活动字节清零，避免重复累加。
            std::lock_guard<std::mutex> lock(report_mutex);
            std::uint64_t actual = file_done[index];
            if (item.file.size > 0) {
                actual = item.file.size;
            }
            completed_bytes += actual;
            file_done[index] = 0;
            ++completed_files;
        }
        // 新文件写入后让缓存下次重新计算（避免旧路径缓存误命中）。
        std::lock_guard<std::mutex> cache_lock(cache_mutex);
        util::Result<std::string> ignored = hash_cache.sha1(item.target_path);
        (void)ignored;
    };

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers.emplace_back([&]() {
            for (;;) {
                if (failed.load() || stop.stop_requested()) {
                    return;
                }
                const std::size_t index = next_index.fetch_add(1);
                if (index >= installs.size()) {
                    return;
                }
                run_one(index);
            }
        });
    }
    for (std::thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    if (stop.stop_requested()) {
        return util::Result<void>::fail(util::Error::make("mod.cancelled", "已取消", {}));
    }
    if (failed.load()) {
        return util::Result<void>::fail(first_error);
    }

    // 最终上报：保证 100%（任务中心/下载页据此收尾，不会停在 90.x%）。
    if (progress) {
        std::lock_guard<std::mutex> lock(report_mutex);
        current.completed_files = static_cast<int>(installs.size());
        current.total_done = completed_bytes;
        if (current.total_total < current.total_done) {
            current.total_total = current.total_done;
        }
        current.current_done = current.current_total;
        progress(current);
    }

    util::Result<void> saved = hash_cache.save();
    if (saved.is_error()) {
        util::log_warn("[modinstall] hash cache save failed: " + saved.error().detail);
    }
    return util::Result<void>::ok();
}

}  // namespace sscl::core
