#include "platform/zip_utils.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include <miniz/miniz.h>

#include "platform/file_utils.hpp"
#include "util/string_utils.hpp"

#pragma comment(lib, "miniz.lib")

namespace sscl::platform {

namespace {

// 条目名 → 目标相对路径；不安全返回 false（绝对路径/盘符/`.`/`..`/空段）。
bool safe_relative_entry(const std::string& entry, std::filesystem::path* out) {
    if (entry.empty()) {
        return false;
    }
    std::string normalized = entry;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.front() == '/') {
        return false;
    }
    // 形如 "C:/..." 或 "C:" 的盘符路径
    if (normalized.size() >= 2 && normalized[1] == ':') {
        return false;
    }
    std::filesystem::path relative;
    std::string component;
    for (std::size_t index = 0; index <= normalized.size(); ++index) {
        if (index == normalized.size() || normalized[index] == '/') {
            if (component.empty() || component == "." || component == "..") {
                return false;
            }
            relative /= sscl::util::path_from_utf8(component);
            component.clear();
            continue;
        }
        component.push_back(normalized[index]);
    }
    if (relative.empty()) {
        return false;
    }
    *out = std::move(relative);
    return true;
}

util::Result<void> open_archive(std::string_view bytes, mz_zip_archive* zip) {
    if (bytes.empty()) {
        return util::Result<void>::fail(
            util::Error::make("zip.empty", "ZIP 内容为空", {}));
    }
    if (!mz_zip_reader_init_mem(zip, bytes.data(), bytes.size(), 0)) {
        return util::Result<void>::fail(
            util::Error::make("zip.open_failed", "无法读取 ZIP 归档", {}));
    }
    return util::Result<void>::ok();
}

struct ZipCloser {
    mz_zip_archive* zip = nullptr;
    ~ZipCloser() {
        if (zip != nullptr) {
            mz_zip_reader_end(zip);
        }
    }
};

util::Result<std::vector<std::string>> entry_names(mz_zip_archive* zip) {
    std::vector<std::string> names;
    const mz_uint count = mz_zip_reader_get_num_files(zip);
    names.reserve(count);
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(zip, index, &stat)) {
            continue;
        }
        if (stat.m_is_directory) {
            continue;
        }
        std::string name(stat.m_filename);
        if (name.empty() || name.back() == '/') {
            continue;
        }
        names.push_back(std::move(name));
    }
    return util::Result<std::vector<std::string>>::ok(std::move(names));
}

util::Result<std::string> read_entry(mz_zip_archive* zip, const std::string& entry) {
    const int index = mz_zip_reader_locate_file(zip, entry.c_str(), nullptr, 0);
    if (index < 0) {
        return util::Result<std::string>::fail(util::Error::make(
            "zip.entry_missing", "ZIP 中缺少必需条目：" + entry, {}));
    }
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(zip, static_cast<mz_uint>(index), &stat)) {
        return util::Result<std::string>::fail(util::Error::make(
            "zip.entry_missing", "无法读取 ZIP 条目信息：" + entry, {}));
    }
    if (stat.m_is_directory) {
        return util::Result<std::string>::fail(util::Error::make(
            "zip.entry_missing", "ZIP 条目是目录：" + entry, {}));
    }
    // 防御：单条目上限 256 MiB（安装器内嵌件通常 < 20 MiB）
    constexpr mz_uint64 kMaxEntryBytes = 256ull * 1024 * 1024;
    if (stat.m_uncomp_size > kMaxEntryBytes) {
        return util::Result<std::string>::fail(util::Error::make(
            "zip.entry_too_large", "ZIP 条目过大：" + entry, {}));
    }
    std::string data(static_cast<std::size_t>(stat.m_uncomp_size), '\0');
    if (!data.empty() &&
        !mz_zip_reader_extract_to_mem(zip, static_cast<mz_uint>(index), data.data(),
                                      data.size(), 0)) {
        return util::Result<std::string>::fail(util::Error::make(
            "zip.extract_failed", "ZIP 条目解压失败：" + entry, {}));
    }
    return util::Result<std::string>::ok(std::move(data));
}

util::Result<std::size_t> extract_subtree(mz_zip_archive* zip, const std::string& prefix,
                                          const std::filesystem::path& dest_dir) {
    std::string normalized_prefix = prefix;
    std::replace(normalized_prefix.begin(), normalized_prefix.end(), '\\', '/');
    if (!normalized_prefix.empty() && normalized_prefix.back() != '/') {
        normalized_prefix.push_back('/');
    }

    std::size_t written = 0;
    const mz_uint count = mz_zip_reader_get_num_files(zip);
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(zip, index, &stat)) {
            continue;
        }
        if (stat.m_is_directory) {
            continue;
        }
        std::string name(stat.m_filename);
        std::string normalized = name;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        // 前缀匹配后**剥掉前缀**再落盘：zip_extract_subtree(archive, "maven/", libraries)
        // 应得到 libraries/net/minecraftforge/...（而非 libraries/maven/net/...）。
        std::string relative_name = normalized;
        if (!normalized_prefix.empty()) {
            if (normalized.size() < normalized_prefix.size() ||
                normalized.compare(0, normalized_prefix.size(), normalized_prefix) != 0) {
                continue;
            }
            relative_name = normalized.substr(normalized_prefix.size());
        }
        std::filesystem::path relative;
        if (!safe_relative_entry(relative_name, &relative)) {
            return util::Result<std::size_t>::fail(util::Error::make(
                "zip.unsafe_path", "ZIP 包含不安全路径：" + normalized, {}));
        }
        const std::filesystem::path target = dest_dir / relative;
        // 限定命名空间：std::filesystem::path 参与 ADL 会同时找到 std::filesystem::create_directories
        // （与 platform::create_directories 构成重载歧义，C2668）。
        util::Result<void> created = ::sscl::platform::create_directories(target.parent_path());
        if (created.is_error()) {
            return util::Result<std::size_t>::fail(created.error());
        }
        std::error_code ec;
        if (std::filesystem::is_regular_file(target, ec) &&
            std::filesystem::file_size(target, ec) == stat.m_uncomp_size) {
            continue;  // 幂等：大小一致视为已就绪
        }
        util::Result<std::string> data = read_entry(zip, name);
        if (data.is_error()) {
            return util::Result<std::size_t>::fail(data.error());
        }
        const std::string& bytes = data.value();
        util::Result<void> wrote = write_file(target, bytes);
        if (wrote.is_error()) {
            return util::Result<std::size_t>::fail(wrote.error());
        }
        ++written;
    }
    return util::Result<std::size_t>::ok(written);
}

}  // namespace

