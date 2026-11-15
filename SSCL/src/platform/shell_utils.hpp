#pragma once

// 系统外壳辅助：打开浏览器、写剪贴板、保存文件对话框。

#include <filesystem>
#include <string>
#include <vector>

namespace sscl::platform {

// ShellExecuteW 打开 URL（默认浏览器）。
bool open_url(const std::string& utf8_url);

// 写入剪贴板文本（UTF-8 → UTF-16）。
bool set_clipboard_text(const std::string& utf8_text);

// 读取剪贴板文本（UTF-16 → UTF-8）。无文本/被占用时返回空串；
// 单次最多读取 64K 字符，避免超长剪贴板拖住 UI 线程。
std::string get_clipboard_text();

// Windows 保存文件对话框（PCL 下载页“另存为”）。owner 为 HWND（可空）。
// filter 形如 L"Mod 文件\0*.jar;*.litemod\0所有文件\0*.*\0\0"。
// 成功返回完整路径；用户取消返回 false。
bool save_file_dialog(void* owner, const wchar_t* title, const std::wstring& default_name,
                      const std::wstring& default_dir, const wchar_t* filter,
                      const wchar_t* default_ext, std::wstring* out_path);

// Windows 多选打开文件对话框。成功返回选择数量。
bool open_file_dialog(void* owner, const wchar_t* title, const wchar_t* filter,
                      std::vector<std::wstring>* out_paths);

// 选择文件夹（PCL McFolderSelected）。成功返回完整路径。
bool select_folder_dialog(void* owner, const wchar_t* title, std::wstring* out_path);

// 资源管理器打开目录/文件；失败返回 false。
bool open_folder(const std::filesystem::path& path);

// 当前用户漫游 AppData（%APPDATA%）；失败返回空路径。
std::filesystem::path roaming_appdata_dir();

// 放入回收站（失败返回 false；不抛异常）。
bool recycle_file(const std::filesystem::path& path);
// 永久删除（PCL Shift+Delete）；失败返回 false。
bool delete_file_permanent(const std::filesystem::path& path);

}  // namespace sscl::platform
