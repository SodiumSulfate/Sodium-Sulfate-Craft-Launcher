#include "core/game/version_list.hpp"

#include <utility>

#include <nlohmann/json.hpp>

#include "platform/file_utils.hpp"

namespace sscl::core {

namespace {

using nlohmann::json;

}  // namespace

util::Result<VersionList> parse_version_list(const std::string& json_text) {
    json root = json::parse(json_text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return util::Result<VersionList>::fail(
            util::Error::make("versionlist.parse_failed", "版本清单解析失败", {}));
    }

    VersionList list;
    if (root.contains("latest") && root["latest"].is_object()) {
        list.latest_release = root["latest"].value("release", "");
        list.latest_snapshot = root["latest"].value("snapshot", "");
    }
    if (root.contains("versions") && root["versions"].is_array()) {
        for (const json& node : root["versions"]) {
            if (!node.is_object()) {
                continue;
            }
            VersionListEntry entry;
            entry.id = node.value("id", "");
            entry.type = node.value("type", "");
            entry.url = node.value("url", "");
            entry.time = node.value("time", "");
            entry.release_time = node.value("releaseTime", "");
            if (!entry.id.empty() && !entry.url.empty()) {
                list.entries.push_back(std::move(entry));
            }
        }
    }
    return util::Result<VersionList>::ok(std::move(list));
}

util::Result<VersionList> fetch_version_list(DownloadScheduler& scheduler,
                                             const std::filesystem::path& cache_file,
                                             std::stop_token stop) {
    DownloadRequest request;
    request.url = kVersionListUrl;
    request.destination = cache_file;
    request.display_name = "Minecraft 版本清单";

    util::Result<void> downloaded = scheduler.download_file(request, nullptr, stop);
    if (downloaded.is_error()) {
        // 网络失败回退已有缓存（PCL 行为：缓存可用即可展示）
        std::error_code ec;
        if (std::filesystem::is_regular_file(cache_file, ec)) {
            util::Result<std::string> text = platform::read_file(cache_file);
            if (text.is_ok()) {
                return parse_version_list(text.value());
            }
        }
        return util::Result<VersionList>::fail(std::move(downloaded.error()));
    }

    util::Result<std::string> text = platform::read_file(cache_file);
    if (text.is_error()) {
        return util::Result<VersionList>::fail(std::move(text.error()));
    }
    return parse_version_list(text.value());
}

}  // namespace sscl::core