util::Result<std::vector<std::string>> zip_entry_names_from_memory(std::string_view bytes) {
    mz_zip_archive zip{};
    util::Result<void> opened = open_archive(bytes, &zip);
    if (opened.is_error()) {
        return util::Result<std::vector<std::string>>::fail(opened.error());
    }
    ZipCloser closer{&zip};
    return entry_names(&zip);
}

util::Result<std::vector<std::string>> zip_entry_names(const std::filesystem::path& archive) {
    util::Result<std::string> bytes = read_file(archive);
    if (bytes.is_error()) {
        return util::Result<std::vector<std::string>>::fail(bytes.error());
    }
    return zip_entry_names_from_memory(bytes.value());
}

util::Result<std::string> zip_read_entry_from_memory(std::string_view bytes,
                                                     const std::string& entry) {
    mz_zip_archive zip{};
    util::Result<void> opened = open_archive(bytes, &zip);
    if (opened.is_error()) {
        return util::Result<std::string>::fail(opened.error());
    }
    ZipCloser closer{&zip};
    return read_entry(&zip, entry);
}

util::Result<std::string> zip_read_entry(const std::filesystem::path& archive,
                                         const std::string& entry) {
    util::Result<std::string> bytes = read_file(archive);
    if (bytes.is_error()) {
        return util::Result<std::string>::fail(bytes.error());
    }
    return zip_read_entry_from_memory(bytes.value(), entry);
}

util::Result<std::size_t> zip_extract_subtree_from_memory(std::string_view bytes,
                                                          const std::string& prefix,
                                                          const std::filesystem::path& dest_dir) {
    mz_zip_archive zip{};
    util::Result<void> opened = open_archive(bytes, &zip);
    if (opened.is_error()) {
        return util::Result<std::size_t>::fail(opened.error());
    }
    ZipCloser closer{&zip};
    return extract_subtree(&zip, prefix, dest_dir);
}

util::Result<std::size_t> zip_extract_subtree(const std::filesystem::path& archive,
                                              const std::string& prefix,
                                              const std::filesystem::path& dest_dir) {
    util::Result<std::string> bytes = read_file(archive);
    if (bytes.is_error()) {
        return util::Result<std::size_t>::fail(bytes.error());
    }
    return zip_extract_subtree_from_memory(bytes.value(), prefix, dest_dir);
}

util::Result<void> zip_write_files(const std::filesystem::path& archive,
                                   const std::vector<ZipWriteEntry>& entries) {
    if (archive.empty()) {
        return util::Result<void>::fail(
            util::Error::make("zip.invalid_path", "ZIP 输出路径为空", {}));
    }
    const std::string archive_path = sscl::util::path_to_utf8(archive);
    mz_zip_archive zip{};
    // 先写内存再原子落盘：miniz 的文件 API 只接受 ANSI 路径，中文目录会失败；
    // 同时避免写一半的 ZIP 留在磁盘上。
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
        return util::Result<void>::fail(
            util::Error::make("zip.create_failed", "无法创建 ZIP 文件", archive_path));
    }
    for (const ZipWriteEntry& entry : entries) {
        if (entry.archive_path.empty() || entry.file.empty()) {
            continue;
        }
        util::Result<std::string> bytes = read_file(entry.file);
        if (bytes.is_error()) {
            mz_zip_writer_end(&zip);
            return util::Result<void>::fail(std::move(bytes.error()));
        }
        if (!mz_zip_writer_add_mem(&zip, entry.archive_path.c_str(), bytes.value().data(),
                                   bytes.value().size(), MZ_DEFAULT_COMPRESSION)) {
            mz_zip_writer_end(&zip);
            return util::Result<void>::fail(util::Error::make(
                "zip.write_failed", "写入 ZIP 条目失败", entry.archive_path));
        }
    }
    void* buffer = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size)) {
        mz_zip_writer_end(&zip);
        return util::Result<void>::fail(
            util::Error::make("zip.write_failed", "无法完成 ZIP 文件", archive_path));
    }
    mz_zip_writer_end(&zip);
    std::string_view data;
    if (buffer != nullptr && size > 0) {
        data = std::string_view(static_cast<const char*>(buffer), size);
    }
    util::Result<void> written = write_file_atomic(archive, data);
    mz_free(buffer);
    return written;
}

}  // namespace sscl::platform
