#include "core/java/java_downloader.hpp"

#include "platform/file_utils.hpp"
#include "platform/zip_utils.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <string_view>
#include <system_error>

namespace sscl::core {

namespace {

constexpr const char* kDefaultUrlTemplate =
    "https://api.adoptium.net/v3/binary/latest/{major}/ga/windows/x64/jre/hotspot/normal/eclipse";

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string replace_major(std::string text, int major_version) {
    const std::string placeholder = "{major}";
    const std::string value = std::to_string(major_version);
    std::size_t pos = 0;
    while ((pos = text.find(placeholder, pos)) != std::string::npos) {
        text.replace(pos, placeholder.size(), value);
        pos += value.size();
    }
    return text;
}

// /sdl 下禁用 CRT 的 getenv（C4996），直接走 Win32 宽字符环境变量接口。
std::string env_string(const wchar_t* name) {
    const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring value(static_cast<std::size_t>(needed), L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    value.resize(written);
    return util::wide_to_utf8(value);
}

}  // namespace

std::filesystem::path find_installed_java_runtime(const std::filesystem::path& runtime_root,
                                                  int major_version) {
    if (runtime_root.empty() || major_version <= 0) {
        return {};
    }
    const std::filesystem::path candidate =
        runtime_root / (L"jre-" + std::to_wstring(major_version));
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate / L"bin" / L"java.exe", ec)) {
        return candidate;
    }
    return {};
}

util::Result<std::filesystem::path> ensure_java_runtime(
    int major_version, const JavaDownloadOptions& options, DownloadScheduler& scheduler,
    const JavaDownloadProgressFn& progress, std::stop_token stop) {
    if (major_version <= 0) {
        return util::Result<std::filesystem::path>::fail(
            util::Error::make("java.unavailable", "无效的 Java 主版本", {}));
    }
    if (options.runtime_root.empty()) {
        return util::Result<std::filesystem::path>::fail(
            util::Error::make("java.download_failed", "未提供 Java 运行时目录", {}));
    }
    const std::filesystem::path existing =
        find_installed_java_runtime(options.runtime_root, major_version);
    if (!existing.empty()) {
        return util::Result<std::filesystem::path>::ok(existing);
    }
    if (stop.stop_requested()) {
        return util::Result<std::filesystem::path>::fail(
            util::Error::make("java.download_failed", "Java 下载已取消", {}));
    }

    std::string url_template = options.url_template;
    if (url_template.empty()) {
        url_template = env_string(L"SSCL_JAVA_URL_TEMPLATE");
    }
    if (url_template.empty()) {
        url_template = kDefaultUrlTemplate;
    }
    const std::string url = replace_major(url_template, major_version);

    std::error_code ec;
    std::filesystem::create_directories(options.runtime_root, ec);
    if (ec) {
        return util::Result<std::filesystem::path>::fail(util::Error::make(
            "java.download_failed", "无法创建 Java 运行时目录", ec.message()));
    }
    const std::filesystem::path archive =
        options.runtime_root / (L"jre-" + std::to_wstring(major_version) + L".zip");
    std::filesystem::remove(archive, ec);

    DownloadRequest request;
    request.url = url;
    request.destination = archive;
    request.display_name = "Java " + std::to_string(major_version) + " 运行时";
    util::Result<void> downloaded = scheduler.download_file(request, progress, stop);
    if (downloaded.is_error()) {
        std::filesystem::remove(archive, ec);
        return util::Result<std::filesystem::path>::fail(util::Error::make(
            "java.download_failed", "Java 运行时下载失败：" + downloaded.error().message,
            downloaded.error().detail));
    }
    if (options.max_archive_bytes > 0) {
        const std::uintmax_t size = std::filesystem::file_size(archive, ec);
        if (!ec && size > static_cast<std::uintmax_t>(options.max_archive_bytes)) {
            std::filesystem::remove(archive, ec);
            return util::Result<std::filesystem::path>::fail(util::Error::make(
                "java.download_failed", "Java 运行时压缩包异常过大", std::to_string(size)));
        }
    }

    // 找归档内 `.../bin/java.exe`，以其顶层目录作为解包前缀。
    util::Result<std::vector<std::string>> entries = platform::zip_entry_names(archive);
    std::string prefix;
    if (entries.is_ok()) {
        for (const std::string& name : entries.value()) {
            if (ends_with(name, "/bin/java.exe")) {
                const std::string suffix = "/bin/java.exe";
                prefix = name.substr(0, name.size() - suffix.size() + 1);  // 含末尾 '/'
                break;
            }
        }
    }
    if (prefix.empty()) {
        std::filesystem::remove(archive, ec);
        return util::Result<std::filesystem::path>::fail(util::Error::make(
            "java.extract_failed", "Java 运行时压缩包缺少 bin/java.exe",
            util::path_to_utf8(archive)));
    }

    const std::filesystem::path target =
        options.runtime_root / (L"jre-" + std::to_wstring(major_version));
    std::filesystem::remove_all(target, ec);
    util::Result<std::size_t> extracted =
        platform::zip_extract_subtree(archive, prefix, target);
    std::filesystem::remove(archive, ec);
    if (extracted.is_error()) {
        return util::Result<std::filesystem::path>::fail(util::Error::make(
            "java.extract_failed", "Java 运行时解压失败", extracted.error().detail));
    }
    if (find_installed_java_runtime(options.runtime_root, major_version).empty()) {
        return util::Result<std::filesystem::path>::fail(util::Error::make(
            "java.extract_failed", "Java 运行时解压后缺少 bin/java.exe",
            util::path_to_utf8(target)));
    }
    util::log_info("[java] runtime installed: " + util::path_to_utf8(target) + " (" +
                   std::to_string(extracted.value()) + " files)");
    return util::Result<std::filesystem::path>::ok(target);
}

}  // namespace sscl::core
