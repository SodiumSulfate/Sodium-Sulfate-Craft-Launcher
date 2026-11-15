#pragma once

// 版本管理（PCL 2.99 对齐，V1）：
// - McFolderInfo：MC 文件夹列表（当前/官启/自定义）；
// - InstanceCard：版本列表的卡片分组；
// - 版本名合法性、分类/排序、文件夹扫描均为纯函数，便于单测。
// 依赖：instance_types（VersionSummary）、launcher_config（McFolderConfig）。

#include <filesystem>
#include <string>
#include <vector>

#include "core/game/instance_types.hpp"
#include "core/game/launcher_config.hpp"

namespace sscl::core {

enum class McFolderType {
    kVanilla = 0,
    kRenamedVanilla = 1,
    kCustom = 2,
};

struct McFolderInfo {
    std::string name;
    std::filesystem::path location;
    McFolderType type = McFolderType::kCustom;
    bool selected = false;
};

struct InstanceCard {
    InstanceCategory category = InstanceCategory::kAuto;
    std::string title;
    std::vector<VersionSummary> versions;
};

const char* instance_state_key(InstanceState state);
const char* instance_category_key(InstanceCategory category);

// 文件夹/重命名用的版本名校验（对齐 PCL ValidateFolderName 的常用规则）。
bool instance_name_is_valid(const std::string& name, std::string* reason);

// 按 PCL 卡片规则分组 + 排序（不访问磁盘）。
// show_hidden=true 时只返回“隐藏的版本”卡片；false 时排除隐藏版本。
std::vector<InstanceCard> classify_instance_summaries(std::vector<VersionSummary> versions,
                                                      bool show_hidden);

// 扫描 MC 文件夹列表（不创建目录）：
// 当前/base → base 的同级目录 → 官方目录 → 用户保存的列表；
// 同名路径去重；selected_path 匹配项 selected=true，否则第一项。
std::vector<McFolderInfo> scan_mc_folders(const std::filesystem::path& base_game_dir,
                                          const std::filesystem::path& official_game_dir,
                                          const std::vector<McFolderConfig>& saved,
                                          const std::string& selected_path);

}  // namespace sscl::core
