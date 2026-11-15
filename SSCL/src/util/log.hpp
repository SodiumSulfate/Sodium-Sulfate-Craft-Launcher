#pragma once

// 日志（开发文档 §4.2：自实现；§9.2：日志必须脱敏 access_token / refresh_token / API Key）。
// 输出目标：SSCL\logs\launcher-YYYYMMDD-HHMMSS.log（每次运行一个文件）+ 调试器 OutputDebugString。
// 线程安全。见 DECISIONS.md D-007：日志属基础设施，采用进程级单例；业务服务仍走依赖注入。

#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace sscl::util {

enum class LogLevel {
    kTrace,
    kDebug,
    kInfo,
    kWarn,
    kError,
};

class Logger {
public:
    // 进程级日志器。
    static Logger& instance();

    // 打开日志文件；log_dir_utf8 为 UTF-8 目录路径（不存在则逐级创建）。
    // 成功返回 true；失败返回 false（此时日志仅输出到调试器）。
    bool init(const std::string& log_dir_utf8, LogLevel level = LogLevel::kInfo);

    // 关闭日志文件（幂等）。
    void shutdown();

    void set_level(LogLevel level);
    LogLevel level() const;

    // 写一条日志。message 应为单行（不含换行）；内部会先做 redact()。
    void log(LogLevel level, std::string_view message);

    // 敏感信息脱敏：把 token / key / password 等键的值替换为 ***。
    // 策略（尽力而为）：按键名（不区分大小写）定位，屏蔽其后直到分隔符的值；
    // "Authorization: Bearer xxx" 保留 "Bearer " 前缀只屏蔽凭据。
    static std::string redact(std::string_view text);

private:
    Logger() = default;
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    mutable std::mutex m_mutex;
    std::ofstream m_file;
    LogLevel m_level = LogLevel::kInfo;
};

// 便捷函数
void log_trace(std::string_view message);
void log_debug(std::string_view message);
void log_info(std::string_view message);
void log_warn(std::string_view message);
void log_error(std::string_view message);

}  // namespace sscl::util
