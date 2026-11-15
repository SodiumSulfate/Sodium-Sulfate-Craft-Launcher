#include "core/modpack/modpack_importer.hpp"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>

#include "platform/file_utils.hpp"
#include "platform/zip_utils.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

namespace sscl::core {

namespace {

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// 归档内路径 → 安全的相对路径（"/" 分隔）；非法返回空串。
std::string safe_relative_path(const std::string& raw) {
    std::string text = raw;
    for (char& ch : text) {
        if (ch == '\\') {
            ch = '/';
        }
    }
    if (text.empty() || text.front() == '/') {
        return {};
    }
    if (text.size() >= 2 && text[1] == ':') {
        return {};  // 盘符
    }
    std::string normalized;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t slash = text.find('/', start);
        const std::string segment =
            text.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (segment.empty() || segment == "." || segment == "..") {
            return {};
        }
        if (!normalized.empty()) {
            normalized += '/';
        }
        normalized += segment;
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return normalized;
}

std::string json_string(const nlohmann::json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_string()) {
        return {};
    }
    return object[key].get<std::string>();
}

int64_t json_int64(const nlohmann::json& object, const char* key, int64_t fallback) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_number_integer()) {
        return fallback;
    }
    return object[key].get<int64_t>();
}

// mrpack dependencies：识别加载器与版本。
void read_mrpack_dependencies(const nlohmann::json& dependencies, ModpackIndex* index) {
    if (!dependencies.is_object()) {
        return;
    }
    if (dependencies.contains("fabric-loader")) {
        index->loader = "fabric";
        index->loader_version = json_string(dependencies, "fabric-loader");
    } else if (dependencies.contains("forge")) {
        index->loader = "forge";
        index->loader_version = json_string(dependencies, "forge");
    } else if (dependencies.contains("neoforge")) {
        index->loader = "neoforge";
        index->loader_version = json_string(dependencies, "neoforge");
    } else if (dependencies.contains("quilt-loader")) {
        // Quilt 安装尚未支持（D-041）：保留识别，让 UI 给出明确提示。
        index->loader = "quilt";
        index->loader_version = json_string(dependencies, "quilt-loader");
    }
    index->minecraft_version = json_string(dependencies, "minecraft");
}

util::Result<void> parse_mrpack(const std::string& text, ModpackIndex* index) {
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return util::Result<void>::fail(
            util::Error::make("modpack.invalid", "整合包索引不是有效 JSON", {}));
    }
    index->format = ModpackFormat::kModrinth;
    index->name = json_string(root, "name");
    index->version = json_string(root, "versionId");
    if (root.contains("dependencies")) {
        read_mrpack_dependencies(root["dependencies"], index);
    }
    if (!root.contains("files") || !root["files"].is_array()) {
        return util::Result<void>::fail(
            util::Error::make("modpack.invalid", "整合包缺少 files 列表", {}));
    }
    for (const auto& item : root["files"]) {
        if (!item.is_object()) {
            continue;
        }
        ModpackFile file;
        file.path = safe_relative_path(json_string(item, "path"));
        if (file.path.empty()) {
            index->warnings.push_back("跳过非法路径：" + json_string(item, "path"));
            continue;
        }
        if (item.contains("env") && item["env"].is_object() &&
            json_string(item["env"], "client") == "unsupported") {
            continue;
        }
        if (item.contains("hashes") && item["hashes"].is_object()) {
            file.sha1 = json_string(item["hashes"], "sha1");
            file.sha512 = json_string(item["hashes"], "sha512");
        }
        file.size = json_int64(item, "fileSize", -1);
        if (item.contains("downloads") && item["downloads"].is_array() &&
            !item["downloads"].empty() && item["downloads"][0].is_string()) {
            file.url = item["downloads"][0].get<std::string>();
        }
        if (file.url.empty()) {
            index->warnings.push_back("缺少下载地址：" + file.path);
            continue;
        }
        index->files.push_back(std::move(file));
    }
    return util::Result<void>::ok();
}

