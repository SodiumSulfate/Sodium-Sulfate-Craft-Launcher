#include "core/launch/natives.hpp"

#include <fstream>
#include <string_view>
#include <utility>

#include <miniz/miniz.h>

#include "platform/file_utils.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

#pragma comment(lib, "miniz.lib")

namespace sscl::core {

namespace {

bool has_suffix(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_excluded(const std::string& entry, const std::vector<std::string>& prefixes) {
    for (const std::string& prefix : prefixes) {
        if (!prefix.empty() && entry.compare(0, prefix.size(), prefix) == 0) {
            return true;  // extract.exclude 为路径前缀匹配（ExtractRules.java:33-35）
        }
    }
    return false;
}

bool resolve_entry_target(const std::filesystem::path& natives_dir, const std::string& entry,
                          std::filesystem::path& target) {
    const std::filesystem::path relative(entry);
    if (relative.empty() || relative.is_absolute() || relative.has_root_path()) {
        return false;
    }
    for (const std::filesystem::path& component : relative) {
        if (component == "..") {
            return false;
        }
    }

    const std::filesystem::path normalized_base = natives_dir.lexically_normal();
    const std::filesystem::path normalized_target =
        (normalized_base / relative).lexically_normal();
    const std::filesystem::path relative_target =
        normalized_target.lexically_relative(normalized_base);
    if (relative_target.empty() || relative_target == "." || relative_target.has_root_path()) {
        return false;
    }
    for (const std::filesystem::path& component : relative_target) {
        if (component == "..") {
            return false;
        }
    }

    target = normalized_target;
    return true;
}

std::filesystem::path native_jar_path(const Library& library, const std::string& classifier,
                                      const std::filesystem::path& libraries_dir) {
    // 旧式 downloads.classifiers 优先；否则按 Maven 坐标 + classifier 推导
    const auto it = library.classifiers.find(classifier);
    if (it != library.classifiers.end() && !it->second.path.empty()) {
        return libraries_dir / it->second.path;
    }
    const std::string relative = library_relative_path(library.name + ":" + classifier);
    if (relative.empty()) {
        return {};
    }
    return libraries_dir / relative;
}

}  // namespace

util::Result<void> extract_natives(const std::vector<Library>& libraries,
                                   const std::filesystem::path& libraries_dir,
                                   const std::string& os_name, bool is_64bit,
                                   const std::filesystem::path& natives_dir) {
    // 先清空目录（§3 流程）
    std::error_code ec;
    std::filesystem::remove_all(natives_dir, ec);
    util::Result<void> created = platform::create_directories(natives_dir);
    if (created.is_error()) {
        return created;
    }

    for (const Library& library : libraries) {
        const std::string classifier = select_native_classifier(library, os_name, is_64bit);
        if (classifier.empty()) {
            continue;
        }
        const std::filesystem::path jar = native_jar_path(library, classifier, libraries_dir);
        if (jar.empty()) {
            continue;
        }

        util::Result<std::string> bytes = platform::read_file(jar);
        if (bytes.is_error()) {
            return util::Result<void>::fail(util::Error::make(
                "natives.jar_missing", "natives 依赖缺失：" + library.name,
                util::path_to_utf8(jar)));
        }
        const std::string& data = bytes.value();

        mz_zip_archive zip{};
        if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) {
            return util::Result<void>::fail(util::Error::make(
                "natives.open_failed", "无法读取 natives 包：" + library.name,
                util::path_to_utf8(jar)));
        }

        const unsigned count = mz_zip_reader_get_num_files(&zip);
        for (unsigned i = 0; i < count; ++i) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
                continue;
            }
            const std::string entry(stat.m_filename);  // NUL 结尾（miniz 3.x 无 m_filename_size）
            if (entry.empty() || entry.back() == '/' || stat.m_is_directory) {
                continue;
            }
            if (has_suffix(entry, ".sha1") || has_suffix(entry, ".git")) {
                continue;
            }
            std::vector<std::string> excludes;
            if (library.has_extract) {
                excludes = library.extract.exclude;
            }
            if (is_excluded(entry, excludes)) {
                continue;
            }

            std::filesystem::path target;
            if (!resolve_entry_target(natives_dir, entry, target)) {
                mz_zip_reader_end(&zip);
                return util::Result<void>::fail(util::Error::make(
                    "natives.unsafe_path", "natives 包包含不安全路径：" + library.name, entry));
            }
            util::Result<void> parent_created =
                platform::create_directories(target.parent_path());
            if (parent_created.is_error()) {
                mz_zip_reader_end(&zip);
                return parent_created;
            }

            // 已存在且大小一致 → 跳过（§3）
            std::error_code file_ec;
            if (std::filesystem::is_regular_file(target, file_ec) &&
                std::filesystem::file_size(target, file_ec) == stat.m_uncomp_size) {
                continue;
            }

            std::vector<char> buffer(static_cast<std::size_t>(stat.m_uncomp_size));
            if (!mz_zip_reader_extract_to_mem(&zip, i, buffer.data(), buffer.size(), 0)) {
                mz_zip_reader_end(&zip);
                return util::Result<void>::fail(util::Error::make(
                    "natives.extract_failed", "natives 解压失败：" + library.name, entry));
            }
            util::Result<void> written =
                platform::write_file(target, std::string_view(buffer.data(), buffer.size()));
            if (written.is_error()) {
                mz_zip_reader_end(&zip);
                return written;
            }
        }
        mz_zip_reader_end(&zip);
        util::log_debug("[natives] extracted " + library.name);
    }
    return util::Result<void>::ok();
}

}  // namespace sscl::core
