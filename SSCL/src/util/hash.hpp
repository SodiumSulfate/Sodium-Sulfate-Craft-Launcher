#pragma once

// 哈希（开发文档 §4.2：系统 BCrypt，SHA1/SHA256/MD5）。
// 下载校验优先 SHA1；老版本清单/镜像可能给 MD5（knowledge/download-mirrors.md §2）。

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "util/result.hpp"

namespace sscl::util {

enum class HashAlgorithm {
    kSha1,
    kSha256,
    kMd5,
};

// 内存数据摘要，返回小写十六进制。
std::string hash_hex(HashAlgorithm algorithm, std::string_view data);

// 流式计算文件摘要（不限文件大小），返回小写十六进制。
Result<std::string> hash_file_hex(HashAlgorithm algorithm, const std::filesystem::path& path);

// CurseForge 指纹：MurmurHash2(seed=1)，先剔除 0x09/0x0A/0x0D/0x20。
std::uint32_t murmurhash2_filtered(std::string_view data);
Result<std::uint32_t> curseforge_fingerprint(const std::filesystem::path& path);

// 十六进制比较（不区分大小写）。
bool hash_hex_equal(std::string_view a, std::string_view b);

}  // namespace sscl::util