util::Result<void> parse_curseforge_manifest(const std::string& text, ModpackIndex* index) {
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return util::Result<void>::fail(
            util::Error::make("modpack.invalid", "manifest.json 不是有效 JSON", {}));
    }
    index->format = ModpackFormat::kCurseForge;
    index->name = json_string(root, "name");
    index->version = json_string(root, "version");
    if (root.contains("minecraft") && root["minecraft"].is_object()) {
        const nlohmann::json& minecraft = root["minecraft"];
        index->minecraft_version = json_string(minecraft, "version");
        if (minecraft.contains("modLoaders") && minecraft["modLoaders"].is_array()) {
            for (const auto& loader : minecraft["modLoaders"]) {
                if (!loader.is_object()) {
                    continue;
                }
                const std::string id = json_string(loader, "id");  // forge-47.2.0 / fabric-0.15
                if (id.empty()) {
                    continue;
                }
                const std::size_t dash = id.find('-');
                const std::string kind = dash == std::string::npos ? id : id.substr(0, dash);
                if (kind == "fabric") {
                    index->loader = "fabric";
                } else if (kind == "forge") {
                    index->loader = "forge";
                } else if (kind == "neoforge") {
                    index->loader = "neoforge";
                } else if (kind == "quilt") {
                    index->loader = "quilt";
                } else {
                    continue;
                }
                index->loader_version =
                    dash == std::string::npos ? std::string() : id.substr(dash + 1);
                break;
            }
        }
    }
    if (!root.contains("files") || !root["files"].is_array()) {
        return util::Result<void>::fail(
            util::Error::make("modpack.invalid", "manifest.json 缺少 files 列表", {}));
    }
    for (const auto& item : root["files"]) {
        if (!item.is_object()) {
            continue;
        }
        ModpackFile file;
        file.project_id = std::to_string(json_int64(item, "projectID", 0));
        file.file_id = std::to_string(json_int64(item, "fileID", 0));
        if (file.project_id == "0" || file.file_id == "0") {
            index->warnings.push_back("跳过缺少 projectID/fileID 的条目");
            continue;
        }
        file.required = true;
        if (item.contains("required") && item["required"].is_boolean()) {
            file.required = item["required"].get<bool>();
        }
        // CF 不提供 path：下载后按项目名落 mods/（由 install_modpack_files 生成文件名）。
        file.path = "mods/curseforge-" + file.project_id + "-" + file.file_id + ".jar";
        index->files.push_back(std::move(file));
    }
    return util::Result<void>::ok();
}

}  // namespace

util::Result<ModpackIndex> parse_modpack(const std::filesystem::path& archive) {
    util::Result<std::vector<std::string>> entries = platform::zip_entry_names(archive);
    if (entries.is_error()) {
        return util::Result<ModpackIndex>::fail(util::Error::make(
            "modpack.read_failed", "无法读取整合包：" + util::path_to_utf8(archive),
            entries.error().detail));
    }
    std::string mrpack_entry;
    std::string manifest_entry;
    for (const std::string& name : entries.value()) {
        if (ends_with(name, "modrinth.index.json")) {
            mrpack_entry = name;
        } else if (ends_with(name, "manifest.json")) {
            manifest_entry = name;
        }
    }
    if (mrpack_entry.empty() && manifest_entry.empty()) {
        return util::Result<ModpackIndex>::fail(util::Error::make(
            "modpack.unsupported", "无法识别的整合包格式（支持 .mrpack 与 CurseForge manifest）",
            util::path_to_utf8(archive)));
    }
    const std::string entry = !mrpack_entry.empty() ? mrpack_entry : manifest_entry;
    util::Result<std::string> text = platform::zip_read_entry(archive, entry);
    if (text.is_error()) {
        return util::Result<ModpackIndex>::fail(util::Error::make(
            "modpack.read_failed", "无法读取整合包索引：" + entry, text.error().detail));
    }
    ModpackIndex index;
    util::Result<void> parsed = !mrpack_entry.empty()
                                    ? parse_mrpack(text.value(), &index)
                                    : parse_curseforge_manifest(text.value(), &index);
    if (parsed.is_error()) {
        return util::Result<ModpackIndex>::fail(parsed.error());
    }
    if (index.minecraft_version.empty()) {
        return util::Result<ModpackIndex>::fail(util::Error::make(
            "modpack.invalid", "整合包未声明 Minecraft 版本", util::path_to_utf8(archive)));
    }
    util::log_info("[modpack] parsed " + index.name + " (" + index.minecraft_version + " / " +
                   (index.loader.empty() ? std::string("vanilla") : index.loader) + ") files=" +
                   std::to_string(index.files.size()));
    return util::Result<ModpackIndex>::ok(std::move(index));
}

