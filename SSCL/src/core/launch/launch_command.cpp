#include "core/launch/launch_command.hpp"

#include <utility>

#include "platform/file_utils.hpp"
#include "platform/process.hpp"
#include "platform/win_error.hpp"
#include "util/string_utils.hpp"

namespace sscl::core {

namespace {

constexpr std::size_t kMaxCommandLength = 30000;  // <32767 留余量（§7.1）

std::string acp_encoding_name() {
    switch (::GetACP()) {
        case 936:
            return "GBK";
        case 932:
            return "MS932";
        case 949:
            return "MS949";
        case 950:
            return "MS950";
        case 65001:
            return "UTF-8";
        case 1252:
            return "windows-1252";
        default:
            return "UTF-8";
    }
}

// D-118：支持成对的半角双引号分组（"C:\Program Files\..."）——外置登录的
// -javaagent:<jar>=<server> 里 jar 路径可能含空格。引号本身不进入 token，
// 拼接命令行时由 platform::quote_process_arg 统一补引号。
std::vector<std::string> tokenize_arguments(const std::string& text) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_quotes = false;
    bool has_token = false;
    for (const char c : text) {
        if (c == '"') {
            in_quotes = !in_quotes;
            has_token = true;
            continue;
        }
        if (!in_quotes && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) {
            if (has_token && !current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            has_token = false;
            continue;
        }
        current.push_back(c);
        has_token = true;
    }
    if (has_token && !current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

std::string quote_argfile_token(const std::string& token) {
    // 未加引号时 Java argfile 解析器把 \ 当转义符，含 \ 也必须引号包裹；
    // '#' 开头是注释行、空白/引号需包裹（JDK @-file 规则）。
    const bool needs_quotes =
        token.empty() || token.find_first_of(" \t\n\v\"'\\#") != std::string::npos;
    if (!needs_quotes) {
        return token;
    }
    std::string out;
    out.push_back('"');
    for (const char c : token) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

}  // namespace

std::string replace_placeholders(const std::string& text,
                                 const std::map<std::string, std::string>& values) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '$' && i + 1 < text.size() && text[i + 1] == '{') {
            const std::size_t end = text.find('}', i + 2);
            if (end != std::string::npos) {
                const std::string key = text.substr(i, end - i + 1);  // 键包含 ${}
                const auto it = values.find(key);
                if (it != values.end()) {
                    out += it->second;
                } else {
                    out += key;  // 未知占位符原样保留
                }
                i = end + 1;
                continue;
            }
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

std::string build_classpath(const VersionJson& version, const LaunchContext& context) {
    std::map<std::string, bool> features = context.features;
    if (context.settings.width > 0 && context.settings.height > 0) {
        features["has_custom_resolution"] = true;
    }

    std::string classpath;
    for (const Library& library : version.libraries) {
        if (!rules_allow(library.rules, context.os, features)) {
            continue;
        }
        std::string relative;
        if (library.has_artifact && !library.artifact.path.empty()) {
            relative = library.artifact.path;
        } else {
            relative = library_relative_path(library.name);
        }
        if (relative.empty()) {
            continue;
        }
        const std::filesystem::path absolute = context.paths.libraries_dir / relative;
        if (!classpath.empty()) {
            classpath += ';';
        }
        classpath += sscl::util::path_to_utf8(absolute);
    }
    // 客户端 jar 追加在末尾（launch-args.md §2）
    if (!context.paths.version_jar.empty()) {
        if (!classpath.empty()) {
            classpath += ';';
        }
        classpath += sscl::util::path_to_utf8(context.paths.version_jar);
    }
    return classpath;
}

std::map<std::string, std::string> build_placeholder_map(const VersionJson& version,
                                                         const LaunchContext& context,
                                                         const std::string& classpath) {
    std::map<std::string, std::string> values;
    const LaunchAccount& account = context.account;
    const LaunchPaths& paths = context.paths;

    values["${auth_player_name}"] = account.player_name;
    values["${auth_session}"] = account.access_token;
    values["${auth_access_token}"] = account.access_token;
    values["${auth_uuid}"] = account.uuid_compact;
    values["${version_name}"] =
        context.version_name.empty() ? version.id : context.version_name;
    values["${profile_name}"] = "Minecraft";
    values["${version_type}"] =
        context.version_type.empty() ? version.type : context.version_type;
    values["${game_directory}"] = sscl::util::path_to_utf8(paths.game_dir);
    values["${user_type}"] = account.user_type;
    // C-7（Axolotl 方案）：--clientId 传应用 Client ID，--xuid 无 Xbox xuid 时传 "0"。
    values["${clientid}"] = context.client_id;
    values["${auth_xuid}"] = context.auth_xuid;
    values["${assets_index_name}"] =
        version.has_asset_index ? version.asset_index.id : version.assets;
    values["${user_properties}"] = account.user_properties;
    values["${resolution_width}"] = std::to_string(context.settings.width);
    values["${resolution_height}"] = std::to_string(context.settings.height);
    values["${library_directory}"] = sscl::util::path_to_utf8(paths.libraries_dir);
    values["${libraries_directory}"] = sscl::util::path_to_utf8(paths.libraries_dir);
    values["${classpath_separator}"] = ";";
    values["${primary_jar}"] = sscl::util::path_to_utf8(paths.version_jar);
    values["${primary_jar_name}"] = sscl::util::path_to_utf8(paths.version_jar.filename());
    values["${language}"] = context.settings.language;
    values["${file_separator}"] = "\\";
    values["${classpath}"] = classpath;
    values["${game_assets}"] = sscl::util::path_to_utf8(paths.assets_dir);
    values["${assets_root}"] = sscl::util::path_to_utf8(paths.assets_dir);
    values["${natives_directory}"] = sscl::util::path_to_utf8(paths.natives_dir);
    values["${launcher_name}"] = context.launcher_name;
    values["${launcher_version}"] = context.launcher_version;
    return values;
}

util::Result<std::filesystem::path> write_argfile(const std::filesystem::path& file,
                                                  const std::vector<std::string>& arguments) {
    std::string content;
    for (const std::string& argument : arguments) {
        content += quote_argfile_token(argument);
        content.push_back('\n');  // 每行一个 token，避开空白歧义
    }
    util::Result<void> written = platform::write_file_atomic(file, content);
    if (written.is_error()) {
        return util::Result<std::filesystem::path>::fail(std::move(written.error()));
    }
    return util::Result<std::filesystem::path>::ok(file);
}

util::Result<LaunchPlan> build_launch_plan(const VersionJson& version,
                                           const LaunchContext& context) {
    if (version.main_class.empty()) {
        return util::Result<LaunchPlan>::fail(
            util::Error::make("launch.main_class_missing", "版本缺少 mainClass", version.id));
    }

    std::map<std::string, bool> features = context.features;
    if (context.settings.width > 0 && context.settings.height > 0) {
        features["has_custom_resolution"] = true;
    }

    const std::string classpath = build_classpath(version, context);
    const std::map<std::string, std::string> placeholders =
        build_placeholder_map(version, context, classpath);

    std::vector<std::string> raw;

    // 1. 内存（用户参数优先，这里取 settings）
    raw.push_back("-Xmx" + std::to_string(context.settings.memory_mb) + "m");
    raw.push_back("-Xms" + std::to_string(context.settings.memory_mb) + "m");

    // 2. 编码（launch-args.md §1.2）
    const std::string encoding = acp_encoding_name();
    raw.push_back("-Dfile.encoding=" + encoding);
    const int java_major = context.settings.java_major_version;
    if (java_major >= 18) {
        raw.push_back("-Dstdout.encoding=" + encoding);
        raw.push_back("-Dstderr.encoding=" + encoding);
    } else if (java_major == 8) {
        raw.push_back("-Dsun.stdout.encoding=" + encoding);
        raw.push_back("-Dsun.stderr.encoding=" + encoding);
    }

    // 3. log4j（§1.5）
    if (version.has_logging && context.paths.has_log4j) {
        if (!version.logging.argument.empty()) {
            std::string argument = version.logging.argument;
            const std::size_t pos = argument.find("${path}");
            if (pos != std::string::npos) {
                argument.replace(pos, 7, sscl::util::path_to_utf8(context.paths.log4j_config));
            }
            raw.push_back(argument);
        } else {
            raw.push_back("-Dlog4j.configurationFile=" +
                          sscl::util::path_to_utf8(context.paths.log4j_config));
        }
    }

    // 4./5. JVM 参数：版本 arguments.jvm 优先，否则默认集（§1.6-1.7）
    std::vector<std::string> jvm =
        collect_arguments(version.jvm_arguments, context.os, features);
    if (jvm.empty()) {
        jvm.push_back(
            "-XX:HeapDumpPath=MojangTricksIntelDriversForPerformance_javaw.exe_minecraft.exe.heapdump");
        if (context.os.name == "windows" && context.os.version.rfind("10.", 0) == 0) {
            // 仅 Windows 10（^10\.），不含 Win11（§1.6）
            jvm.push_back("-Dos.name=Windows 10");
            jvm.push_back("-Dos.version=10.0");
        }
        jvm.push_back("-Djava.library.path=${natives_directory}");
        jvm.push_back("-Dminecraft.launcher.brand=${launcher_name}");
        jvm.push_back("-Dminecraft.launcher.version=${launcher_version}");
        jvm.push_back("-cp");
        jvm.push_back("${classpath}");
    }
    raw.insert(raw.end(), jvm.begin(), jvm.end());

    // V5 每版本 GC 设置（放在 mainClass 之前）。
    switch (context.settings.gc_mode) {
        case 1:
            raw.push_back("-XX:+UseZGC");
            break;
        case 2:
            raw.push_back("-XX:+UseZGC");
            if (java_major == 21 || java_major == 22) {
                raw.push_back("-XX:+ZGenerational");
            }
            break;
        case 3:
            raw.push_back("-XX:+UseG1GC");
            break;
        case 4:
            raw.push_back("-XX:+UseG1GC");
            raw.push_back("-XX:+ParallelRefProcEnabled");
            raw.push_back("-XX:MaxGCPauseMillis=200");
            raw.push_back("-XX:+UnlockExperimentalVMOptions");
            raw.push_back("-XX:G1NewSizePercent=20");
            raw.push_back("-XX:G1MaxNewSizePercent=40");
            break;
        case 0:
        case 5:
        default:
            break;
    }

    // V5 每版本追加 JVM 参数。
    if (!context.settings.jvm_args.empty()) {
        const std::vector<std::string> extra = tokenize_arguments(context.settings.jvm_args);
        raw.insert(raw.end(), extra.begin(), extra.end());
    }

    // 6. mainClass
    raw.push_back(version.main_class);

    // 7. 游戏参数
    std::vector<std::string> game;
    if (!version.minecraft_arguments.empty()) {
        game = tokenize_arguments(version.minecraft_arguments);
        // 旧格式追加 DEFAULT_GAME_ARGUMENTS
        const auto it = features.find("has_custom_resolution");
        if (it != features.end() && it->second) {
            game.push_back("--width");
            game.push_back("${resolution_width}");
            game.push_back("--height");
            game.push_back("${resolution_height}");
        }
    } else {
        game = collect_arguments(version.game_arguments, context.os, features);
    }
    if (!context.settings.game_args.empty()) {
        const std::vector<std::string> extra = tokenize_arguments(context.settings.game_args);
        game.insert(game.end(), extra.begin(), extra.end());
    }
    if (!context.settings.server_enter.empty()) {
        if (version.release_time > "2023-05-04") {
            game.push_back("--quickPlayMultiplayer");
            game.push_back(context.settings.server_enter);
        } else {
            const std::size_t colon = context.settings.server_enter.find(':');
            if (colon != std::string::npos) {
                game.push_back("--server");
                game.push_back(context.settings.server_enter.substr(0, colon));
                game.push_back("--port");
                game.push_back(context.settings.server_enter.substr(colon + 1));
            } else {
                game.push_back("--server");
                game.push_back(context.settings.server_enter);
            }
        }
    }
    raw.insert(raw.end(), game.begin(), game.end());

    // 8. 全屏（§1.11）
    if (context.settings.fullscreen) {
        raw.push_back("--fullscreen");
    }

    // 统一占位符替换
    LaunchPlan plan;
    // D-104：启动后行为随计划一起交给 launch_game（不参与占位符替换）。
    plan.process_priority = context.settings.process_priority;
    plan.maximize_window = context.settings.maximize_window;
    plan.window_title = context.settings.window_title;
    plan.arguments.reserve(raw.size());
    for (const std::string& argument : raw) {
        plan.arguments.push_back(replace_placeholders(argument, placeholders));
    }

    // 命令行组装；超长写 @argfile（§7.1）
    std::string command_line = platform::quote_process_arg(
        sscl::util::wide_to_utf8(context.paths.java_exe.wstring()));
    std::size_t length = command_line.size();
    for (const std::string& argument : plan.arguments) {
        length += platform::quote_process_arg(argument).size() + 1;
    }

    if (length > kMaxCommandLength) {
        const std::filesystem::path argfile = context.paths.game_dir / "sscl-args.txt";
        util::Result<std::filesystem::path> written = write_argfile(argfile, plan.arguments);
        if (written.is_error()) {
            return util::Result<LaunchPlan>::fail(std::move(written.error()));
        }
        plan.used_argfile = true;
        plan.argfile_path = written.value();
        plan.command_line =
            command_line + " @\"" + sscl::util::path_to_utf8(written.value()) + "\"";
    } else {
        for (const std::string& argument : plan.arguments) {
            command_line.push_back(' ');
            command_line += platform::quote_process_arg(argument);
        }
        plan.command_line = command_line;
    }

    return util::Result<LaunchPlan>::ok(std::move(plan));
}

}  // namespace sscl::core
