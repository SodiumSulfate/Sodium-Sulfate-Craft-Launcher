#include "util/string_utils.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace sscl::util {

std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int utf8_length = static_cast<int>(utf8.size());
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), utf8_length, nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    const int written =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), utf8_length, wide.data(), needed);
    if (written != needed) {
        return {};
    }
    return wide;
}

std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int wide_length = static_cast<int>(wide.size());
    const int needed =
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length, utf8.data(),
                                              needed, nullptr, nullptr);
    if (written != needed) {
        return {};
    }
    return utf8;
}

std::string path_to_utf8(const std::filesystem::path& path) {
    return wide_to_utf8(path.wstring());
}

std::filesystem::path path_from_utf8(std::string_view utf8) {
    return std::filesystem::path(utf8_to_wide(utf8));
}

std::string trim_left(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size()) {
        const char c = text[begin];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') {
            ++begin;
        } else {
            break;
        }
    }
    return std::string(text.substr(begin));
}

std::string trim_right(std::string_view text) {
    size_t end = text.size();
    while (end > 0) {
        const char c = text[end - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') {
            --end;
        } else {
            break;
        }
    }
    return std::string(text.substr(0, end));
}

std::string trim(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end) {
        const char c = text[begin];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') {
            ++begin;
        } else {
            break;
        }
    }
    while (end > begin) {
        const char c = text[end - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') {
            --end;
        } else {
            break;
        }
    }
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> split(std::string_view text, char delimiter, bool keep_empty) {
    std::vector<std::string> parts;
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t pos = text.find(delimiter, begin);
        const size_t end = (pos == std::string_view::npos) ? text.size() : pos;
        std::string_view segment = text.substr(begin, end - begin);
        if (keep_empty || !segment.empty()) {
            parts.emplace_back(segment);
        }
        if (pos == std::string_view::npos) {
            break;
        }
        begin = pos + 1;
    }
    return parts;
}

std::string to_lower_ascii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

}  // namespace sscl::util
