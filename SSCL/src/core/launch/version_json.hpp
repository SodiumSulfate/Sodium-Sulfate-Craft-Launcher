#pragma once

// Minecraft 版本 JSON 模型、解析与 inheritsFrom 合并
// （knowledge/version-json.md：字段语义与合并顺序源自 HMCL 行为数据）。
// 关键：合并时 libraries 是**子前父后**，arguments 是**父前子后**（顺序相反）。
// rules 求值：无规则→允许；有规则→默认 DISALLOW，取最后一条生效规则的 action。

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "util/result.hpp"

namespace sscl::core {

struct OsInfo {
    std::string name;     // "windows" / "osx" / "linux"
    std::string version;  // 系统版本号（规则 version 正则全匹配的对象）
    std::string arch;     // "x86_64" / "x86" / "arm64"
};

struct RuleOsRestriction {
    std::string name;      // "windows"/"osx"/"linux"，支持 "win-x86" 复合形式
    std::string version;   // 正则全匹配；空 = 不限
    std::string arch;      // 正则全匹配；空 = 不限
};

struct Rule {
    std::string action;  // "allow" / "disallow"
    bool has_os = false;
    RuleOsRestriction os;
    std::map<std::string, bool> features;  // 与当前 features 逐项完全相等才生效
};

// 参数条目：字符串 或 {rules, value[]}（异构数组，version-json.md §6）
struct RuledValues {
    std::vector<Rule> rules;
    std::vector<std::string> values;
};

struct ArgumentList {
    std::vector<RuledValues> entries;
};

struct LibraryArtifact {
    std::string path;  // 相对 libraries 目录的路径；空则由 name 推导
    std::string url;
    std::string sha1;
    std::int64_t size = -1;
};

struct LibraryExtract {
    std::vector<std::string> exclude;  // 路径前缀
};

struct Library {
    std::string name;                            // Maven 坐标 group:artifact:version[:classifier][@ext]
    std::string url;                             // Maven 仓库基址（downloads 缺失时拼接；HMCL Library.computePath）
    std::map<std::string, std::string> natives;  // os → classifier（可含 ${arch}）
    std::map<std::string, LibraryArtifact> classifiers;  // 描述符 → artifact（旧式）
    bool has_artifact = false;
    LibraryArtifact artifact;  // downloads.artifact（新式）
    bool has_extract = false;
    LibraryExtract extract;
    std::vector<Rule> rules;
    std::string hint;  // "local"（MultiMC 兼容）
    // download_url 为空但 declared artifact 存在 ⇒ 由安装器处理器生成（禁止下载、只校验存在）
    bool local_only = false;
};

struct AssetIndexRef {
    std::string id;
    std::string url;
    std::string sha1;
};

struct DownloadEntry {
    std::string sha1;
    std::string url;
    std::int64_t size = -1;
};

struct VersionDownloads {
    bool has_client = false;
    DownloadEntry client;
    bool has_client_mappings = false;  // Mojang 官方映射（Forge DOWNLOAD_MOJMAPS 用它）
    DownloadEntry client_mappings;
};

struct LoggingFile {
    std::string id;
    std::string url;
    std::string sha1;
    std::int64_t size = -1;
};

struct LoggingConfig {
    std::string argument;
    LoggingFile file;
};

struct JavaVersionInfo {
    std::string component;
    int major_version = 0;
};

// Forge/NeoForge 安装器处理器（仅安装期使用；启动期忽略，见 design/mod-loader-install.md §6.6）。
struct InstallerProcessor {
    std::vector<std::string> sides;      // 空 = 全部；含 "client" 才在客户端安装时执行
    std::string jar;                     // Maven 坐标
    std::vector<std::string> classpath;  // Maven 坐标
    std::vector<std::string> args;       // 含 {VAR} / [coords] / 'literal'
    std::map<std::string, std::string> outputs;  // 路径模板 → SHA1 模板
};

// install_profile.data 的单侧取值（client/server）。
struct SidedData {
    std::string client;
    std::string server;
};

// SSCL 注入的安装元数据（`sscl` 字段；D-037）。
// 仅用于诊断、版本隔离/Mod 目标推断，启动期忽略。
struct VersionSSCL {
    bool has = false;
    std::string loader;             // fabric / forge / neoforge / quilt
    std::string loader_version;
    std::string minecraft_version;
    std::string installer_url;
};

struct VersionJson {
    std::string id;
    std::string inherits_from;
    std::string main_class;
    std::string minecraft_arguments;  // 旧格式
    std::string jar;
    std::string assets;
    std::string type;
    // PCL 版本识别用（2.99 McVersion.GetVanillaName）：
    std::string client_version;  // PCL 下载的版本
    std::string release_time;    // ISO8601；未知为空
    std::string time;            // HMCL patches 判定用
    std::vector<Library> libraries;
    VersionSSCL sscl;

    bool has_arguments = false;  // 新格式 {game:[], jvm:[]}
    ArgumentList game_arguments;
    ArgumentList jvm_arguments;

    bool has_asset_index = false;
    AssetIndexRef asset_index;
    bool has_downloads = false;
    VersionDownloads downloads;
    bool has_logging = false;
    LoggingConfig logging;
    bool has_java_version = false;
    JavaVersionInfo java_version;

    // 安装器数据（仅 Forge/NeoForge 安装期使用；启动期忽略）
    bool has_processors = false;
    std::vector<InstallerProcessor> processors;
    bool has_installer_data = false;
    std::map<std::string, SidedData> installer_data;

    int minimum_launcher_version = 0;
};

// 解析版本 JSON 文本（nlohmann-json）。
util::Result<VersionJson> parse_version_json(const std::string& json_text);

// 合并：child = 子版本（this），parent = 已解析的父版本。
// 严格按 version-json.md §2：libraries 子前父后；arguments 父前子后。
VersionJson merge_version(const VersionJson& child, const VersionJson& parent);

// rules 求值（version-json.md §3）：返回该组规则下是否允许。
bool rules_allow(const std::vector<Rule>& rules, const OsInfo& os,
                 const std::map<std::string, bool>& features);

// 从参数条目收集当前环境适用的字符串。
std::vector<std::string> collect_arguments(const ArgumentList& list, const OsInfo& os,
                                           const std::map<std::string, bool>& features);

// natives classifier 选择（${arch} → 32/64）；无 natives 返回空串。
std::string select_native_classifier(const Library& library, const std::string& os_name,
                                     bool is_64bit);

// Maven 坐标 → 相对路径：group/artifact/version/artifact-version[-classifier].jar
std::string library_relative_path(const std::string& maven_name);

}  // namespace sscl::core
