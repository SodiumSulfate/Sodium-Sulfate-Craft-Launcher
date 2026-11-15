#pragma once

// 文件工具：读/写/原子写（开发文档 §5.1：配置写临时文件后 MoveFileEx 替换）。

#include <filesystem>
#include <string>
#include <string_view>

#include "util/result.hpp"

namespace sscl::platform {

bool file_exists(const std::filesystem::path& path);
bool directory_exists(const std::filesystem::path& path);

// 读整个文件为字节串（文本按 UTF-8 约定解释，但不做校验）。
util::Result<std::string> read_file(const std::filesystem::path& path);

// 覆盖写入。
util::Result<void> write_file(const std::filesystem::path& path, std::string_view data);

// 原子写入：写同目录临时文件 → MoveFileEx(REPLACE_EXISTING|WRITE_THROUGH) 替换。
// flush_to_disk=false 时去掉 WRITE_THROUGH：**原子替换语义不变**，但不再等磁盘落盘。
// 供 UI 线程上的小型交互写入（accounts.dat / config.json）使用，避免按钮点按时
// 卡在强制刷盘上；下载/版本清单/密钥等写入保持默认（true）。
util::Result<void> write_file_atomic(const std::filesystem::path& path, std::string_view data,
                                     bool flush_to_disk = true);

// 递归创建目录。
util::Result<void> create_directories(const std::filesystem::path& path);

// 探测目录是否可写（创建即删探针文件）；目录不存在返回 false。
bool is_directory_writable(const std::filesystem::path& path);

// 创建并校验数据目录 SSCL\（§4.3：不可写时报错提示，绝不回退到其他位置）。
util::Result<std::filesystem::path> ensure_data_dir();

}  // namespace sscl::platform
