#pragma once

// 路径工具（开发文档 §4.3/§9.1：数据目录为 EXE 同目录 SSCL\，不写 %APPDATA%、不回退）。

#include <filesystem>

namespace sscl::platform {

// 可执行文件绝对路径；失败返回空路径。
std::filesystem::path exe_path();

// EXE 所在目录；失败返回空路径。
std::filesystem::path exe_dir();

// 数据目录：EXE 同目录下的 SSCL\。
std::filesystem::path data_dir();

}  // namespace sscl::platform
