#include "platform/path_utils.hpp"

#include "platform/win_error.hpp"

namespace sscl::platform {

std::filesystem::path exe_path() {
    std::wstring buffer(260, L'\0');
    for (;;) {
        const DWORD length =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        // 缓冲区不足：扩容重试
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path exe_dir() {
    return exe_path().parent_path();
}

std::filesystem::path data_dir() {
    return exe_dir() / L"SSCL";
}

}  // namespace sscl::platform
