#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// 命令行参数模型。所有注入配置字段均为 optional：
// CLI 显式给出的值优先于 --ini 兼容配置，ini 只填充 CLI 未指定的字段。
enum class CbtHookMode
{
    Auto,      // module 存在且导出 CBTProc 才安装钩子
    ForceOn,   // 强制安装，导出缺失视为错误
    ForceOff   // 从不安装
};

enum class WineOverrideMode
{
    Auto,      // 探测到 Wine 时自动为 module 设置 WINEDLLOVERRIDES
    Explicit,  // 使用用户提供的 override 串（无论是否在 Wine 下）
    Disabled   // 完全不动环境变量
};

struct RemoteExportRequest
{
    std::wstring dll;          // 已（或将）注入到目标进程的 DLL 路径
    std::wstring export_name;  // 要远程调用的导出名
    std::optional<std::wstring> argument;  // 以宽字符串指针形式传入导出
};

struct CliOptions
{
    // ---- 注入配置 ----
    std::optional<std::wstring> target;      // 验证/等待的进程名
    std::optional<std::wstring> module;      // 运行时/钩子宿主模块（CBT 通道 + 加载验证）
    std::optional<std::wstring> launch;      // 待启动程序；空 + 非 wait 模式无意义
    std::optional<std::wstring> launch_args;
    std::optional<std::wstring> working_dir; // 缺省时由 launch 路径推断
    std::optional<std::wstring> delay;       // 验证通过后的驻留秒数，"-1" = 等待回车
    std::vector<std::wstring> inject_dlls;   // NT 通道注入列表（可重复 --inject）
    std::optional<RemoteExportRequest> invoke_export;
    bool wait_mode = false;                  // --wait：不启动进程，轮询已运行目标
    bool wait_inject = false;                // --wait-inject：wait 模式找到目标后主动 NT 注入
    bool direct_module = false;              // --direct-module：挂起阶段 NT 直注 module

    // ---- 集成 ----
    bool machine_readable = false;
    bool test_mode = false;
    bool verbose = false;
    std::optional<std::wstring> launch_barrier_id;
    std::wstring mutex_name = L"Local\\hybrid-inject";
    DWORD verify_timeout_ms = 30000;
    CbtHookMode cbt_mode = CbtHookMode::Auto;
    WineOverrideMode wine_override = WineOverrideMode::Auto;
    std::optional<std::wstring> wine_override_spec;

    // ---- 配置来源 ----
    std::optional<std::filesystem::path> ini_path;
    bool plugin_host_alias = false;          // 经 --plugin-host-config 别名进入（补发兼容事件）
};

class CliParseError : public std::runtime_error
{
public:
    explicit CliParseError(const std::string &message)
        : std::runtime_error(message)
    {
    }
};

// 解析 argv；抛出 CliParseError 表示参数非法。
// help_requested 为 true 时仅表示用户请求了 --help。
CliOptions ParseCliOptions(
    int argc,
    wchar_t *argv[],
    bool &help_requested);

// 与 SSMT 原启动器的兼容别名展开后的说明（用于 --help）。
const char *GetUsageText();
