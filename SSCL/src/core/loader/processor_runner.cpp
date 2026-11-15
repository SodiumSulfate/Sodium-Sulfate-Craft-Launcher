#include "core/loader/processor_runner.hpp"

#include <algorithm>
#include <cctype>

#include "platform/file_utils.hpp"
#include "platform/process.hpp"
#include "platform/zip_utils.hpp"
#include "util/hash.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

namespace sscl::core {

namespace {

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string strip_quotes(const std::string& text) {
    if (text.size() >= 2 && text.front() == '\'' && text.back() == '\'') {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

std::filesystem::path library_path(const std::string& coordinates,
                                   const std::filesystem::path& libraries_dir) {
    if (coordinates.empty()) {
        return {};
    }
    return libraries_dir / library_relative_path(coordinates);
}

// 从 processor jar 的 MANIFEST.MF 读取 Main-Class（含续行折叠）。
util::Result<std::string> read_jar_main_class(const std::filesystem::path& jar) {
    util::Result<std::string> manifest = platform::zip_read_entry(jar, "META-INF/MANIFEST.MF");
    if (manifest.is_error()) {
        return util::Result<std::string>::fail(util::Error::make(
            "loader.installer_invalid", "处理器 jar 缺少 MANIFEST.MF",
            util::path_to_utf8(jar)));
    }
    const std::string& text = manifest.value();
    std::string current;
    std::vector<std::string> lines;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            if (!current.empty() && current.back() == '\r') {
                current.pop_back();
            }
            lines.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(text[i]);
    }
    std::string value;
    for (const std::string& line : lines) {
        if (!line.empty() && line.front() == ' ' && !value.empty()) {
            value += line.substr(1);  // 续行
            continue;
        }
        if (starts_with(line, "Main-Class:")) {
            value = trim(line.substr(std::string("Main-Class:").size()));
        }
    }
    if (value.empty()) {
        return util::Result<std::string>::fail(util::Error::make(
            "loader.installer_invalid", "处理器 jar 未声明 Main-Class",
            util::path_to_utf8(jar)));
    }
    return util::Result<std::string>::ok(std::move(value));
}

bool has_task(const InstallerProcessor& processor, const std::string& task) {
    for (std::size_t i = 0; i + 1 < processor.args.size(); ++i) {
        if (processor.args[i] == "--task" && processor.args[i + 1] == task) {
            return true;
        }
    }
    return false;
}

std::string argument_after(const InstallerProcessor& processor, const std::string& key) {
    for (std::size_t i = 0; i + 1 < processor.args.size(); ++i) {
        if (processor.args[i] == key) {
            return processor.args[i + 1];
        }
    }
    return {};
}

// 人类可读标签：优先 --task 名（如 MERGE_MAPPING），否则 jar 坐标。
std::string processor_label(const InstallerProcessor& processor, std::size_t index) {
    const std::string task = argument_after(processor, "--task");
    if (!task.empty()) {
        return task;
    }
    if (!processor.jar.empty()) {
        return processor.jar;
    }
    return "处理器 " + std::to_string(index + 1);
}

}  // namespace

util::Result<std::string> resolve_installer_data_value(
    const std::string& raw, const std::filesystem::path& installer_path,
    const std::filesystem::path& libraries_dir, const std::filesystem::path& temp_dir,
    std::stop_token stop) {
    const std::string text = trim(raw);
    if (text.empty()) {
        return util::Result<std::string>::ok(std::string());
    }
    if (text.front() == '\'' && text.back() == '\'') {
        return util::Result<std::string>::ok(strip_quotes(text));
    }
    if (text.front() == '[' && text.back() == ']') {
        const std::string coordinates = text.substr(1, text.size() - 2);
        return util::Result<std::string>::ok(
            util::path_to_utf8(library_path(coordinates, libraries_dir)));
    }
    if (text.front() == '/') {
        // 安装器内条目 → 抽取到临时目录
        const std::string entry = text.substr(1);
        if (stop.stop_requested()) {
            return util::Result<std::string>::fail(
                util::Error::make("process.cancelled", "安装已取消", {}));
        }
        util::Result<std::string> bytes = platform::zip_read_entry(installer_path, entry);
        if (bytes.is_error()) {
            return util::Result<std::string>::fail(util::Error::make(
                "loader.installer_invalid", "安装器缺少数据文件：" + entry, {}));
        }
        std::error_code ec;
        std::filesystem::create_directories(temp_dir, ec);
        const std::filesystem::path target =
            temp_dir / util::path_from_utf8(entry).filename();
        util::Result<void> written = platform::write_file(target, bytes.value());
        if (written.is_error()) {
            return util::Result<std::string>::fail(written.error());
        }
        return util::Result<std::string>::ok(util::path_to_utf8(target));
    }
    return util::Result<std::string>::ok(text);
}

util::Result<std::string> resolve_processor_text(const std::string& text,
                                                 const ProcessorEnvironment& env) {
    const std::string trimmed = trim(text);
    if (trimmed.empty()) {
        return util::Result<std::string>::ok(std::string());
    }
    if (trimmed.front() == '\'' && trimmed.back() == '\'') {
        return util::Result<std::string>::ok(strip_quotes(trimmed));
    }
    if (trimmed.front() == '[' && trimmed.back() == ']') {
        const std::string coordinates = trimmed.substr(1, trimmed.size() - 2);
        return util::Result<std::string>::ok(
            util::path_to_utf8(library_path(coordinates, env.libraries_dir)));
    }
    // 单一 {KEY}
    if (trimmed.front() == '{' && trimmed.back() == '}' && trimmed.find('}') == trimmed.size() - 1) {
        const std::string key = trimmed.substr(1, trimmed.size() - 2);
        const auto it = env.variables.find(key);
        if (it == env.variables.end()) {
            return util::Result<std::string>::fail(util::Error::make(
                "loader.processor_variable_missing", "处理器变量缺失：{" + key + "}", {}));
        }
        return resolve_processor_text(it->second, env);  // 变量值可能仍是 [coords]/'literal'
    }
    // 全文替换 {KEY}（如 {ROOT}/libraries）
    std::string result;
    result.reserve(trimmed.size());
    for (std::size_t i = 0; i < trimmed.size();) {
        if (trimmed[i] == '{') {
            const std::size_t close = trimmed.find('}', i + 1);
            if (close != std::string::npos) {
                const std::string key = trimmed.substr(i + 1, close - i - 1);
                const auto it = env.variables.find(key);
                if (it == env.variables.end()) {
                    return util::Result<std::string>::fail(util::Error::make(
                        "loader.processor_variable_missing", "处理器变量缺失：{" + key + "}", {}));
                }
                result += it->second;
                i = close + 1;
                continue;
            }
        }
        result.push_back(trimmed[i]);
        ++i;
    }
    return util::Result<std::string>::ok(std::move(result));
}

util::Result<ProcessorCommand> build_processor_command(const InstallerProcessor& processor,
                                                       const ProcessorEnvironment& env) {
    ProcessorCommand command;
    const std::filesystem::path jar_path = library_path(processor.jar, env.libraries_dir);
    if (jar_path.empty() || !std::filesystem::is_regular_file(jar_path)) {
        return util::Result<ProcessorCommand>::fail(util::Error::make(
            "loader.processor_classpath_missing", "处理器 jar 未找到：" + processor.jar, {}));
    }
    for (const std::string& name : processor.classpath) {
        const std::filesystem::path path = library_path(name, env.libraries_dir);
        if (path.empty()) {
            continue;
        }
        command.classpath.push_back(path);
    }
    command.classpath.push_back(jar_path);

    util::Result<std::string> main_class = read_jar_main_class(jar_path);
    if (main_class.is_error()) {
        return util::Result<ProcessorCommand>::fail(main_class.error());
    }
    command.main_class = std::move(main_class.value());
    command.args.reserve(processor.args.size());
    for (const std::string& arg : processor.args) {
        util::Result<std::string> resolved = resolve_processor_text(arg, env);
        if (resolved.is_error()) {
            return util::Result<ProcessorCommand>::fail(resolved.error());
        }
        command.args.push_back(std::move(resolved.value()));
    }
    return util::Result<ProcessorCommand>::ok(std::move(command));
}

util::Result<void> run_processors(const std::vector<InstallerProcessor>& processors,
                                  const ProcessorEnvironment& env, std::stop_token stop,
                                  const ProcessorLineFn& on_line,
                                  const MojmapsDownloadFn& download_mojmaps,
                                  const ProcessorProgressFn& on_progress) {
    for (std::size_t index = 0; index < processors.size(); ++index) {
        const InstallerProcessor& processor = processors[index];
        if (!processor.sides.empty() &&
            std::find(processor.sides.begin(), processor.sides.end(), "client") ==
                processor.sides.end()) {
            continue;  // 仅服务端处理器
        }
        if (stop.stop_requested()) {
            return util::Result<void>::fail(
                util::Error::make("process.cancelled", "安装已取消", {}));
        }
        if (on_progress) {
            // 每一步都回报，避免长任务（remap 等）期间界面没有任何变化
            on_progress(index + 1, processors.size(), processor_label(processor, index));
        }

        // outputs：路径模板 → SHA1 模板
        struct Output {
            std::filesystem::path path;
            std::string sha1;
        };
        std::vector<Output> outputs;
        bool ready = true;
        for (const auto& [path_template, sha_template] : processor.outputs) {
            util::Result<std::string> path_text = resolve_processor_text(path_template, env);
            if (path_text.is_error()) {
                return util::Result<void>::fail(path_text.error());
            }
            util::Result<std::string> sha_text = resolve_processor_text(sha_template, env);
            if (sha_text.is_error()) {
                return util::Result<void>::fail(sha_text.error());
            }
            Output output;
            output.path = util::path_from_utf8(path_text.value());
            output.sha1 = sha_text.value();
            std::error_code ec;
            if (output.path.empty() || !std::filesystem::is_regular_file(output.path, ec)) {
                ready = false;
            } else if (!output.sha1.empty()) {
                util::Result<std::string> actual =
                    util::hash_file_hex(util::HashAlgorithm::kSha1, output.path);
                if (actual.is_error() || !util::hash_hex_equal(actual.value(), output.sha1)) {
                    ready = false;
                }
            }
            outputs.push_back(std::move(output));
        }
        if (!outputs.empty() && ready) {
            util::log_info("[loader] processor " + std::to_string(index) + " already applied");
            continue;  // 幂等：输出就绪
        }

        // MOJMAPS：不联网执行处理器，交给上层下载
        // 注意：不能用 processor.outputs 的 SHA1 校验（Forge 的 {MOJMAPSHA1} 与 Mojang 对象哈希
        // 不一致，实测 1.21.11 会 hash_mismatch）；HMCL/Axolotl 均以该 MC 的
        // downloads.client_mappings.sha1 为准，由 download_mojmaps 回调负责校验。
        if (has_task(processor, "DOWNLOAD_MOJMAPS") && download_mojmaps) {
            const std::string output_arg = argument_after(processor, "--output");
            if (!output_arg.empty()) {
                util::Result<std::string> output_text = resolve_processor_text(output_arg, env);
                if (output_text.is_error()) {
                    return util::Result<void>::fail(output_text.error());
                }
                if (on_line) {
                    on_line("[SSCL] 自行下载 Mojang 映射（处理器默认联网，已拦截）");
                }
                util::Result<void> downloaded = download_mojmaps(
                    util::path_from_utf8(output_text.value()), std::string());
                if (downloaded.is_error()) {
                    return downloaded;
                }
                for (const Output& output : outputs) {
                    std::error_code exists_ec;
                    if (!std::filesystem::is_regular_file(output.path, exists_ec)) {
                        return util::Result<void>::fail(util::Error::make(
                            "loader.processor_output_mismatch",
                            "MOJMAPS 未生成输出：" + util::path_to_utf8(output.path), {}));
                    }
                }
                continue;
            }
        }

        // 构造并执行命令
        util::Result<ProcessorCommand> command = build_processor_command(processor, env);
        if (command.is_error()) {
            return util::Result<void>::fail(command.error());
        }
        std::string cp;
        for (const std::filesystem::path& entry : command.value().classpath) {
            if (!std::filesystem::is_regular_file(entry)) {
                return util::Result<void>::fail(util::Error::make(
                    "loader.processor_classpath_missing", "处理器 classpath 文件缺失：" +
                        util::path_to_utf8(entry), {}));
            }
            if (!cp.empty()) {
                cp.push_back(';');
            }
            cp += util::path_to_utf8(entry);
        }
        std::vector<std::string> arguments;
        arguments.push_back("-cp");
        arguments.push_back(cp);
        arguments.push_back(command.value().main_class);
        for (const std::string& arg : command.value().args) {
            arguments.push_back(arg);
        }
        util::log_info("[loader] processor " + std::to_string(index) + ": " +
                       command.value().main_class + " (" + std::to_string(arguments.size()) +
                       " args)");

        platform::ProcessOptions options;
        options.timeout_ms = 10 * 60 * 1000;  // 10 分钟（D-039）
        options.stop = stop;
        if (on_line) {
            options.on_output = on_line;
        }
        util::Result<platform::ProcessResult> executed = platform::run_process_ex(
            util::path_from_utf8(env.java_executable), arguments, env.game_dir, options);
        if (executed.is_error()) {
            return util::Result<void>::fail(executed.error());
        }
        if (executed.value().exit_code != 0) {
            return util::Result<void>::fail(util::Error::make(
                "loader.processor_failed",
                "安装器处理器执行失败（退出码 " + std::to_string(executed.value().exit_code) + "）",
                command.value().main_class));
        }
        // 执行后校验输出
        for (const Output& output : outputs) {
            if (!std::filesystem::is_regular_file(output.path)) {
                return util::Result<void>::fail(util::Error::make(
                    "loader.processor_output_mismatch",
                    "处理器未生成输出：" + util::path_to_utf8(output.path),
                    command.value().main_class));
            }
            if (!output.sha1.empty()) {
                util::Result<std::string> actual =
                    util::hash_file_hex(util::HashAlgorithm::kSha1, output.path);
                if (actual.is_error() || !util::hash_hex_equal(actual.value(), output.sha1)) {
                    return util::Result<void>::fail(util::Error::make(
                        "loader.processor_output_mismatch",
                        "处理器输出 SHA1 不一致：" + util::path_to_utf8(output.path),
                        command.value().main_class));
                }
            }
        }
    }
    return util::Result<void>::ok();
}

}  // namespace sscl::core
