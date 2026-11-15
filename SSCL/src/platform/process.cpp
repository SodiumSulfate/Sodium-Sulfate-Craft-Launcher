#include "platform/process.hpp"

#include "platform/win_error.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

namespace sscl::platform {

namespace {

// 子进程输出按系统 ANSI 代码页（如 GBK）转 UTF-8。
std::string ansi_to_utf8(const std::string& bytes) {
    if (bytes.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_ACP, 0, bytes.data(),
                                             static_cast<int>(bytes.size()), nullptr, 0);
    if (needed <= 0) {
        return bytes;
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(),
                          needed);
    return sscl::util::wide_to_utf8(wide);
}

std::wstring build_command_line(const std::filesystem::path& executable,
                                const std::vector<std::string>& arguments) {
    std::string line = quote_process_arg(sscl::util::wide_to_utf8(executable.wstring()));
    for (const std::string& argument : arguments) {
        line.push_back(' ');
        line += quote_process_arg(argument);
    }
    return sscl::util::utf8_to_wide(line);
}

// 一路输出的收集器：完整文本（保留尾部 max_capture_bytes）+ 待交付的整行队列。
struct StreamCollector {
    std::string text;
    std::string line_buffer;
};

struct OutputCollector {
    std::mutex mutex;
    StreamCollector out;
    StreamCollector err;
    std::vector<std::string> pending_lines;  // UTF-8，每项不含行尾
    std::size_t max_capture_bytes = 1u << 20;
    bool notify = false;  // 是否有 on_output

    void append(StreamCollector* stream, const char* data, std::size_t size) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (max_capture_bytes > 0) {
                stream->text.append(data, size);
                if (stream->text.size() > max_capture_bytes) {
                    stream->text.erase(0, stream->text.size() - max_capture_bytes);
                }
            }
            if (notify) {
                stream->line_buffer.append(data, size);
                // 逐行切分；行尾 \n（可能带 \r）作为分隔
                std::size_t start = 0;
                while (true) {
                    const std::size_t newline = stream->line_buffer.find('\n', start);
                    if (newline == std::string::npos) {
                        stream->line_buffer.erase(0, start);
                        break;
                    }
                    std::string line = stream->line_buffer.substr(start, newline - start);
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    pending_lines.push_back(ansi_to_utf8(line));
                    start = newline + 1;
                }
            }
        }
    }

    // 把待交付行移交给调用方线程（避免在读线程里回调用户代码）。
    std::vector<std::string> take_lines() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> lines;
        lines.swap(pending_lines);
        return lines;
    }
};

// 读尽管道直到 EOF；结束时关闭该句柄（句柄所有权转给线程）。
void read_pipe_all(HANDLE pipe, OutputCollector* collector, StreamCollector* stream) {
    char buffer[4096];
    for (;;) {
        DWORD read = 0;
        if (!::ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
            break;
        }
        collector->append(stream, buffer, static_cast<std::size_t>(read));
    }
    ::CloseHandle(pipe);
}

}  // namespace

std::string quote_process_arg(std::string_view arg) {
    // 无空格/引号/制表符且非空：原样返回
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos) {
        return std::string(arg);
    }

    std::string out;
    out.push_back('"');
    size_t backslashes = 0;
    for (const char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            // 引号前的反斜杠翻倍，再转义引号本身
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            backslashes = 0;
            continue;
        }
        if (backslashes > 0) {
            out.append(backslashes, '\\');
            backslashes = 0;
        }
        out.push_back(c);
    }
    if (backslashes > 0) {
        // 结尾反斜杠位于闭合引号前，需翻倍
        out.append(backslashes * 2, '\\');
    }
    out.push_back('"');
    return out;
}

