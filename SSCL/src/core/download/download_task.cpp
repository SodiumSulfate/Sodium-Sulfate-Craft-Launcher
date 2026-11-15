#include "core/download/download_task.hpp"

#include <fstream>
#include <utility>

#include "platform/file_utils.hpp"
#include "platform/http_client.hpp"
#include "platform/win_error.hpp"

namespace sscl::core {

DownloadTask::DownloadTask(Params params) : m_params(std::move(params)) {
    set_name(display_name());
}

std::string DownloadTask::display_name() const {
    if (!m_params.display_name.empty()) {
        return m_params.display_name;
    }
    return sscl::util::wide_to_utf8(m_params.destination.filename().wstring());
}

util::Result<void> DownloadTask::do_run() {
    platform::HttpRequest request;
    request.method = "GET";
    request.url = m_params.url;

    util::Result<platform::HttpStream> opened = platform::open_http_stream(request);
    if (opened.is_error()) {
        return util::Result<void>::fail(std::move(opened.error()));
    }
    platform::HttpStream stream = std::move(opened.value());

    if (stream.status_code() != 200 && stream.status_code() != 206) {
        return util::Result<void>::fail(util::Error::make(
            "download.http_status", "下载失败：服务器返回 " + std::to_string(stream.status_code()),
            m_params.url));
    }

    const std::filesystem::path destination = m_params.destination;
    if (!destination.parent_path().empty()) {
        util::Result<void> created = platform::create_directories(destination.parent_path());
        if (created.is_error()) {
            return created;
        }
    }

    std::filesystem::path temp = destination;
    temp += L".tmp-" + std::to_wstring(::GetCurrentProcessId());

    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
        return util::Result<void>::fail(util::Error::make(
            "download.write_failed", "无法创建临时文件",
            sscl::util::wide_to_utf8(temp.wstring())));
    }

    const std::int64_t total = stream.content_length();
    std::uint64_t downloaded = 0;
    char buffer[64 * 1024];
    for (;;) {
        if (is_cancelled()) {
            out.close();
            ::DeleteFileW(temp.c_str());
            return util::Result<void>::fail(cancelled_error());
        }

        util::Result<std::size_t> read = stream.read(buffer, sizeof(buffer));
        if (read.is_error()) {
            out.close();
            ::DeleteFileW(temp.c_str());
            return util::Result<void>::fail(std::move(read.error()));
        }
        if (read.value() == 0) {
            break;
        }

        out.write(buffer, static_cast<std::streamsize>(read.value()));
        if (!out.good()) {
            out.close();
            ::DeleteFileW(temp.c_str());
            return util::Result<void>::fail(util::Error::make(
                "download.write_failed", "写入临时文件失败",
                sscl::util::wide_to_utf8(temp.wstring())));
        }

        downloaded += read.value();
        set_progress(downloaded, total > 0 ? static_cast<std::uint64_t>(total) : 0,
                     display_name());
    }
    out.close();

    // 哈希校验（§7.4：优先 SHA1；MD5 兼容老清单）
    if (!m_params.expected_hash_hex.empty()) {
        util::Result<std::string> actual =
            util::hash_file_hex(m_params.hash_algorithm, temp);
        if (actual.is_error()) {
            ::DeleteFileW(temp.c_str());
            return util::Result<void>::fail(std::move(actual.error()));
        }
        if (!util::hash_hex_equal(actual.value(), m_params.expected_hash_hex)) {
            ::DeleteFileW(temp.c_str());
            return util::Result<void>::fail(util::Error::make(
                "download.hash_mismatch",
                "文件校验失败，已丢弃：" + display_name(),
                m_params.url + "\nexpected: " + m_params.expected_hash_hex +
                    "\nactual: " + actual.value()));
        }
    }

    if (!::MoveFileExW(temp.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const unsigned long err = ::GetLastError();
        ::DeleteFileW(temp.c_str());
        return util::Result<void>::fail(
            util::Error::make("download.write_failed", "无法保存文件：" + display_name(),
                              sscl::platform::format_win_error(err)));
    }

    set_progress(downloaded, downloaded, display_name());
    return util::Result<void>::ok();
}

}  // namespace sscl::core