std::string match_modpack_instance(const ModpackIndex& index,
                                   const std::vector<VersionSummary>& installed) {
    std::string exact;
    std::string mc_only;
    for (const VersionSummary& summary : installed) {
        if (summary.minecraft_version != index.minecraft_version) {
            continue;
        }
        if (summary.state == InstanceState::kError) {
            continue;
        }
        if (summary.loader == index.loader) {
            // 加载器版本也一致时优先（mrpack/CF 均给出确切 loader 版本）。
            if (!index.loader_version.empty() && summary.loader_version == index.loader_version) {
                return summary.version.id;
            }
            if (exact.empty()) {
                exact = summary.version.id;
            }
        } else if (index.loader.empty() && summary.loader.empty() && mc_only.empty()) {
            mc_only = summary.version.id;
        }
    }
    if (!exact.empty()) {
        return exact;
    }
    return mc_only;
}

util::Result<void> resolve_curseforge_urls(ModpackIndex* index,
                                           const ModpackUrlResolver& resolver,
                                           const std::stop_token& stop) {
    if (index == nullptr || index->format != ModpackFormat::kCurseForge) {
        return util::Result<void>::ok();
    }
    if (!resolver) {
        return util::Result<void>::fail(util::Error::make(
            "modpack.curseforge_unavailable", "CurseForge 整合包需要可用的 API Key", {}));
    }
    for (ModpackFile& file : index->files) {
        if (stop.stop_requested()) {
            return util::Result<void>::fail(util::Error::make(
                "modpack.cancelled", "整合包导入已取消", {}));
        }
        if (!file.url.empty() || file.project_id.empty() || file.file_id.empty()) {
            continue;
        }
        util::Result<std::string> url = resolver(file.project_id, file.file_id);
        if (url.is_error()) {
            index->warnings.push_back("CurseForge 文件不可用：" + file.project_id + "/" +
                                      file.file_id + "（" + url.error().message + "）");
            file.required = false;
            continue;
        }
        file.url = url.value();
    }
    return util::Result<void>::ok();
}

util::Result<int> install_modpack_files(const ModpackIndex& index,
                                        const std::filesystem::path& instance_dir,
                                        DownloadScheduler& scheduler,
                                        const ModpackInstallOptions& options,
                                        const ModpackProgressFn& progress,
                                        std::stop_token stop) {
    int total = 0;
    std::uint64_t total_bytes = 0;
    for (const ModpackFile& file : index.files) {
        if (!file.required || file.url.empty()) {
            continue;
        }
        ++total;
        if (file.size > 0) {
            total_bytes += static_cast<std::uint64_t>(file.size);
        }
    }
    if (total == 0) {
        return util::Result<int>::fail(util::Error::make(
            "modpack.no_files", "整合包没有可安装的文件（可能需要 CurseForge API Key）", {}));
    }

    int completed = 0;
    int written = 0;
    std::uint64_t done_bytes = 0;
    for (const ModpackFile& file : index.files) {
        if (stop.stop_requested()) {
            return util::Result<int>::fail(
                util::Error::make("modpack.cancelled", "整合包导入已取消", {}));
        }
        if (!file.required || file.url.empty()) {
            continue;
        }
        const std::wstring wide = util::utf8_to_wide(file.path);
        const std::filesystem::path target = instance_dir / std::filesystem::path(wide);
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (!options.overwrite && std::filesystem::is_regular_file(target, ec)) {
            ++completed;
            if (file.size > 0) {
                done_bytes += static_cast<std::uint64_t>(file.size);
            }
            if (progress) {
                progress(ModpackInstallProgress{completed, total, file.path, done_bytes,
                                                total_bytes});
            }
            continue;
        }

        DownloadRequest request;
        request.url = file.url;
        request.destination = target;
        request.expected_sha1 = file.sha1;
        request.expected_size = file.size;
        request.display_name = file.path;
        util::Result<void> downloaded =
            scheduler.download_file(request, nullptr, stop);
        if (downloaded.is_error()) {
            return util::Result<int>::fail(util::Error::make(
                "modpack.download_failed", "整合包文件下载失败：" + file.path,
                downloaded.error().code + " / " + downloaded.error().detail));
        }
        ++completed;
        ++written;
        if (file.size > 0) {
            done_bytes += static_cast<std::uint64_t>(file.size);
        }
        if (progress) {
            progress(
                ModpackInstallProgress{completed, total, file.path, done_bytes, total_bytes});
        }
    }
    util::log_info("[modpack] installed " + std::to_string(written) + "/" +
                   std::to_string(total) + " files into " + util::path_to_utf8(instance_dir));
    return util::Result<int>::ok(written);
}

}  // namespace sscl::core