util::Result<ProcessResult> run_process_ex(const std::filesystem::path& executable,
                                           const std::vector<std::string>& arguments,
                                           const std::filesystem::path& working_dir,
                                           const ProcessOptions& options) {
    if (executable.empty()) {
        return util::Result<ProcessResult>::fail(
            util::Error::make("process.create_failed", "可执行文件路径为空", {}));
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE out_read = nullptr;
    HANDLE out_write = nullptr;
    HANDLE err_read = nullptr;
    HANDLE err_write = nullptr;
    if (!::CreatePipe(&out_read, &out_write, &security, 0) ||
        !::CreatePipe(&err_read, &err_write, &security, 0)) {
        const unsigned long err = ::GetLastError();
        if (out_read) ::CloseHandle(out_read);
        if (out_write) ::CloseHandle(out_write);
        if (err_read) ::CloseHandle(err_read);
        if (err_write) ::CloseHandle(err_write);
        return util::Result<ProcessResult>::fail(
            util::Error::make("process.create_failed", "无法创建输出管道", format_win_error(err)));
    }
    // 父进程读取端不参与继承
    ::SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

    // 子进程 stdin 指向 NUL
    HANDLE null_in =
        ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &security, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = (null_in == INVALID_HANDLE_VALUE) ? nullptr : null_in;
    startup.hStdOutput = out_write;
    startup.hStdError = err_write;

    PROCESS_INFORMATION process{};
    std::wstring command_line = build_command_line(executable, arguments);
    const wchar_t* work_dir = working_dir.empty() ? nullptr : working_dir.c_str();

    const BOOL created =
        ::CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                         CREATE_NO_WINDOW, nullptr, work_dir, &startup, &process);

    // 父进程不再写管道
    ::CloseHandle(out_write);
    ::CloseHandle(err_write);
    if (null_in != INVALID_HANDLE_VALUE) {
        ::CloseHandle(null_in);
    }

    if (!created) {
        const unsigned long err = ::GetLastError();
        ::CloseHandle(out_read);
        ::CloseHandle(err_read);
        return util::Result<ProcessResult>::fail(util::Error::make(
            "process.create_failed", "无法启动进程", format_win_error(err)));
    }

    OutputCollector collector;
    collector.max_capture_bytes = options.max_capture_bytes;
    collector.notify = static_cast<bool>(options.on_output);

    std::thread out_reader(read_pipe_all, out_read, &collector, &collector.out);
    std::thread err_reader(read_pipe_all, err_read, &collector, &collector.err);

    bool timed_out = false;
    bool cancelled = false;
    const auto started = std::chrono::steady_clock::now();
    const auto deliver = [&collector, &options]() {
        if (!options.on_output) {
            return;
        }
        std::vector<std::string> lines = collector.take_lines();
        for (const std::string& line : lines) {
            options.on_output(line);
        }
    };

    for (;;) {
        const DWORD wait_result = ::WaitForSingleObject(process.hProcess, 50);
        deliver();
        if (wait_result == WAIT_OBJECT_0) {
            break;
        }
        if (options.stop.stop_requested()) {
            cancelled = true;
            ::TerminateProcess(process.hProcess, 1);
            ::WaitForSingleObject(process.hProcess, INFINITE);
            break;
        }
        if (options.timeout_ms >= 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            if (elapsed.count() >= options.timeout_ms) {
                timed_out = true;
                ::TerminateProcess(process.hProcess, 1);
                ::WaitForSingleObject(process.hProcess, INFINITE);
                break;
            }
        }
    }

    out_reader.join();
    err_reader.join();
    deliver();

    DWORD exit_code = 1;
    if (!::GetExitCodeProcess(process.hProcess, &exit_code)) {
        exit_code = 1;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);

    ProcessResult result;
    result.exit_code = static_cast<int>(exit_code);
    {
        std::lock_guard<std::mutex> lock(collector.mutex);
        result.stdout_text = ansi_to_utf8(collector.out.text);
        result.stderr_text = ansi_to_utf8(collector.err.text);
    }

    if (cancelled) {
        return util::Result<ProcessResult>::fail(util::Error::make(
            "process.cancelled", "进程执行已取消",
            "stdout: " + result.stdout_text + "\nstderr: " + result.stderr_text));
    }
    if (timed_out) {
        return util::Result<ProcessResult>::fail(util::Error::make(
            "process.timeout", "进程执行超时，已强制结束",
            "stdout: " + result.stdout_text + "\nstderr: " + result.stderr_text));
    }
    return util::Result<ProcessResult>::ok(std::move(result));
}

util::Result<ProcessResult> run_process(const std::filesystem::path& executable,
                                        const std::vector<std::string>& arguments,
                                        const std::filesystem::path& working_dir,
                                        int timeout_ms) {
    ProcessOptions options;
    options.timeout_ms = timeout_ms;
    return run_process_ex(executable, arguments, working_dir, options);
}

}  // namespace sscl::platform
