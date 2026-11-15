#include "core/mod/mod_name.hpp"

#include <algorithm>
#include <cctype>

namespace sscl::core {

namespace {

bool ends_with_icase(const std::string& text, const char* suffix) {
    const std::string tail(suffix);
    if (text.size() < tail.size()) {
        return false;
    }
    const std::size_t offset = text.size() - tail.size();
    for (std::size_t i = 0; i < tail.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(text[offset + i]);
        const unsigned char b = static_cast<unsigned char>(tail[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

void trim_trailing_dots_spaces(std::string* text) {
    while (!text->empty()) {
        const char c = text->back();
        if (c == '.' || c == ' ') {
            text->pop_back();
        } else {
            break;
        }
    }
}

bool is_reserved_name(const std::string& base) {
    std::string upper;
    upper.reserve(base.size());
    for (const unsigned char c : base) {
        upper.push_back(static_cast<char>(std::toupper(c)));
    }
    const std::size_t dot = upper.find('.');
    if (dot != std::string::npos) {
        upper.resize(dot);
    }
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") {
        return true;
    }
    if (upper.size() == 4 && (upper.rfind("COM", 0) == 0 || upper.rfind("LPT", 0) == 0) &&
        upper[3] >= '1' && upper[3] <= '9') {
        return true;
    }
    return false;
}

std::string truncate_utf8(const std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text;
    }
    std::size_t end = max_bytes;
    // 不要把 UTF-8 多字节序列截成半个字符。
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return text.substr(0, end);
}

}  // namespace

util::Result<std::string> sanitize_mod_file_name(const std::string& raw) {
    if (raw.empty()) {
        return util::Result<std::string>::fail(util::Error::make(
            "mod.file_name_invalid", "Mod 文件名不合法", "empty"));
    }
    // 取 basename：同时处理 / 与 \；末尾分隔符视为非法。
    const std::size_t slash = raw.find_last_of("/\\");
    const std::string base_name =
        slash == std::string::npos ? raw : raw.substr(slash + 1);
    if (base_name.empty() || base_name == "." || base_name == "..") {
        return util::Result<std::string>::fail(util::Error::make(
            "mod.file_name_invalid", "Mod 文件名不合法", raw));
    }

    std::string name;
    name.reserve(base_name.size());
    for (const unsigned char c : base_name) {
        if (c < 0x20 || c == 0x7F) {
            continue;  // 控制字符直接丢弃
        }
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' ||
            c == '|' || c == '?' || c == '*') {
            name.push_back('_');
        } else if (c == '~') {
            name.push_back('-');  // PCL 实测：~ 会导致 Mixin 加载失败
        } else {
            name.push_back(static_cast<char>(c));
        }
    }
    // 首尾空格/点：Windows 会静默去掉，统一显式处理。
    while (!name.empty() && name.front() == ' ') {
        name.erase(name.begin());
    }
    trim_trailing_dots_spaces(&name);
    if (name.empty()) {
        return util::Result<std::string>::fail(util::Error::make(
            "mod.file_name_invalid", "Mod 文件名不合法", raw));
    }

    std::string base = name;
    std::string extension;
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        base = name.substr(0, dot);
        extension = name.substr(dot);
        trim_trailing_dots_spaces(&base);
    } else {
        extension.clear();
    }
    if (base.empty()) {
        return util::Result<std::string>::fail(util::Error::make(
            "mod.file_name_invalid", "Mod 文件名不合法", raw));
    }

    // 只允许 .jar（大小写不敏感，统一小写扩展名）。
    if (extension.empty() || !ends_with_icase(extension, ".jar")) {
        return util::Result<std::string>::fail(util::Error::make(
            "mod.file_type_unsupported", "只支持安装 .jar 格式的 Mod", raw));
    }
    extension = ".jar";

    if (is_reserved_name(base)) {
        base.insert(base.begin(), '_');
    }

    const std::size_t max_base = kMaxModFileNameBytes > extension.size()
                                     ? kMaxModFileNameBytes - extension.size()
                                     : 1;
    base = truncate_utf8(base, max_base);
    trim_trailing_dots_spaces(&base);
    if (base.empty()) {
        return util::Result<std::string>::fail(util::Error::make(
            "mod.file_name_invalid", "Mod 文件名不合法", raw));
    }
    return util::Result<std::string>::ok(base + extension);
}

bool is_installable_mod_file(const std::string& file_name) {
    return ends_with_icase(file_name, ".jar");
}

bool is_disabled_mod_file(const std::string& file_name) {
    return ends_with_icase(file_name, ".disabled");
}

bool is_old_mod_file(const std::string& file_name) {
    return ends_with_icase(file_name, ".old");
}

std::string strip_mod_state_suffix(const std::string& file_name) {
    std::string result = file_name;
    bool changed = true;
    while (changed) {
        changed = false;
        if (is_disabled_mod_file(result)) {
            result.resize(result.size() - std::string(".disabled").size());
            changed = true;
        } else if (is_old_mod_file(result)) {
            result.resize(result.size() - std::string(".old").size());
            changed = true;
        }
    }
    return result;
}

std::string make_old_mod_file_name(const std::string& file_name) {
    return file_name + ".old";
}

std::string make_disabled_mod_file_name(const std::string& file_name) {
    return file_name + ".disabled";
}

}  // namespace sscl::core
