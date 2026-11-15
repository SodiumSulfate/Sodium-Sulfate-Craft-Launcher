#pragma once

// Java 运行时自动下载（D-091，P0：新机/1.20.5+ 无 Java 时无法启动）。
//
// 数据源：Adoptium Temurin JRE（Windows x64，zip），按主版本取 latest GA：
//   https://api.adoptium.net/v3/binary/latest/{major}/ga/windows/x64/jre/hotspot/normal/eclipse
// 可用环境变量 SSCL_JAVA_URL_TEMPLATE 覆盖（`{major}` 占位），便于国内镜像/离线部署。
//
// 落盘：<runtime_root>/jre-<major>/bin/java.exe；已存在则直接复用，不重复下载。
// 校验：解压后 `bin/java.exe` 必须存在，并由调用方用 java -version 复核主版本。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

#include "core/download/download_scheduler.hpp"
#include "util/result.hpp"

namespace sscl::core {

struct JavaDownloadOptions {
    std::filesystem::path runtime_root;              // 通常 <data>/runtime
    std::string url_template;                        // 空 = 内置 Adoptium 模板
    std::int64_t max_archive_bytes = 300LL * 1024 * 1024;  // 防御异常大包
};

using JavaDownloadProgressFn = std::function<void(std::uint64_t, std::uint64_t)>;

// 确保某个主版本的 JRE 可用；返回 JRE 根目录（含 bin/java.exe）。
// 失败：java.download_failed / java.extract_failed / java.unavailable。
util::Result<std::filesystem::path> ensure_java_runtime(
    int major_version, const JavaDownloadOptions& options, DownloadScheduler& scheduler,
    const JavaDownloadProgressFn& progress = nullptr, std::stop_token stop = {});

// 已存在的 JRE 根目录（不联网）；无则返回空路径。
std::filesystem::path find_installed_java_runtime(const std::filesystem::path& runtime_root,
                                                  int major_version);

}  // namespace sscl::core
