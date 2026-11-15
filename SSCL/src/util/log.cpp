#include "util/log.hpp"

#include "util/string_utils.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace sscl::util {
namespace {

std::string level_name(LogLevel level) {
    switch (level) {
        case LogLevel::kTrace:
            return "trace";
        case LogLevel::kDebug:
            return "debug";
        case LogLevel::kInfo:
            return "info";
        case LogLevel::kWarn:
            return "warn";
        case LogLevel::kError:
            return "error";
    }
    return "info";
}

// "2026-10-23 18:30:00.123"
std::string now_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm_buf.tm_year + 1900,
                  tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                  static_cast<int>(ms.count()));
    return std::string(buf);
}

// 文件名时间戳："20261023-183000"
std::string file_stamp() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d", tm_buf.tm_year + 1900,
                  tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return std::string(buf);
}

bool is_value_delimiter(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == '\'' || c == '&' ||
           c == ',' || c == ';' || c == '}' || c == ']' || c == '|';
}

bool is_separator_or_quote(char c) {
    return c == ' ' || c == '\t' || c == '"' || c == '\'' || c == ':' || c == '=';
}

}  // namespace

Logger& Logger::instance() {
    static Logger s_instance;
    return s_instance;
}

Logger::~Logger() {
    shutdown();
}

bool Logger::init(const std::string& log_dir_utf8, LogLevel level) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.is_open()) {
        m_file.close();
    }
    m_level = level;

    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(utf8_to_wide(log_dir_utf8));
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path file_path = dir / (L"launcher-" + utf8_to_wide(file_stamp()) + L".log");
    m_file.open(file_path, std::ios::out | std::ios::app);
    return m_file.is_open();
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.is_open()) {
        m_file.flush();
        m_file.close();
    }
}

void Logger::set_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_level = level;
}

LogLevel Logger::level() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_level;
}

void Logger::log(LogLevel level, std::string_view message) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (level < m_level) {
            // 低于当前等级：调试器输出也不写，保持一致
            return;
        }
    }

    const std::string safe = redact(message);
    const std::string line = "[" + now_timestamp() + "] [" + level_name(level) + "] " + safe + "\r\n";

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_file.is_open()) {
            m_file << line;
            if (level >= LogLevel::kError) {
                m_file.flush();
            }
        }
    }
    ::OutputDebugStringA(line.c_str());
}

std::string Logger::redact(std::string_view text) {
    // 需要脱敏的键（小写）；覆盖 §9.2 明文要求的 access_token / refresh_token / API Key
    static const char* const kSecretKeys[] = {
        "access_token",  "refresh_token", "client_secret", "api_key",       "apikey",
        "x-api-key",     "authorization", "password",      "session_token", "auth_token",
    };
    static const std::string kMask = "***";

    std::string out(text);
    for (const char* key : kSecretKeys) {
        const std::string key_lower(key);
        size_t search_pos = 0;
        while (true) {
            const std::string lower = to_lower_ascii(out);
            const size_t pos = lower.find(key_lower, search_pos);
            if (pos == std::string::npos) {
                break;
            }

            // 跳过键与值之间的引号 / 空白 / ':' / '='
            size_t value_begin = pos + key_lower.size();
            while (value_begin < out.size() && is_separator_or_quote(out[value_begin])) {
                ++value_begin;
            }

            // "Authorization: Bearer xxx" 保留 "Bearer " 前缀
            size_t mask_begin = value_begin;
            if (value_begin + 7 <= out.size() &&
                to_lower_ascii(std::string_view(out).substr(value_begin, 7)) == "bearer ") {
                mask_begin = value_begin + 7;
            }

            size_t value_end = mask_begin;
            while (value_end < out.size() && !is_value_delimiter(out[value_end])) {
                ++value_end;
            }

            if (value_end > mask_begin) {
                out.replace(mask_begin, value_end - mask_begin, kMask);
                search_pos = mask_begin + kMask.size();
            } else {
                search_pos = value_begin;
            }
        }
    }
    return out;
}

void log_trace(std::string_view message) {
    Logger::instance().log(LogLevel::kTrace, message);
}

void log_debug(std::string_view message) {
    Logger::instance().log(LogLevel::kDebug, message);
}

void log_info(std::string_view message) {
    Logger::instance().log(LogLevel::kInfo, message);
}

void log_warn(std::string_view message) {
    Logger::instance().log(LogLevel::kWarn, message);
}

void log_error(std::string_view message) {
    Logger::instance().log(LogLevel::kError, message);
}

}  // namespace sscl::util
