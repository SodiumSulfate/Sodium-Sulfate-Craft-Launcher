#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "core/java/java_manager.hpp"

namespace core = sscl::core;

namespace {

core::JavaRuntime make_runtime(int major, const wchar_t* executable) {
    core::JavaRuntime runtime;
    runtime.executable = executable;
    runtime.version.major_version = major;
    runtime.version.version_string = std::to_string(major);
    return runtime;
}

core::JavaVersionInfo requirement(int major) {
    core::JavaVersionInfo required;
    required.component = "java-runtime";
    required.major_version = major;
    return required;
}

}  // namespace

TEST_CASE("java.parse: Oracle 1.8 与 OpenJDK 现代版本") {
    auto oracle = core::parse_java_version(
        "java version \"1.8.0_402\"\n"
        "Java(TM) SE Runtime Environment (build 1.8.0_402-b06)\n");
    REQUIRE(oracle.is_ok());
    CHECK(oracle.value().major_version == 8);
    CHECK(oracle.value().version_string == "1.8.0_402");

    auto openjdk = core::parse_java_version(
        "openjdk version \"17.0.10\" 2024-01-16\n"
        "OpenJDK Runtime Environment Temurin-17.0.10+7\n");
    REQUIRE(openjdk.is_ok());
    CHECK(openjdk.value().major_version == 17);
    CHECK(openjdk.value().version_string == "17.0.10");

    auto modern = core::parse_java_version("openjdk version \"21\" 2023-09-19\n");
    REQUIRE(modern.is_ok());
    CHECK(modern.value().major_version == 21);

    auto early_access = core::parse_java_version("openjdk version \"22-ea\" 2024-03-19\n");
    REQUIRE(early_access.is_ok());
    CHECK(early_access.value().major_version == 22);
}

TEST_CASE("java.parse: 拒绝无版本或非法版本输出") {
    CHECK(core::parse_java_version("OpenJDK Runtime Environment").is_error());
    CHECK(core::parse_java_version("openjdk version \"1.x\"").is_error());
    CHECK(core::parse_java_version("openjdk version \"17oops\"").is_error());
    CHECK(core::parse_java_version("").is_error());
}

TEST_CASE("java.select: 完全匹配优先于更高版本") {
    const std::vector<core::JavaRuntime> runtimes = {
        make_runtime(21, L"C:\\Java\\21\\bin\\java.exe"),
        make_runtime(17, L"C:\\Java\\17\\bin\\java.exe"),
        make_runtime(22, L"C:\\Java\\22\\bin\\java.exe"),
    };

    auto selected = core::select_java_runtime(runtimes, requirement(17));
    REQUIRE(selected.is_ok());
    CHECK(selected.value().match_kind == core::JavaMatchKind::kExact);
    CHECK(selected.value().runtime.executable == runtimes[1].executable);
}

TEST_CASE("java.select: 无完全匹配时选择最接近要求的更高主版本") {
    const std::vector<core::JavaRuntime> runtimes = {
        make_runtime(22, L"C:\\Java\\22\\bin\\java.exe"),
        make_runtime(21, L"C:\\Java\\21\\bin\\java.exe"),
        make_runtime(8, L"C:\\Java\\8\\bin\\java.exe"),
        make_runtime(18, L"C:\\Java\\18\\bin\\java.exe"),
    };

    auto selected = core::select_java_runtime(runtimes, requirement(17));
    REQUIRE(selected.is_ok());
    CHECK(selected.value().match_kind == core::JavaMatchKind::kCompatibleHigher);
    CHECK(selected.value().runtime.executable == runtimes[3].executable);
}

TEST_CASE("java.select: 同主版本保持发现顺序并忽略无效项") {
    const std::vector<core::JavaRuntime> runtimes = {
        make_runtime(0, L"C:\\Java\\invalid\\bin\\java.exe"),
        make_runtime(17, L"C:\\Java\\first\\bin\\java.exe"),
        make_runtime(17, L"C:\\Java\\second\\bin\\java.exe"),
    };

    auto selected = core::select_java_runtime(runtimes, requirement(17));
    REQUIRE(selected.is_ok());
    CHECK(selected.value().runtime.executable == runtimes[1].executable);
}

TEST_CASE("java.select: 低版本不满足要求且版本要求必须有效") {
    const std::vector<core::JavaRuntime> runtimes = {
        make_runtime(8, L"C:\\Java\\8\\bin\\java.exe"),
        make_runtime(16, L"C:\\Java\\16\\bin\\java.exe"),
    };

    auto missing = core::select_java_runtime(runtimes, requirement(17));
    REQUIRE(missing.is_error());
    CHECK(missing.error().code == "java.no_compatible_runtime");

    auto invalid = core::select_java_runtime(runtimes, requirement(0));
    REQUIRE(invalid.is_error());
    CHECK(invalid.error().code == "java.invalid_requirement");
}
