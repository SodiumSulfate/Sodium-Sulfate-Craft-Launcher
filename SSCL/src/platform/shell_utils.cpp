#include "platform/shell_utils.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "util/string_utils.hpp"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")

namespace sscl::platform {

bool open_url(const std::string& utf8_url) {
    const std::wstring wide = sscl::util::utf8_to_wide(utf8_url);
    const HINSTANCE result =
        ::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool open_folder(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::path target = path;
    ec.clear();
    if (std::filesystem::is_regular_file(target, ec)) {
        // 传进来的是文件（如 java.exe）：打开它所在的目录，绝不执行该文件。
        target = target.parent_path();
    }
    ec.clear();
    if (!std::filesystem::is_directory(target, ec)) {
        // 目录不存在：仅在「看起来是目录」（无扩展名）时创建再打开（PCL 同款行为）。
        // 带扩展名却不存在的路径一律拒绝，绝不交给 ShellExecute 去猜——数据目录
        // 末段正好叫 "SSCL"，"open" 动词会回退搜索同名 SSCL.exe 并启动新程序。
        if (target.has_extension()) {
            return false;
        }
        std::filesystem::create_directories(target, ec);
        ec.clear();
        if (!std::filesystem::is_directory(target, ec)) {
            return false;
        }
    }

    ec.clear();
    target = std::filesystem::absolute(target, ec);
    if (ec) {
        return false;
    }
    const std::wstring plain = target.wstring();
    std::wstring explore_path = plain;
    if (!explore_path.empty() && explore_path.back() != L'\\' &&
        explore_path.back() != L'/') {
        explore_path.push_back(L'\\');  // 尾随分隔符让 ShellExecute 明确识别为目录
    }

    // "explore" 是「在资源管理器中浏览文件夹」的专用动词；"open" 会对路径做
    // 可执行文件探测，是「点打开数据目录却启动新 SSCL」的根因。
    HINSTANCE result = ::ShellExecuteW(nullptr, L"explore", explore_path.c_str(), nullptr,
                                       nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        // 兜底：显式启动 explorer.exe。这里用不带尾反斜杠的路径，
        // 否则 "C:\a\SSCL\" 里的 \" 会被命令行解析成转义引号。
        const std::wstring params = L"\"" + plain + L"\"";
        result = ::ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr,
                                 SW_SHOWNORMAL);
    }
    return reinterpret_cast<INT_PTR>(result) > 32;
}

std::filesystem::path roaming_appdata_dir() {
    PWSTR raw = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) ||
        raw == nullptr) {
        if (raw != nullptr) {
            ::CoTaskMemFree(raw);
        }
        return {};
    }
    std::filesystem::path result(raw);
    ::CoTaskMemFree(raw);
    return result;
}

bool set_clipboard_text(const std::string& utf8_text) {
    if (!::OpenClipboard(nullptr)) {
        return false;
    }
    bool ok = false;
    ::EmptyClipboard();
    const std::wstring wide = sscl::util::utf8_to_wide(utf8_text);
    const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* data = ::GlobalLock(memory)) {
            memcpy(data, wide.c_str(), bytes);
            ::GlobalUnlock(memory);
            ok = ::SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!ok) {
            ::GlobalFree(memory);
        }
    }
    ::CloseClipboard();
    return ok;
}

std::string get_clipboard_text() {
    // 单次粘贴上限：超长剪贴板（日志/代码）不应一次性塞进单行输入框，
    // 既没必要也会让 utf8 转换与排版变慢。
    constexpr std::size_t kClipboardTextLimit = 64 * 1024;
    if (::IsClipboardFormatAvailable(CF_UNICODETEXT) == FALSE) {
        return {};
    }
    if (!::OpenClipboard(nullptr)) {
        return {};  // 剪贴板被其它进程占用：静默失败，按一次空粘贴处理
    }
    std::string utf8;
    if (HANDLE handle = ::GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* text = static_cast<const wchar_t*>(::GlobalLock(handle))) {
            std::size_t length = 0;
            while (length < kClipboardTextLimit && text[length] != L'\0') {
                ++length;
            }
            utf8 = sscl::util::wide_to_utf8(std::wstring(text, length));
            ::GlobalUnlock(handle);
        }
    }
    ::CloseClipboard();
    return utf8;
}

bool save_file_dialog(void* owner, const wchar_t* title, const std::wstring& default_name,
                      const std::wstring& default_dir, const wchar_t* filter,
                      const wchar_t* default_ext, std::wstring* out_path) {
    if (out_path == nullptr) {
        return false;
    }
    std::vector<wchar_t> buffer(32768, L'\0');
    const std::wstring name = default_name.empty() ? L"download" : default_name;
    const std::size_t copy = (std::min)(name.size(), buffer.size() - 1);
    std::copy_n(name.c_str(), copy, buffer.data());

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = static_cast<HWND>(owner);
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrInitialDir = default_dir.empty() ? nullptr : default_dir.c_str();
    dialog.lpstrTitle = title;
    dialog.lpstrDefExt = default_ext;
    dialog.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (::GetSaveFileNameW(&dialog) == FALSE) {
        return false;
    }
    *out_path = buffer.data();
    return true;
}

bool open_file_dialog(void* owner, const wchar_t* title, const wchar_t* filter,
                      std::vector<std::wstring>* out_paths) {
    if (out_paths == nullptr) {
        return false;
    }
    out_paths->clear();
    std::vector<wchar_t> buffer(32768, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = static_cast<HWND>(owner);
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrTitle = title;
    dialog.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST |
                   OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (::GetOpenFileNameW(&dialog) == FALSE) {
        return false;
    }
    std::size_t offset = 0;
    const std::wstring first(buffer.data() + offset);
    offset += first.size() + 1;
    if (buffer[offset] == L'\0') {
        out_paths->push_back(first);
        return true;
    }
    while (offset < buffer.size() && buffer[offset] != L'\0') {
        const std::wstring name(buffer.data() + offset);
        offset += name.size() + 1;
        out_paths->push_back(first + L"\\" + name);
    }
    return !out_paths->empty();
}

bool select_folder_dialog(void* owner, const wchar_t* title, std::wstring* out_path) {
    if (out_path == nullptr) {
        return false;
    }
    // BIF_NEWDIALOGSTYLE（现代文件夹对话框）要求调用线程已初始化 COM。
    // 本进程未在任何位置做全局初始化，这里按需临时初始化；
    // 只回滚自己成功建立的套间（S_FALSE 也算成功，必须 CoUninitialize）。
    struct ScopedComApartment {
        bool owned = false;
        ScopedComApartment() {
            owned = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        }
        ~ScopedComApartment() {
            if (owned) {
                ::CoUninitialize();
            }
        }
        ScopedComApartment(const ScopedComApartment&) = delete;
        ScopedComApartment& operator=(const ScopedComApartment&) = delete;
    } com;
    (void)com;

    BROWSEINFOW browse{};
    browse.hwndOwner = static_cast<HWND>(owner);
    browse.lpszTitle = title;
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    PIDLIST_ABSOLUTE idlist = ::SHBrowseForFolderW(&browse);
    if (idlist == nullptr) {
        return false;
    }
    wchar_t path[MAX_PATH * 4]{};
    const bool ok = ::SHGetPathFromIDListW(idlist, path) != FALSE;
    ::CoTaskMemFree(idlist);
    if (!ok) {
        return false;
    }
    *out_path = path;
    return true;
}

bool recycle_file(const std::filesystem::path& path) {
    std::wstring from = path.wstring();
    from.push_back(L'\0');
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return ::SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

bool delete_file_permanent(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return !ec;
}

}  // namespace sscl::platform
