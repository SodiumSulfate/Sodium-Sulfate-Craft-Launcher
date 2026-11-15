// platform 模块单元测试（M0 批次 2）：
// 测试临时文件写在测试程序所在目录的 unittest_tmp\ 下（属 SSCL 树内构建输出区，
// 遵守开发文档 §1：不写入 %TEMP% / 用户目录）。

#include <doctest/doctest.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <filesystem>
#include <string>

#include "platform/file_utils.hpp"
#include "platform/path_utils.hpp"
#include "platform/process.hpp"
#include "platform/system_monitor.hpp"

namespace {

std::filesystem::path test_scratch_dir() {
    const std::filesystem::path dir = sscl::platform::exe_dir() / L"unittest_tmp";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

}  // namespace

TEST_CASE("platform.file: 写入/读回/原子写") {
    const std::filesystem::path file = test_scratch_dir() / L"roundtrip.txt";

    REQUIRE(sscl::platform::write_file(file, "hello 世界").is_ok());
    auto read = sscl::platform::read_file(file);
    REQUIRE(read.is_ok());
    CHECK(read.value() == "hello 世界");

    REQUIRE(sscl::platform::write_file_atomic(file, "第二版").is_ok());
    read = sscl::platform::read_file(file);
    REQUIRE(read.is_ok());
    CHECK(read.value() == "第二版");
    CHECK(sscl::platform::file_exists(file));

    auto missing = sscl::platform::read_file(test_scratch_dir() / L"no-such-file.bin");
    CHECK(missing.is_error());
    CHECK(missing.error().code == "fs.read_failed");
}

TEST_CASE("platform.file: 目录创建与可写探测") {
    const std::filesystem::path dir = test_scratch_dir() / L"sub" / L"deep";
    REQUIRE(sscl::platform::create_directories(dir).is_ok());
    CHECK(sscl::platform::directory_exists(dir));
    CHECK(sscl::platform::is_directory_writable(dir));
    CHECK(!sscl::platform::is_directory_writable(dir / L"not-exist"));
}

TEST_CASE("platform.path: exe_dir / data_dir") {
    CHECK(!sscl::platform::exe_path().empty());
    CHECK(sscl::platform::data_dir().filename() == L"SSCL");
}

TEST_CASE("platform.process: quote_process_arg") {
    CHECK(sscl::platform::quote_process_arg("abc") == "abc");
    CHECK(sscl::platform::quote_process_arg("a b") == "\"a b\"");
    CHECK(sscl::platform::quote_process_arg("") == "\"\"");
    CHECK(sscl::platform::quote_process_arg("a\"b") == "\"a\\\"b\"");
    CHECK(sscl::platform::quote_process_arg("a b\\") == "\"a b\\\\\"");
}

TEST_CASE("platform.process: run_process 捕获退出码") {
    // 使用 System32 绝对路径，消除 CreateProcess 对可执行名的搜索差异；
    // 失败时 MESSAGE 打印错误码/详情，便于诊断（REQUIRE 只报 false）。
    wchar_t sys_dir[MAX_PATH] = {};
    ::GetSystemDirectoryW(sys_dir, MAX_PATH);
    const std::filesystem::path cmd = std::filesystem::path(sys_dir) / L"cmd.exe";

    auto result = sscl::platform::run_process(cmd, {"/c", "exit 3"}, {}, 15000);
    if (result.is_error()) {
        MESSAGE("run_process failed: code=", result.error().code,
                " message=", result.error().message, " detail=", result.error().detail);
    }
    REQUIRE(result.is_ok());
    CHECK(result.value().exit_code == 3);

    auto echo = sscl::platform::run_process(cmd, {"/c", "echo hello"}, {}, 15000);
    if (echo.is_error()) {
        MESSAGE("run_process failed: code=", echo.error().code,
                " message=", echo.error().message, " detail=", echo.error().detail);
    }
    REQUIRE(echo.is_ok());
    CHECK(echo.value().stdout_text.find("hello") != std::string::npos);
}

TEST_CASE("platform.system_monitor: CPU/内存采样在合法范围") {
    sscl::platform::SystemMonitor monitor;
    const sscl::platform::SystemLoad load = monitor.sample();
    CHECK(load.cpu_percent >= 0.0);
    CHECK(load.cpu_percent <= 100.0);
    CHECK(load.memory_total_bytes > 0);
    CHECK(load.memory_used_bytes <= load.memory_total_bytes);
    CHECK(load.memory_percent >= 0.0);
    CHECK(load.memory_percent <= 100.0);
}
