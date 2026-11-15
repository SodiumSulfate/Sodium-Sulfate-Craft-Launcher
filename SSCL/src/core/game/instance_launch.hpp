#pragma once

// 每版本启动设置合并（V5）：把 InstanceSettings 的启动相关字段转换为启动器可用的覆盖值。
// 纯函数，不访问磁盘；Java 选择由 GameService 按 mode/path/range 完成。

#include <string>

#include "core/game/instance_paths.hpp"

namespace sscl::core {

struct InstanceLaunchOverrides {
    int memory_mb = 0;
    int java_mode = 0;          // 0 自动 / 1 范围 / 2 版本文件夹 / 3 指定 Java
    std::string java_path;      // 仅 mode=3
    std::string java_range;     // 仅 mode=1
    std::string jvm_args;
    std::string game_args;
    int gc_mode = 0;
    std::string version_info;
    std::string window_title;
    std::string server_enter;
};

// global_memory_mb：全局启动设置内存；ram_type=0/2 时使用。
InstanceLaunchOverrides merge_instance_launch(const InstanceSettings& settings,
                                              int global_memory_mb);

// 解析 PCL Java 范围（如 "[17.0.1, 25.0)" / "[17, 25]"），返回主版本区间。
// 解析失败返回 false；min_major <= max_major；max_inclusive=false 表示开区间。
bool parse_java_range(const std::string& text, int* min_major, int* max_major,
                      bool* max_inclusive);

}  // namespace sscl::core
