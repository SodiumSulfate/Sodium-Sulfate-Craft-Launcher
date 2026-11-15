#include "platform/file_utils.hpp"

#include "platform/path_utils.hpp"
#include "platform/win_error.hpp"
#include "util/string_utils.hpp"

#include <algorithm>
#include <utility>

namespace sscl::platform {

namespace {

std::string path_utf8(const std::filesystem::path& path) {
    return sscl::util::path_to_utf8(path);
}

}  // namespace

bool file_exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

bool directory_exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

util::Result<std::string> read_file(const std::filesystem::path& path) {
    HANDLE handle =
        ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return util::Result<std::string>::fail(util::Error::make(
            "fs.read_failed", "无法读取文件：" + path_utf8(path),
            format_win_error(::GetLastError())));
    }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(handle, &size)) {
        const unsigned long err = ::GetLastError();
        ::CloseHandle(handle);
        return util::Result<std::string>::fail(
            util::Error::make("fs.read_failed", "无法读取文件：" + path_utf8(path),
                              format_win_error(err)));
    }
    if (size.QuadPart < 0 || size.QuadPart > 512LL * 1024 * 1024) {
        ::CloseHandle(handle);
        return util::Result<std::string>::fail(util::Error::make(
            "fs.too_large", "文件过大，暂不支持读取：" + path_utf8(path),
            std::to_string(size.QuadPart) + " bytes"));
    }

    std::string data(static_cast<size_t>(size.QuadPart), '\0');
    size_t total = 0;
    while (total < data.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(static_cast<size_t>(1024 * 1024), data.size() - total));
        DWORD read = 0;
        if (!::ReadFile(handle, data.data() + total, chunk, &read, nullptr)) {
            const unsigned long err = ::GetLastError();
            ::CloseHandle(handle);
            return util::Result<std::string>::fail(
                util::Error::make("fs.read_failed", "无法读取文件：" + path_utf8(path),
                                  format_win_error(err)));
        }
        if (read == 0) {
            break;
        }
        total += read;
    }
    ::CloseHandle(handle);
    data.resize(total);
    return util::Result<std::string>::ok(std::move(data));
}

util::Result<void> write_file(const std::filesystem::path& path, std::string_view data) {
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return util::Result<void>::fail(
            util::Error::make("fs.write_failed", "无法写入文件：" + path_utf8(path),
                              format_win_error(::GetLastError())));
    }

    size_t total = 0;
    while (total < data.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(static_cast<size_t>(1024 * 1024), data.size() - total));
        DWORD written = 0;
        if (!::WriteFile(handle, data.data() + total, chunk, &written, nullptr)) {
            const unsigned long err = ::GetLastError();
            ::CloseHandle(handle);
            return util::Result<void>::fail(
                util::Error::make("fs.write_failed", "无法写入文件：" + path_utf8(path),
                                  format_win_error(err)));
        }
        // D-086：WriteFile 可能返回 TRUE 但写入 0 字节（磁盘满/管道关闭等）。
        // 缺少该分支会让 total 永不增长 → 调用线程死循环挂死。
        if (written == 0) {
            ::CloseHandle(handle);
            return util::Result<void>::fail(util::Error::make(
                "fs.write_failed", "写入文件失败（0 字节）：" + path_utf8(path), {}));
        }
        total += written;
    }
    ::CloseHandle(handle);
    return util::Result<void>::ok();
}

util::Result<void> write_file_atomic(const std::filesystem::path& path, std::string_view data,
                                     bool flush_to_disk) {
    // 临时文件与目标同目录，保证 MoveFileEx 不跨卷
    std::filesystem::path temp = path;
    temp += L".tmp-" + std::to_wstring(::GetCurrentProcessId());

    util::Result<void> written = write_file(temp, data);
    if (written.is_error()) {
        return written;
    }

    // D-119：交互写入可不等磁盘落盘（仍有原子替换，只是不强制 flush）。
    const DWORD flags = MOVEFILE_REPLACE_EXISTING |
                        (flush_to_disk ? MOVEFILE_WRITE_THROUGH : 0UL);
    if (!::MoveFileExW(temp.c_str(), path.c_str(), flags)) {
        const unsigned long err = ::GetLastError();
        ::DeleteFileW(temp.c_str());
        return util::Result<void>::fail(
            util::Error::make("fs.write_failed", "无法替换文件：" + path_utf8(path),
                              format_win_error(err)));
    }
    return util::Result<void>::ok();
}

util::Result<void> create_directories(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return util::Result<void>::fail(util::Error::make(
            "fs.create_dir_failed", "无法创建目录：" + path_utf8(path), ec.message()));
    }
    return util::Result<void>::ok();
}

bool is_directory_writable(const std::filesystem::path& path) {
    if (!directory_exists(path)) {
        return false;
    }
    // 探针文件：写完自动删除
    const std::filesystem::path probe = path / L".sscl-write-probe";
    HANDLE handle = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                                  nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    ::CloseHandle(handle);
    return true;
}

util::Result<std::filesystem::path> ensure_data_dir() {
    const std::filesystem::path dir = data_dir();

    // 必须限定命名空间：不加限定会被 ADL 把 std::filesystem::create_directories
    // 混入重载集导致调用不明确。
    util::Result<void> created = ::sscl::platform::create_directories(dir);
    if (created.is_error()) {
        return util::Result<std::filesystem::path>::fail(std::move(created.error()));
    }
    if (!is_directory_writable(dir)) {
        // §4.3：不可写时不回退其他位置，直接提示用户
        return util::Result<std::filesystem::path>::fail(util::Error::make(
            "fs.unwritable",
            "无法写入 SSCL 数据目录，请将 SSCL.exe 移动到可写的位置后重试",
            path_utf8(dir)));
    }
    return util::Result<std::filesystem::path>::ok(dir);
}

}  // namespace sscl::platform
