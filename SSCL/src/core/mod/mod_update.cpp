#include "core/mod/mod_update.hpp"

#include <algorithm>
#include <map>
#include <sstream>
#include <utility>

#include "platform/file_utils.hpp"
#include "platform/shell_utils.hpp"
#include "util/string_utils.hpp"

namespace sscl::core {

namespace {

std::string first_sha1(const ModVersion& version) {
    return version.files.empty() ? std::string() : version.files.front().sha1;
}

bool has_version(const ModVersion& version) {
    return !version.version_id.empty();
}

std::string project_title(const ModProject& project, const ModVersion& version) {
    if (!project.title.empty()) {
        return project.title;
    }
    if (!project.id.empty()) {
        return project.id;
    }
    return version.project_id;
}

void merge_candidate(ModUpdateInfo* item, const ModProject& project,
                     const ModVersion& latest) {
    if (!has_version(latest)) {
        return;
    }
    if (has_version(item->latest)) {
        // PCL：发布时间较新者优先；相同时间优先 Modrinth。
        if (latest.published_at < item->latest.published_at) {
            return;
        }
        if (latest.published_at == item->latest.published_at &&
            item->latest.platform != ModPlatform::kCurseForge) {
            return;
        }
    }
    item->latest = latest;
    if (!project.id.empty()) {
        item->project = project;
    }
    if (item->project.title.empty()) {
        item->project.title = project_title(project, latest);
    }
}

}  // namespace

std::string choose_curseforge_latest_file(const ModProject& project,
                                          const std::string& game_version,
                                          ModLoader local_loader) {
    for (const ModProject::LatestFile& latest : project.latest_files) {
        if (!game_version.empty() && latest.game_version != game_version) {
            continue;
        }
        if (local_loader == ModLoader::kUnknown) {
            if (latest.loader != ModLoader::kUnknown) {
                continue;
            }
        } else if (latest.loader != local_loader) {
            continue;
        }
        if (!latest.file_id.empty()) {
            return latest.file_id;
        }
    }
    return {};
}

bool should_update_mod(const ModVersion& current, const ModVersion& latest,
                       const std::string& local_sha1) {
    if (!has_version(current) || !has_version(latest)) {
        return false;
    }
    if (latest.published_at.empty() ||
        latest.published_at <= current.published_at) {
        return false;
    }
    const std::string latest_sha1 = first_sha1(latest);
    if (!latest_sha1.empty() && !local_sha1.empty() && latest_sha1 == local_sha1) {
        return false;
    }
    return latest.downloadable;
}

std::string pcl_update_file_name(const std::string& current_name,
                                 const std::string& latest_name) {
    const std::filesystem::path current_path = util::path_from_utf8(current_name);
    const std::filesystem::path latest_path = util::path_from_utf8(latest_name);
    const std::string current_stem = util::path_to_utf8(current_path.stem());
    const std::string latest_stem = util::path_to_utf8(latest_path.stem());
    auto split = [](const std::string& text) {
        std::vector<std::string> parts;
        std::string current;
        for (const char c : text) {
            if (c == '-') {
                parts.push_back(current);
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        parts.push_back(current);
        return parts;
    };
    std::vector<std::string> current_parts = split(current_stem);
    std::vector<std::string> latest_parts = split(latest_stem);
    bool shortened = false;
    while (!current_parts.empty() && !latest_parts.empty() &&
           current_parts.front() == latest_parts.front()) {
        current_parts.erase(current_parts.begin());
        latest_parts.erase(latest_parts.begin());
        shortened = true;
    }
    while (!current_parts.empty() && !latest_parts.empty() &&
           current_parts.back() == latest_parts.back()) {
        current_parts.pop_back();
        latest_parts.pop_back();
        shortened = true;
    }
    if (!shortened || current_parts.empty() || latest_parts.empty()) {
        return latest_name;
    }
    auto join = [](const std::vector<std::string>& parts) {
        std::string text;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i != 0) {
                text += "-";
            }
            text += parts[i];
        }
        return text;
    };
    const std::string old_core = join(current_parts);
    const std::string new_core = join(latest_parts);
    std::string result = current_name;
    const std::size_t pos = result.find(old_core);
    if (pos == std::string::npos) {
        return latest_name;
    }
    result.replace(pos, old_core.size(), new_core);
    return result;
}

util::Result<int> apply_mod_updates(
    const std::vector<ModUpdateInfo>& updates, DownloadScheduler& scheduler,
    const std::filesystem::path& temp_dir,
    const std::function<void(const ModUpdateProgress&)>& progress,
    const std::stop_token& stop) {
    util::Result<void> created = ::sscl::platform::create_directories(temp_dir);
    if (created.is_error()) {
        return util::Result<int>::fail(std::move(created.error()));
    }
    int applied = 0;
    int total = 0;
    for (const ModUpdateInfo& update : updates) {
        if (update.has_update && !update.latest.files.empty()) {
            ++total;
        }
    }
    int index = 0;
    for (const ModUpdateInfo& update : updates) {
        if (stop.stop_requested()) {
            break;
        }
        if (!update.has_update || update.latest.files.empty()) {
            continue;
        }
        const ModFile& file = update.latest.files.front();
        if (file.url.empty()) {
            continue;
        }
        ++index;
        if (progress) {
            progress(ModUpdateProgress{index, total, file.filename});
        }
        const std::string target_name =
            pcl_update_file_name(update.local.file_name, file.filename);
        const std::filesystem::path temp_file =
            temp_dir / util::path_from_utf8(target_name);
        DownloadRequest request;
        request.url = file.url;
        request.destination = temp_file;
        request.expected_sha1 = file.sha1;
        request.expected_size = file.size > 0 ? static_cast<std::int64_t>(file.size) : -1;
        request.display_name = file.filename;
        util::Result<void> downloaded = scheduler.download_file(request, nullptr, stop);
        if (downloaded.is_error()) {
            continue;
        }
        const std::filesystem::path old_path = update.local.full_path;
        const std::filesystem::path target =
            old_path.parent_path() / util::path_from_utf8(target_name);
        if (!sscl::platform::recycle_file(old_path)) {
            continue;
        }
        std::error_code ec;
        std::filesystem::copy_file(temp_file, target,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(temp_file, ec);
        if (!ec) {
            ++applied;
        }
    }
    return util::Result<int>::ok(applied);
}

util::Result<ModUpdateReport> check_mod_updates(ModCatalog& catalog,
                                                const std::filesystem::path& mods_dir,
                                                const ModUpdateOptions& options,
                                                ModHashCache& hash_cache,
                                                const std::stop_token& stop) {
    ModUpdateReport report;
    const std::vector<LocalModEntry> mods = scan_pcl_mods(mods_dir);
    report.items.reserve(mods.size());
    for (const LocalModEntry& mod : mods) {
        ModUpdateInfo item;
        item.local = mod;
        util::Result<std::string> sha1 = hash_cache.sha1(mod.full_path);
        if (sha1.is_error()) {
            report.warning = "部分 Mod 无法读取（" + mod.file_name + "）";
            continue;
        }
        item.sha1 = sha1.value();
        util::Result<std::uint32_t> fingerprint =
            hash_cache.curseforge_fingerprint(mod.full_path);
        if (fingerprint.is_ok()) {
            item.curseforge_fingerprint = fingerprint.value();
        }
        report.items.push_back(std::move(item));
        if (static_cast<int>(report.items.size()) >= options.max_requests) {
            break;
        }
    }
    if (report.items.empty()) {
        return util::Result<ModUpdateReport>::ok(std::move(report));
    }

    std::map<std::string, std::size_t> by_sha1;
    std::map<std::uint32_t, std::size_t> by_fingerprint;
    for (std::size_t i = 0; i < report.items.size(); ++i) {
        by_sha1[report.items[i].sha1] = i;
        by_fingerprint[report.items[i].curseforge_fingerprint] = i;
        ++report.checked;
    }

    std::vector<std::string> hashes;
    hashes.reserve(report.items.size());
    for (const ModUpdateInfo& item : report.items) {
        hashes.push_back(item.sha1);
    }

    // Modrinth：先查本地 hash 对应版本，再查精确 MC/加载器的最新版本。
    std::string warning;
    util::Result<std::map<std::string, ModVersion>> current_versions =
        catalog.version_files(ModPlatform::kModrinth, hashes, stop);
    if (current_versions.is_error()) {
        warning = "Modrinth 查询失败：" + current_versions.error().message;
    } else {
        std::vector<ModUpdateQuery> queries;
        for (const auto& [hash, version] : current_versions.value()) {
            const auto found = by_sha1.find(hash);
            if (found == by_sha1.end()) {
                continue;
            }
            ModUpdateInfo& item = report.items[found->second];
            if (!first_sha1(version).empty() && first_sha1(version) != item.sha1) {
                continue;
            }
            item.current = version;
            item.project.platform = ModPlatform::kModrinth;
            item.project.id = version.project_id;
            item.project.title = version.project_id;
            ModUpdateQuery query;
            query.hash = item.sha1;
            query.game_version = options.game_version;
            query.loaders = version.loaders;
            if (query.loaders.empty() && options.loader != ModLoader::kUnknown) {
                query.loaders.push_back(options.loader);
            }
            queries.push_back(std::move(query));
        }
        if (!queries.empty()) {
            util::Result<std::map<std::string, ModVersion>> latest_versions =
                catalog.update_individual(ModPlatform::kModrinth, queries, stop);
            if (latest_versions.is_error()) {
                warning = "Modrinth 更新查询失败：" + latest_versions.error().message;
            } else {
                for (const auto& [hash, version] : latest_versions.value()) {
                    const auto found = by_sha1.find(hash);
                    if (found == by_sha1.end()) {
                        continue;
                    }
                    ModUpdateInfo& item = report.items[found->second];
                    ModProject project;
                    project.platform = ModPlatform::kModrinth;
                    project.id = version.project_id;
                    project.title = version.project_id;
                    merge_candidate(&item, project, version);
                }
            }
        }
    }

    // CurseForge：指纹 → 项目 latestFilesIndexes → 文件。
    std::vector<std::uint32_t> fingerprints;
    fingerprints.reserve(report.items.size());
    for (const ModUpdateInfo& item : report.items) {
        fingerprints.push_back(item.curseforge_fingerprint);
    }
    util::Result<std::vector<CurseForgeFingerprintMatch>> matches =
        catalog.curseforge_fingerprints(fingerprints, stop);
    if (matches.is_error()) {
        if (!warning.empty()) {
            warning += "；";
        }
        warning += "CurseForge 指纹查询失败：" + matches.error().message;
    } else if (!matches.value().empty()) {
        std::vector<std::string> project_ids;
        for (const CurseForgeFingerprintMatch& match : matches.value()) {
            if (!match.project_id.empty()) {
                project_ids.push_back(match.project_id);
            }
        }
        util::Result<std::vector<ModProject>> projects =
            catalog.curseforge_projects(project_ids, stop);
        std::map<std::string, ModProject> project_map;
        if (projects.is_ok()) {
            for (const ModProject& project : projects.value()) {
                project_map[project.id] = project;
            }
        }
        std::vector<std::string> file_ids;
        std::vector<std::pair<std::size_t, std::string>> file_to_item;
        for (const CurseForgeFingerprintMatch& match : matches.value()) {
            std::uint32_t fingerprint = 0;
            try {
                fingerprint = static_cast<std::uint32_t>(std::stoul(match.fingerprint));
            } catch (...) {
                continue;
            }
            const auto item_it = by_fingerprint.find(fingerprint);
            if (item_it == by_fingerprint.end()) {
                continue;
            }
            ModUpdateInfo& item = report.items[item_it->second];
            item.current = match.file;
            const auto project_it = project_map.find(match.project_id);
            if (project_it != project_map.end()) {
                item.project = project_it->second;
                const std::string file_id = choose_curseforge_latest_file(
                    project_it->second, options.game_version, options.loader);
                if (!file_id.empty()) {
                    file_ids.push_back(file_id);
                    file_to_item.emplace_back(item_it->second, file_id);
                }
            }
        }
        if (!file_ids.empty()) {
            util::Result<std::vector<ModVersion>> files =
                catalog.curseforge_files(file_ids, stop);
            if (files.is_ok()) {
                std::map<std::string, ModVersion> file_map;
                for (const ModVersion& file : files.value()) {
                    file_map[file.version_id] = file;
                }
                for (const auto& [index, file_id] : file_to_item) {
                    const auto file_it = file_map.find(file_id);
                    if (file_it == file_map.end()) {
                        continue;
                    }
                    ModUpdateInfo& item = report.items[index];
                    merge_candidate(&item, item.project, file_it->second);
                }
            } else if (!warning.empty()) {
                warning += "；";
                warning += "CurseForge 文件查询失败：" + files.error().message;
            }
        }
    }

    for (ModUpdateInfo& item : report.items) {
        item.has_update = should_update_mod(item.current, item.latest, item.sha1);
        if (item.has_update) {
            ++report.with_update;
        }
    }
    report.warning = std::move(warning);
    return util::Result<ModUpdateReport>::ok(std::move(report));
}

}  // namespace sscl::core
