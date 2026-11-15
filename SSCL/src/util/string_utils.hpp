#pragma once

// 字符串工具（开发文档 §4.1：内部字符串一律 UTF-8 std::string，
// 仅在调用 Win32 API 时转换为 UTF-16）。

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sscl::util {

// UTF-8 -> UTF-16。转换失败（非法序列）返回空串。
std::wstring utf8_to_wide(std::string_view utf8);

// UTF-16 -> UTF-8。转换失败（非法序列）返回空串。
std::string wide_to_utf8(std::wstring_view wide);

// 文件路径 -> UTF-8：内部字符串统一使用 UTF-8，禁止使用 path::string()
// （MSVC 的 path::string() 返回系统 ACP/ANSI，中文路径会乱码）。
std::string path_to_utf8(const std::filesystem::path& path);

// UTF-8 -> 文件路径：与 path_to_utf8 成对使用，避免 std::filesystem::path(utf8) 走 ACP。
std::filesystem::path path_from_utf8(std::string_view utf8);

// 去掉首尾 ASCII 空白（空格、\t、\r、\n、\v、\f）。
std::string trim(std::string_view text);
std::string trim_left(std::string_view text);
std::string trim_right(std::string_view text);

// 按单字符分隔符切分；keep_empty 为 false 时丢弃空段。
std::vector<std::string> split(std::string_view text, char delimiter, bool keep_empty = false);

// 转小写：仅处理 ASCII 字母（A-Z），不折叠 Unicode 大小写。
std::string to_lower_ascii(std::string_view text);

}  // namespace sscl::util
