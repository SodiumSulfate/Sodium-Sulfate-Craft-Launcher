// util 模块单元测试（开发文档 §10：版本/参数/校验等核心逻辑必须有单元测试；
// 本文件覆盖 M0 批次 1 的 util：字符串转换、Result、日志脱敏）

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "util/log.hpp"
#include "util/result.hpp"
#include "util/string_utils.hpp"

TEST_CASE("util.string: UTF-8 / UTF-16 往返转换") {
    const std::string text = "你好, SSCL! café";
    const std::wstring wide = sscl::util::utf8_to_wide(text);
    CHECK(!wide.empty());
    CHECK(sscl::util::wide_to_utf8(wide) == text);

    CHECK(sscl::util::utf8_to_wide("").empty());
    CHECK(sscl::util::wide_to_utf8(L"").empty());
    CHECK(sscl::util::wide_to_utf8(L"ABC") == "ABC");
}

TEST_CASE("util.path: 中文路径 UTF-8 往返") {
    const std::filesystem::path path = L"C:\\测试\\游戏\\SSCL";
    const std::string utf8 = sscl::util::path_to_utf8(path);
    CHECK(utf8.find("测试") != std::string::npos);
    CHECK(sscl::util::path_from_utf8(utf8) == path);
}

TEST_CASE("util.string: trim / split / to_lower_ascii") {
    CHECK(sscl::util::trim("  a b \r\n") == "a b");
    CHECK(sscl::util::trim_left("  x ") == "x ");
    CHECK(sscl::util::trim_right(" y  ") == " y");
    CHECK(sscl::util::trim("") == "");

    const auto parts = sscl::util::split("a,,b,c", ',', false);
    REQUIRE(parts.size() == 3);
    CHECK(parts[0] == "a");
    CHECK(parts[1] == "b");
    CHECK(parts[2] == "c");

    const auto keep = sscl::util::split("a,,b", ',', true);
    REQUIRE(keep.size() == 3);
    CHECK(keep[1].empty());

    CHECK(sscl::util::to_lower_ascii("AbC-123") == "abc-123");
}

TEST_CASE("util.result: ok / fail") {
    auto ok = sscl::util::Result<int>::ok(42);
    CHECK(ok.is_ok());
    CHECK(ok.value() == 42);

    auto err = sscl::util::Result<int>::fail(
        sscl::util::Error::make("test.fail", "测试失败", "detail here"));
    CHECK(err.is_error());
    CHECK(err.error().code == "test.fail");
    CHECK(err.error().message == "测试失败");
    CHECK(err.error().detail == "detail here");

    auto void_ok = sscl::util::Result<void>::ok();
    CHECK(void_ok.is_ok());
    auto void_err = sscl::util::Result<void>::fail(sscl::util::Error::make("x", "y"));
    CHECK(void_err.is_error());
    CHECK(void_err.error().code == "x");
}

TEST_CASE("util.log: redact 脱敏敏感信息") {
    const std::string raw =
        R"({"access_token":"abc123","refresh_token":"def456"} Authorization: Bearer xyz789 x-api-key: key-999 password=hunter2)";
    const std::string safe = sscl::util::Logger::redact(raw);

    CHECK(safe.find("abc123") == std::string::npos);
    CHECK(safe.find("def456") == std::string::npos);
    CHECK(safe.find("xyz789") == std::string::npos);
    CHECK(safe.find("key-999") == std::string::npos);
    CHECK(safe.find("hunter2") == std::string::npos);
    CHECK(safe.find("***") != std::string::npos);
    // Bearer 前缀保留，凭据被屏蔽
    CHECK(safe.find("Bearer ***") != std::string::npos);
    // 普通文本不受影响
    CHECK(sscl::util::Logger::redact("download finished") == "download finished");
}
