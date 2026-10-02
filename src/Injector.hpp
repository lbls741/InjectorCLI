#pragma once

#include "CliOptions.hpp"
#include "LaunchEvents.hpp"
#include "ProcessUtils.hpp"

#include <optional>
#include <string>
#include <vector>
// 经 Main 合并（CLI + ini）后的最终注入配置；字段均为具体值。
struct ResolvedConfig
{
    std::wstring target;                        // 进程名（basename）
    std::optional<std::wstring> module;         // 运行时/钩子宿主模块
    std::optional<std::wstring> launch;         // 待启动程序；空 = wait 模式
    std::wstring launch_args;
    std::optional<std::wstring> working_dir;
    std::wstring delay = L"5";
    std::vector<std::wstring> inject_dlls;
    std::optional<RemoteExportRequest> invoke_export;
    bool wait_mode = false;
    bool wait_inject = false;                   // wait 模式下找到目标后主动 NT 注入
    bool direct_module = false;                 // 挂起阶段 NT 直注 module（CBT 不可用时的兜底）
    bool use_shell = false;                     // 显式 ShellExecuteW 启动（UAC 启动器；上游已默认统一挂起路径）
    bool plugin_host_compat = false;            // --plugin-host-config 别名：补发 plugin_host_* 事件
    std::optional<std::wstring> launch_barrier_id;
    DWORD verify_timeout_ms = 30000;
    CbtHookMode cbt_mode = CbtHookMode::Auto;
    bool verbose = false;
};

class Injector
{
public:
    // 执行注入流程；返回整体成败。
    static bool Run(const ResolvedConfig &config, LaunchEventEmitter &events);

private:
    enum class CbtInstallResult
    {
        Installed,
        SkippedNoExport,   // 模块存在但未导出 CBTProc
        SkippedNoModule,   // 未提供 module
        SkippedDisabled,   // 用户显式关闭
        Failed             // 加载模块或 SetWindowsHookEx 失败（致命）
    };

    static CbtInstallResult TryInstallGlobalCbtHook(
        const ResolvedConfig &config,
        HHOOK &hookOut,
        LaunchEventEmitter &events);

    static bool LaunchAndInject(
        const ResolvedConfig &config,
        LaunchEventEmitter &events);

    static bool FallbackWaitMode(
        const ResolvedConfig &config,
        LaunchEventEmitter &events);

    static bool WaitBeforeResume(
        const ResolvedConfig &config,
        const PROCESS_INFORMATION &process,
        LaunchEventEmitter &events);

    static void AutoExitOrWait(const std::wstring &delayStr);

    static int ParseDelay(const std::wstring &delayStr);

    static ModuleWaitResult WaitForModuleLoaded(
        DWORD pid,
        const wchar_t *modulePath,
        DWORD timeoutMs,
        bool verbose);

    static bool NtInjectDll(HANDLE process, const wchar_t *dllPath, bool verbose);

    // 在已注入的 DLL 内远程调用导出函数（参数为宽字符串指针）。
    static bool StartRemoteExport(
        HANDLE process,
        DWORD processId,
        const std::wstring &dllPath,
        const std::wstring &exportName,
        const std::optional<std::wstring> &argument,
        bool verbose,
        DWORD &exitCode);

    static std::uintptr_t GetExportRva(
        const std::wstring &dllPath,
        const char *exportName);

    static std::uintptr_t FindRemoteModuleBase(
        DWORD processId,
        const std::wstring &modulePath,
        bool verbose);

    static PVOID WriteRemoteWideString(HANDLE process, const std::wstring &value);

    static bool RunRemoteEntryPoint(
        HANDLE process,
        PVOID entryPoint,
        PVOID parameter,
        DWORD &exitCode);
};
