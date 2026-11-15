#pragma once

// Result<T, Error>：核心层错误传递方式（开发文档 §5.1）
// 约定：核心层不向 UI 抛未捕获异常；失败一律通过返回值表达，
// Error 携带机器可读 code、用户可读 message（简体中文）与技术细节 detail。

#include <string>
#include <utility>
#include <variant>

namespace sscl::util {

struct Error {
    std::string code;     // 机器可读错误码，形如 "http.timeout"，供程序分支判断
    std::string message;  // 用户可读信息（简体中文），可直接展示
    std::string detail;   // 技术细节，写入日志或供“复制详情”

    static Error make(std::string code, std::string message, std::string detail = {}) {
        return Error{std::move(code), std::move(message), std::move(detail)};
    }
};

// 成功携带 T，失败携带 Error。
// 注意：value()/error() 在状态不符时抛 std::bad_variant_access（属调用方编程错误），
// 调用前必须先检查 is_ok() / is_error()。
template <typename T>
class Result {
public:
    static Result ok(T value) {
        return Result(Storage(std::in_place_index<0>, std::move(value)));
    }

    static Result fail(Error error) {
        return Result(Storage(std::in_place_index<1>, std::move(error)));
    }

    bool is_ok() const { return std::holds_alternative<T>(m_data); }
    bool is_error() const { return std::holds_alternative<Error>(m_data); }
    explicit operator bool() const { return is_ok(); }

    T& value() { return std::get<T>(m_data); }
    const T& value() const { return std::get<T>(m_data); }

    Error& error() { return std::get<Error>(m_data); }
    const Error& error() const { return std::get<Error>(m_data); }

private:
    using Storage = std::variant<T, Error>;

    explicit Result(Storage data) : m_data(std::move(data)) {}

    Storage m_data;
};

// 无返回值操作的特化：仅表达成功 / 失败。
template <>
class Result<void> {
public:
    static Result ok() { return Result(true, Error{}); }

    static Result fail(Error error) { return Result(false, std::move(error)); }

    bool is_ok() const { return m_ok; }
    bool is_error() const { return !m_ok; }
    explicit operator bool() const { return m_ok; }

    Error& error() { return m_error; }
    const Error& error() const { return m_error; }

private:
    Result(bool ok_flag, Error error) : m_ok(ok_flag), m_error(std::move(error)) {}

    bool m_ok;
    Error m_error;
};

}  // namespace sscl::util
