#include "core/game/file_plan.hpp"

#include <nlohmann/json.hpp>

namespace sscl::core {

namespace {

using nlohmann::json;

// HMCL Constants.java:30-32（行为数据）
constexpr const char* kDefaultLibraryUrl = "https://libraries.minecraft.net/";
constexpr const char* kDefaultVersionDownloadUrl = "https://bmclapi2.bangbang93.com/versions/";
constexpr const char* kAssetObjectUrl = "https://resources.download.minecraft.net/";

// 仓库基址 + 相对路径（基址为空用官方 libraries 主机；基址可能不带尾斜杠）。
std::string join_repo(const std::string& base, const std::string& relative) {
    if (base.empty()) {
        return std::string(kDefaultLibraryUrl) + relative;
    }
    std::string out = base;
    if (out.back() != '/') {
        out.push_back('/');
    }
    return out + relative;
}

}  // namespace

util::Result<AssetIndex> parse_asset_index(const std::string& json_text) {
    json root = json::parse(json_text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return util::Result<AssetIndex>::fail(
            util::Error::make("assets.parse_failed", "资产索引解析失败", {}));
    }
    AssetIndex index;
    if (!root.contains("objects") || !root["objects"].is_object()) {
        return util::Result<AssetIndex>::ok(std::move(index));
    }
    for (auto it = root["objects"].begin(); it != root["objects"].end(); ++it) {
        if (!it.value().is_object()) {
            continue;
        }
        AssetObject object;
        object.virtual_path = it.key();
        if (it.value().contains("hash") && it.value()["hash"].is_string()) {
            object.hash = it.value()["hash"].get<std::string>();
        }
        if (it.value().contains("size") && it.value()["size"].is_number_unsigned()) {
            object.size = it.value()["size"].get<std::uint64_t>();
        }
        if (!object.hash.empty()) {
            index.objects.push_back(std::move(object));
        }
    }
    return util::Result<AssetIndex>::ok(std::move(index));
}

std::filesystem::path asset_object_path(const InstallLayout& layout, const std::string& hash) {
    const std::string prefix = hash.size() >= 2 ? hash.substr(0, 2) : "00";
    return layout.assets_dir / "objects" / prefix / hash;
}

std::string library_relative_file_path(const Library& library) {
    if (library.has_artifact && !library.artifact.path.empty()) {
        return library.artifact.path;
    }
    return library_relative_path(library.name);
}

std::string library_download_url(const Library& library) {
    if (library.has_artifact && !library.artifact.url.empty()) {
        return library.artifact.url;
    }
    if (library.local_only) {
        return {};  // 处理器产物：禁止下载
    }
    const std::string relative = library_relative_file_path(library);
    if (relative.empty()) {
        return {};
    }
    if (!library.url.empty()) {
        std::string base = library.url;
        if (base.back() != '/') {
            base.push_back('/');
        }
        return base + relative;
    }
    return std::string(kDefaultLibraryUrl) + relative;
}

std::vector<std::string> missing_local_libraries(
    const VersionJson& version, const InstallLayout& layout, const OsInfo& os,
    const std::map<std::string, bool>& features) {
    std::vector<std::string> missing;
    for (const Library& library : version.libraries) {
        if (!library.local_only) {
            continue;
        }
        if (!rules_allow(library.rules, os, features)) {
            continue;
        }
        const std::string relative = library_relative_file_path(library);
        if (relative.empty()) {
            continue;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(layout.libraries_dir / relative, ec)) {
            missing.push_back(library.name);
        }
    }
    return missing;
}

std::vector<DownloadRequest> plan_version_files(const VersionJson& version,
                                                const InstallLayout& layout, const OsInfo& os,
                                                const std::map<std::string, bool>& features) {
    std::vector<DownloadRequest> requests;
    const std::filesystem::path version_dir = layout.versions_dir / version.id;

    // 客户端 jar（Version.java:214-218：jar 缺省 = id；downloads.client 缺省用默认 URL 拼）
    const std::string jar_name = version.jar.empty() ? version.id : version.jar;
    DownloadRequest client;
    client.display_name = jar_name + ".jar";
    client.destination = layout.versions_dir / jar_name / (jar_name + ".jar");
    if (version.has_downloads && version.downloads.has_client &&
        !version.downloads.client.url.empty()) {
        client.url = version.downloads.client.url;
        client.expected_sha1 = version.downloads.client.sha1;
        client.expected_size = version.downloads.client.size;
    } else {
        client.url = std::string(kDefaultVersionDownloadUrl) + jar_name + "/" + jar_name + ".jar";
    }
    requests.push_back(std::move(client));

    // 依赖库（rules 筛选；natives classifier 一并计划）
    for (const Library& library : version.libraries) {
        if (!rules_allow(library.rules, os, features)) {
            continue;
        }

        if (library.has_artifact || library.natives.empty()) {
            const std::string relative = library_relative_file_path(library);
            if (relative.empty()) {
                continue;
            }
            // 本地生成库（Forge/NeoForge 处理器产物）不下载：由安装收尾与启动前校验存在性
            // （knowledge/mod-loader-install.md §7-1）。
            if (!library.local_only) {
                const std::string url = library_download_url(library);
                if (!url.empty()) {
                    DownloadRequest request;
                    request.display_name = library.name;
                    request.destination = layout.libraries_dir / relative;
                    request.url = url;
                    if (library.has_artifact) {
                        request.expected_sha1 = library.artifact.sha1;
                        request.expected_size = library.artifact.size;
                    }
                    requests.push_back(std::move(request));
                }
            }
        }

        const std::string classifier = select_native_classifier(library, os.name, true);
        if (!classifier.empty()) {
            DownloadRequest request;
            request.display_name = library.name + ":" + classifier;
            const auto it = library.classifiers.find(classifier);
            if (it != library.classifiers.end()) {
                const std::string classifier_relative =
                    it->second.path.empty() ? library_relative_path(library.name + ":" + classifier)
                                            : it->second.path;
                request.destination = layout.libraries_dir / classifier_relative;
                if (!it->second.url.empty()) {
                    request.url = it->second.url;
                } else {
                    request.url = join_repo(library.url, classifier_relative);
                }
                request.expected_sha1 = it->second.sha1;
                request.expected_size = it->second.size;
            } else {
                const std::string relative =
                    library_relative_path(library.name + ":" + classifier);
                request.destination = layout.libraries_dir / relative;
                request.url = join_repo(library.url, relative);
            }
            if (!request.destination.empty()) {
                requests.push_back(std::move(request));
            }
        }
    }

    // 资产索引文件
    if (version.has_asset_index && !version.asset_index.url.empty()) {
        DownloadRequest request;
        request.display_name = "asset index " + version.asset_index.id;
        request.destination = layout.assets_dir / "indexes" / (version.asset_index.id + ".json");
        request.url = version.asset_index.url;
        request.expected_sha1 = version.asset_index.sha1;
        requests.push_back(std::move(request));
    }

    // log4j 配置（launch-args.md §1.5：versions/<id>/log4j2.xml）
    if (version.has_logging && !version.logging.file.url.empty()) {
        DownloadRequest request;
        request.display_name = "log4j2.xml";
        request.destination = version_dir / "log4j2.xml";
        request.url = version.logging.file.url;
        request.expected_sha1 = version.logging.file.sha1;
        request.expected_size = version.logging.file.size;
        requests.push_back(std::move(request));
    }

    return requests;
}

std::vector<DownloadRequest> plan_asset_files(const AssetIndex& index,
                                              const InstallLayout& layout) {
    std::vector<DownloadRequest> requests;
    requests.reserve(index.objects.size());
    for (const AssetObject& object : index.objects) {
        DownloadRequest request;
        request.display_name = object.virtual_path;
        request.destination = asset_object_path(layout, object.hash);
        request.url = std::string(kAssetObjectUrl) + object.hash.substr(0, 2) + "/" + object.hash;
        request.expected_sha1 = object.hash;
        request.expected_size = static_cast<std::int64_t>(object.size);
        requests.push_back(std::move(request));
    }
    return requests;
}

}  // namespace sscl::core
