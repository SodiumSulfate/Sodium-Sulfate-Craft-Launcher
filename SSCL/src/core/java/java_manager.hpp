#pragma once

// Java 运行时检测、版本探测与 Minecraft javaVersion 匹配。
// 检测仅读取环境变量、文件系统和 HKLM 注册表，不写入任何外部位置。

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/launch/version_json.hpp"
#include "util/result.hpp"

namespace sscl::core {

enum class JavaSource {
    kManual,
    kPortableRuntime,
    kJavaHome,
    kPath,
    kCommonDirectory,
    kRegistry,
};

struct JavaVersion {
    int major_version = 0;
    std::string version_string;  // 引号内的原始版本，如 "1.8.0_402" 或 "17.0.10"
};

struct JavaCandidate {
    std::filesystem::path executable;
    JavaSource source = JavaSource::kManual;
};

struct JavaRuntime {
    std::filesystem::path executable;
    JavaSource source = JavaSource::kManual;
    JavaVersion version;
};

enum class JavaMatchKind {
    kExact,
    kCompatibleHigher,
};

struct JavaSelection {
    JavaRuntime runtime;
    JavaMatchKind match_kind = JavaMatchKind::kExact;
};

// 从 Oracle/OpenJDK 的 java -version 输出提取主版本；兼容 1.x 与现代版本号。
util::Result<JavaVersion> parse_java_version(std::string_view version_output);

// 纯选择函数：先选主版本完全匹配者；没有则选主版本最接近要求的更高版本。
// 同主版本保持输入顺序，主版本无效的运行时会被忽略。
util::Result<JavaSelection> select_java_runtime(const std::vector<JavaRuntime>& runtimes,
                                                const JavaVersionInfo& required);

// 枚举现有 java.exe 并按 Windows 路径语义去重，不启动 Java。
// 顺序即选择时的同版本优先级：便携运行时、JAVA_HOME、PATH、常见目录、注册表、附加目录（如 D:\JDK）。
std::vector<JavaCandidate> enumerate_java_candidates();
std::vector<JavaCandidate> enumerate_java_candidates(
    const std::vector<std::filesystem::path>& extra_roots);

// 对候选运行时执行 java -version（默认最多 5 秒）并解析输出。
util::Result<JavaRuntime> inspect_java_runtime(const JavaCandidate& candidate,
                                               int timeout_ms = 5000);

// 校验手工选择的 java.exe/javaw.exe、bin 目录或 Java 根目录，再获取其版本。
util::Result<JavaRuntime> validate_java_path(const std::filesystem::path& path,
                                             int timeout_ms = 5000);

// 检测全部候选；单个运行时不可用不会中断其他检测，失败项不会进入结果。
std::vector<JavaRuntime> detect_java_runtimes(int timeout_ms = 5000);

// D-115：只在给定目录内扫描 java.exe，返回其所在目录（大小写不敏感去重）。
// 这是设置页「扫描位置」的语义：刷新 Java 列表时**只**扫这些目录，
// 不再隐式扫描 JAVA_HOME / 注册表等其它位置。
std::vector<std::filesystem::path> scan_java_dirs(
    const std::vector<std::filesystem::path>& roots);

// D-115：首次启动自动加入的默认扫描位置 —— JAVA_HOME 的父目录、
// PATH 中首个 java.exe 的 JDK 根、以及常见安装目录。只返回实际存在的目录，
// 已按 Windows 路径语义（大小写不敏感 + 规范化）去重。
std::vector<std::filesystem::path> default_java_scan_dirs();

// D-115：扫描给定目录并对每个 java.exe 执行 java -version；失败项丢弃。
std::vector<JavaRuntime> detect_java_in_dirs(
    const std::vector<std::filesystem::path>& roots, int timeout_ms = 5000);
std::vector<JavaRuntime> detect_java_runtimes(
    const std::vector<std::filesystem::path>& extra_roots, int timeout_ms = 5000);

// 枚举、探测并选择适合 VersionJson.javaVersion 的运行时。
util::Result<JavaSelection> auto_select_java_runtime(const JavaVersionInfo& required,
                                                     int timeout_ms = 5000);
util::Result<JavaSelection> auto_select_java_runtime(
    const JavaVersionInfo& required, const std::vector<std::filesystem::path>& extra_roots,
    int timeout_ms = 5000);

}  // namespace sscl::core
