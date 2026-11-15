#include "core/game/instance_launch.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace sscl::core {

namespace {

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

int parse_major(const std::string& text) {
    const std::string value = trim(text);
    if (value.empty()) {
        return -1;
    }
    int first = 0;
    std::size_t i = 0;
    while (i < value.size() && std::isdigit(static_cast<unsigned char>(value[i]))) {
        first = first * 10 + (value[i] - '0');
        ++i;
    }
    if (i == 0) {
        return -1;
    }
    // 1.8.0 -> 8；1.7 -> 7。现代版本直接取首段。
    if (first == 1 && i < value.size() && value[i] == '.') {
        ++i;
        int second = 0;
        const std::size_t start = i;
        while (i < value.size() && std::isdigit(static_cast<unsigned char>(value[i]))) {
            second = second * 10 + (value[i] - '0');
            ++i;
        }
        if (i > start) {
            return second;
        }
    }
    return first;
}

}  // namespace

InstanceLaunchOverrides merge_instance_launch(const InstanceSettings& settings,
                                              int global_memory_mb) {
    InstanceLaunchOverrides result;
    int memory = global_memory_mb;
    if (memory <= 0) {
        memory = 1024;
    }
    if (settings.ram_type == 1) {
        const int gb = (std::max)(1, (std::min)(64, settings.ram_custom_gb));
        memory = gb * 1024;
    }
    result.memory_mb = memory;
    result.java_mode = (std::max)(0, (std::min)(3, settings.java_mode));
    if (result.java_mode == 3) {
        result.java_path = settings.java_path;
    } else if (result.java_mode == 1) {
        result.java_range = settings.java_range;
    }
    result.jvm_args = settings.jvm_args;
    result.game_args = settings.game_args;
    result.gc_mode = (std::max)(0, (std::min)(5, settings.gc_mode));
    result.version_info = settings.version_info;
    result.window_title = settings.window_title;
    result.server_enter = settings.server_enter;
    return result;
}

bool parse_java_range(const std::string& text, int* min_major, int* max_major,
                      bool* max_inclusive) {
    if (min_major == nullptr || max_major == nullptr || max_inclusive == nullptr) {
        return false;
    }
    const std::string value = trim(text);
    if (value.size() < 5) {
        return false;
    }
    if (value.front() != '[' && value.front() != '(') {
        return false;
    }
    if (value.back() != ']' && value.back() != ')') {
        return false;
    }
    const std::size_t comma = value.find(',');
    if (comma == std::string::npos || comma <= 1 || comma >= value.size() - 2) {
        return false;
    }
    const int min_value = parse_major(value.substr(1, comma - 1));
    const int max_value = parse_major(value.substr(comma + 1, value.size() - comma - 2));
    if (min_value <= 0 || max_value <= 0 || min_value > max_value) {
        return false;
    }
    *min_major = min_value;
    *max_major = max_value;
    *max_inclusive = value.back() == ']';
    return true;
}

}  // namespace sscl::core
