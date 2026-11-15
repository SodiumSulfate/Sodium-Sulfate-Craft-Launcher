#pragma once

// PCL 2.99 的 Mod 下载目标规划（D-059）：
// - “下载”= 另存为；这里只计算默认目录 + 文件名，不执行安装；
// - 默认目录：上次同类目录 → 当前选中兼容实例 run_dir/mods →
//   所有实例中兼容者按目录修改时间/文件数排序 → MC 根；
// - 文件名：PCL ToolDownloadTranslateV2 规则 + SSCL 安全兜底。
// 纯本地计算，便于单测；实际保存对话框/下载由 UI/DownloadScheduler 负责。

#include <filesystem>
#include <string>
#include <vector>

#include "core/game/instance_paths.hpp"
#include "core/modplatform/mod_types.hpp"

namespace sscl::core {

struct ModDownloadPlan {
    std::filesystem::path default_folder;
    std::string file_name;
    bool selected_instance_compatible = false;
};

// translate_mode：0=【中文名】原名，1=[中文名] 原名（PCL 默认），2=中文名-原名，
// 3=原名-中文名，其他=原名。translated_name 为空/等于原名时使用原名。
ModDownloadPlan plan_pcl_mod_download(const std::filesystem::path& game_dir,
                                      const InstanceTarget* selected,
                                      const std::vector<InstanceTarget>& instances,
                                      const std::string& cached_folder,
                                      const ModVersion& version, const std::string& raw_file_name,
                                      const std::string& translated_name = {},
                                      int translate_mode = 1);

// 版本是否满足目标实例的 MC/加载器要求（PCL IsInstanceSuitable 的简化精确版）。
bool mod_version_suitable_for(const ModVersion& version, const InstanceTarget& target);

// PCL 文件名：替换 Windows 非法字符、Mod 的 ~→-；只取 basename；超长截断。
std::string pcl_download_file_name(const std::string& raw_file_name,
                                   const std::string& translated_name, int translate_mode);

}  // namespace sscl::core
