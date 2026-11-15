#pragma once

// Win32 错误码格式化（用于 Error::detail，见 util/result.hpp）。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <string>
#include <string_view>

#include "util/string_utils.hpp"

namespace sscl::platform {

// 把 GetLastError() 等 Win32 错误码转为 "<码> <系统消息>"（UTF-8）。
inline std::string format_win_error(unsigned long error_code) {
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length =
        ::FormatMessageW(flags, nullptr, static_cast<DWORD>(error_code), 0,
                         reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    std::string message;
    if (length > 0 && buffer != nullptr) {
        std::wstring_view view(buffer, length);
        while (!view.empty() &&
               (view.back() == L'\r' || view.back() == L'\n' || view.back() == L' ')) {
            view.remove_suffix(1);
        }
        message = std::to_string(error_code) + " " + sscl::util::wide_to_utf8(view);
    } else {
        message = std::to_string(error_code);
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    return message;
}

}  // namespace sscl::platform
