#include "Injector.hpp"

#include <windows.h>
#include <stdio.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <objbase.h>

#include <filesystem>

#include "ProcessUtils.hpp"

namespace
{

typedef NTSTATUS(NTAPI *pNtCreateThreadEx)(PHANDLE, ACCESS_MASK, PVOID, HANDLE, PVOID, PVOID, ULONG, ULONG_PTR, SIZE_T, SIZE_T, PVOID);
typedef NTSTATUS(NTAPI *pNtAllocateVirtualMemory)(HANDLE, PVOID *, ULONG_PTR, PSIZE_T, ULONG, ULONG);
typedef NTSTATUS(NTAPI *pNtWriteVirtualMemory)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);

pNtCreateThreadEx ResolveNtCreateThreadEx()
{
    return reinterpret_cast<pNtCreateThreadEx>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateThreadEx"));
}

pNtAllocateVirtualMemory ResolveNtAllocateVirtualMemory()
{
    return reinterpret_cast<pNtAllocateVirtualMemory>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtAllocateVirtualMemory"));
}

pNtWriteVirtualMemory ResolveNtWriteVirtualMemory()
{
    return reinterpret_cast<pNtWriteVirtualMemory>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtWriteVirtualMemory"));
}

std::string Narrow(const std::wstring &value)
{
    if (value.empty())
    {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1)
    {
        return {};
    }

    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), required, nullptr, nullptr);
    while (!result.empty() && result.back() == '\0')
    {
        result.pop_back();
    }
    return result;
}

std::wstring AbsolutePath(const std::wstring &path)
{
    wchar_t full[MAX_PATH]{};
    const DWORD length = GetFullPathNameW(path.c_str(), MAX_PATH, full, nullptr);
    if (length == 0 || length >= MAX_PATH)
    {
        return path;
    }
    return std::wstring(full, length);
}

} // namespace

bool Injector::Run(const ResolvedConfig &config, LaunchEventEmitter &events)
{
    printf("[Injector] Starting injection.\n");
    if (!config.launch || config.launch->empty())
    {
        printf("[Injector] No launch configured; waiting for target process.\n");
        return FallbackWaitMode(config, events);
    }

    return LaunchAndInject(config, events);
}

Injector::CbtInstallResult Injector::TryInstallGlobalCbtHook(
    const ResolvedConfig &config,
    HHOOK &hookOut,
    LaunchEventEmitter &events)
{
    hookOut = nullptr;

    if (config.cbt_mode == CbtHookMode::ForceOff)
    {
        events.emit_kv("cbt_hook_skipped", {{"reason", "disabled"}});
        return CbtInstallResult::SkippedDisabled;
    }

    if (!config.module)
    {
        events.emit_kv("cbt_hook_skipped", {{"reason", "no_module"}});
        return CbtInstallResult::SkippedNoModule;
    }

    HMODULE localModule = LoadLibraryW(config.module->c_str());
    if (!localModule)
    {
        printf("[Injector] Failed to load %ls: %lu\n", config.module->c_str(), GetLastError());
        return CbtInstallResult::Failed;
    }

    FARPROC cbt = GetProcAddress(localModule, "CBTProc");
    if (!cbt)
    {
        if (config.cbt_mode == CbtHookMode::ForceOn)
        {
            printf("[Injector] %ls does not export CBTProc.\n", config.module->c_str());
            return CbtInstallResult::Failed;
        }

        printf("[Injector] %ls does not export CBTProc; skipping CBT hook.\n", config.module->c_str());
        events.emit_kv("cbt_hook_skipped", {{"reason", "no_cbtproc_export"}});
        return CbtInstallResult::SkippedNoExport;
    }

    HHOOK hook = SetWindowsHookExW(WH_CBT, reinterpret_cast<HOOKPROC>(cbt), localModule, 0);
    if (!hook)
    {
        printf("[Injector] SetWindowsHookEx failed: %lu\n", GetLastError());
        return CbtInstallResult::Failed;
    }

    hookOut = hook;
    printf("[Injector] Global CBT hook installed from %ls.\n", config.module->c_str());
    events.emit_kv("cbt_hook_installed",
                   {{"module", Narrow(ProcessUtils::BasenameOf(*config.module))}});
    return CbtInstallResult::Installed;
}

bool Injector::LaunchAndInject(
    const ResolvedConfig &config,
    LaunchEventEmitter &events)
{
    printf("[Injector] Launch-and-inject mode selected.\n");

    HHOOK cbtHook = nullptr;
    const CbtInstallResult hookResult = TryInstallGlobalCbtHook(config, cbtHook, events);
    if (hookResult == CbtInstallResult::Failed)
    {
        events.emit_error("prepare", "hook_install_failed", "Failed to install global CBT hook");
        return false;
    }

    if (cbtHook == nullptr && config.module && !config.direct_module)
    {
        // 提示配置矛盾：module 不会经任何通道进入目标。
        printf("[Injector] WARNING: --module set but CBT hook inactive and --direct-module not given;\n"
               "           the module will not be delivered to the target.\n");
    }

    wchar_t runPath[MAX_PATH] = {};
    wchar_t runArgs[MAX_PATH] = {};
    wchar_t workingDir[MAX_PATH] = {};
    wcsncpy_s(runPath, MAX_PATH, config.launch->c_str(), _TRUNCATE);
    wcsncpy_s(runArgs, MAX_PATH, config.launch_args.c_str(), _TRUNCATE);

    // 仅在显式 --shell 时走 ShellExecute（UAC 启动器场景）。
    // 上游 e9af2c7 已统一为挂起注入路径：无论有无注入项都 CreateProcessW 挂起，
    // module 仍由 CBT 钩子负责送达，注入后统一验证。
    if (config.use_shell)
    {
        printf("[Injector] Launching target with ShellExecute: %ls\n", runPath);
        const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const wchar_t *configuredDir = config.working_dir ? config.working_dir->c_str() : runPath;
        wchar_t *workingDirPtr = ProcessUtils::DeduceWorkingDirectory(configuredDir, workingDir);
        HINSTANCE result = ShellExecuteW(nullptr, nullptr, runPath, runArgs, workingDirPtr, SW_SHOWNORMAL);
        const bool launched = (reinterpret_cast<INT_PTR>(result) > 32);
        if (!launched)
        {
            printf("[Injector] ShellExecute failed: %Id\n", reinterpret_cast<INT_PTR>(result));
            events.emit_error("target_launch", "shell_execute_failed", "ShellExecute failed");
        }

        bool ok = false;
        if (launched)
        {
            if (config.module)
            {
                wchar_t moduleFullPath[MAX_PATH] = {};
                if (!GetFullPathNameW(config.module->c_str(), MAX_PATH, moduleFullPath, nullptr))
                {
                    printf("[Injector] Failed to resolve module path: %lu\n", GetLastError());
                    events.emit_error("runtime", "module_resolve_failed", "Failed to resolve module path");
                }
                else
                {
                    ok = ProcessUtils::WaitForTarget(
                        config.target.c_str(),
                        moduleFullPath,
                        ParseDelay(config.delay),
                        true);
                }
            }
            else
            {
                ok = ProcessUtils::WaitForTarget(
                    config.target.c_str(),
                    nullptr,
                    ParseDelay(config.delay),
                    true);
            }
        }

        if (cbtHook)
        {
            UnhookWindowsHookEx(cbtHook);
        }
        if (SUCCEEDED(coInit))
        {
            CoUninitialize();
        }
        if (ok)
        {
            events.emit("runtime_detected");
            events.emit("launch_complete");
        }
        else
        {
            events.emit_error("runtime", "runtime_not_detected", "Target module was not detected");
        }
        return ok;
    }

    wchar_t *filePart = nullptr;
    DWORD pathLen = 0;
    if (config.working_dir)
    {
        pathLen = GetFullPathNameW(config.working_dir->c_str(), MAX_PATH, workingDir, nullptr);
        if (pathLen == 0 || pathLen >= MAX_PATH)
        {
            printf("[Injector] Failed to resolve launch working directory: %lu\n", GetLastError());
            events.emit_error("target_launch", "working_directory_failed", "Failed to resolve launch working directory");
            if (cbtHook)
            {
                UnhookWindowsHookEx(cbtHook);
            }
            return false;
        }
    }
    else
    {
        // 从 launch 路径推断工作目录：截掉文件名部分。
        pathLen = GetFullPathNameW(runPath, MAX_PATH, workingDir, &filePart);
        if (pathLen == 0 || pathLen >= MAX_PATH || !filePart)
        {
            printf("[Injector] Failed to resolve launch working directory: %lu\n", GetLastError());
            events.emit_error("target_launch", "working_directory_failed", "Failed to resolve launch working directory");
            if (cbtHook)
            {
                UnhookWindowsHookEx(cbtHook);
            }
            return false;
        }
        *filePart = L'\0';
    }

    std::wstring cmdLine = L"\"";
    cmdLine += runPath;
    cmdLine += L"\"";
    if (wcslen(runArgs) > 0)
    {
        cmdLine += L" ";
        cmdLine += runArgs;
    }

    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};

    printf("[Injector] Launching target suspended: %ls\n", runPath);
    if (!CreateProcessW(
            runPath,
            cmdBuf.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_SUSPENDED,
            nullptr,
            workingDir,
            &si,
            &pi))
    {
        printf("[Injector] CreateProcess failed: %lu\n", GetLastError());
        events.emit_error("target_launch", "create_process_failed", "CreateProcess failed");
        if (cbtHook)
        {
            UnhookWindowsHookEx(cbtHook);
        }
        return false;
    }

    events.emit_pid("target_created", pi.dwProcessId);

    bool extraOk = true;

    // 注入序列：直注 module（可选）在最前，保证运行时最早就位。
    std::vector<std::wstring> injectionOrder;
    if (config.direct_module && config.module)
    {
        injectionOrder.push_back(*config.module);
    }
    for (const std::wstring &dll : config.inject_dlls)
    {
        injectionOrder.push_back(AbsolutePath(dll));
    }

    for (size_t index = 0; index < injectionOrder.size(); ++index)
    {
        const std::wstring &dllPath = injectionOrder[index];

        printf("[Injector] Injecting DLL %zu/%zu: %ls\n",
               index + 1, injectionOrder.size(), dllPath.c_str());

        events.emit_module("inject_begin", dllPath.c_str());

        const bool currentOk = NtInjectDll(pi.hProcess, dllPath.c_str(), config.verbose);

        if (!currentOk)
        {
            printf("[Injector] Failed to inject DLL: %ls\n", dllPath.c_str());
            events.emit_error("inject", "inject_failed", "Failed to inject DLL");
        }
        else
        {
            events.emit_module("inject_complete", dllPath.c_str());
        }

        extraOk = extraOk && currentOk;
    }

    bool exportOk = true;

    if (config.invoke_export)
    {
        const RemoteExportRequest &request = *config.invoke_export;

        if (config.plugin_host_compat)
        {
            events.emit("plugin_host_begin");
        }

        events.emit_kv("export_call_begin",
                       {{"dll", Narrow(request.dll)},
                        {"export", Narrow(request.export_name)}});

        printf("[Injector] Calling remote export %ls!%ls\n",
               request.dll.c_str(), request.export_name.c_str());

        DWORD exportExitCode = 0;
        exportOk = StartRemoteExport(
            pi.hProcess,
            pi.dwProcessId,
            AbsolutePath(request.dll),
            request.export_name,
            request.argument,
            config.verbose,
            exportExitCode);

        if (exportOk)
        {
            char exitCodeText[16]{};
            snprintf(exitCodeText, sizeof(exitCodeText), "0x%08lX",
                     static_cast<unsigned long>(exportExitCode));
            events.emit_kv("export_call_complete",
                           {{"dll", Narrow(request.dll)},
                            {"export", Narrow(request.export_name)},
                            {"exit_code", exitCodeText}});

            if (config.plugin_host_compat)
            {
                events.emit("plugin_host_ready");
            }
        }
        else
        {
            events.emit_error("export_call", "export_call_failed", "Remote export call failed");
        }
    }

    // 上游 c5d40f5：必需注入失败时绝不恢复目标。半注入状态的游戏会在图形
    // 初始化阶段失败且拿不到有用的注入错误；目标此刻仍挂起，可以确定性地
    // 清理，不会波及已经运行起来的进程。
    if (!extraOk || !exportOk)
    {
        events.emit_error("preflight", "required_injection_failed", "Required DLL injection failed");
        TerminateProcess(pi.hProcess, ERROR_DLL_INIT_FAILED);
        if (cbtHook)
        {
            UnhookWindowsHookEx(cbtHook);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }

    if (!WaitBeforeResume(config, pi, events))
    {
        TerminateProcess(pi.hProcess, ERROR_CANCELLED);
        if (cbtHook)
        {
            UnhookWindowsHookEx(cbtHook);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }

    events.emit("before_resume");
    const DWORD previousSuspendCount = ResumeThread(pi.hThread);

    if (previousSuspendCount == static_cast<DWORD>(-1))
    {
        printf("[Injector] ResumeThread failed: %lu\n", GetLastError());
        events.emit_error("resume", "resume_failed", "ResumeThread failed");
    }
    else
    {
        printf("[Injector] Target resumed. Previous suspend count: %lu\n", previousSuspendCount);
        events.emit("target_resumed");
    }

    // 验证对象优先级：module > 首个注入 DLL。
    std::optional<std::wstring> verifyModule = config.module;
    if (!verifyModule && !config.inject_dlls.empty())
    {
        verifyModule = AbsolutePath(config.inject_dlls.front());
    }

    bool moduleOk = true;
    if (verifyModule)
    {
        const ModuleWaitResult moduleResult = WaitForModuleLoaded(
            pi.dwProcessId, verifyModule->c_str(), config.verify_timeout_ms, config.verbose);

        switch (moduleResult)
        {
        case ModuleWaitResult::Loaded:
            printf("[Injector] Module loaded.\n");
            moduleOk = true;
            events.emit("runtime_detected");
            break;

        case ModuleWaitResult::VerificationDenied:
            printf("[Injector] Module verification was denied; assuming success.\n");
            moduleOk = true;
            events.emit("runtime_detected");
            break;

        case ModuleWaitResult::TimedOut:
            printf("[Injector] Timed out waiting for module.\n");
            moduleOk = false;
            events.emit_error("runtime", "runtime_timeout", "Timed out waiting for module");
            break;
        }
    }

    if (cbtHook)
    {
        UnhookWindowsHookEx(cbtHook);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    const bool ok = moduleOk && extraOk && exportOk;
    if (ok)
    {
        events.emit("launch_complete");
    }
    else
    {
        events.emit_error("launch", "launch_failed", "Launch completed with errors");
    }
    AutoExitOrWait(config.delay);
    return ok;
}

bool Injector::FallbackWaitMode(
    const ResolvedConfig &config,
    LaunchEventEmitter &events)
{
    printf("[Injector] Fallback wait mode selected.\n");

    HHOOK cbtHook = nullptr;
    const CbtInstallResult hookResult = TryInstallGlobalCbtHook(config, cbtHook, events);
    if (hookResult == CbtInstallResult::Failed)
    {
        events.emit_error("prepare", "hook_install_failed", "Failed to install global CBT hook");
        return false;
    }

    std::wstring moduleFullPath;
    if (config.module)
    {
        moduleFullPath = AbsolutePath(*config.module);
    }

    const int delay = ParseDelay(config.delay);
    const bool injectOnFind = config.wait_inject && !config.inject_dlls.empty();
    bool injected = false;
    bool found = false;

    // 轮询等待目标进程；找到后：
    //   * --wait-inject：OpenProcess + NT 注入全部 inject_dlls 并远程调用导出；
    //   * CBT 钩子有效且有 module：等待模块经钩子送达（原 3DMigoto 语义）。
    for (int seconds = 0;; ++seconds)
    {
        const std::optional<DWORD> pid = ProcessUtils::FindProcessIdByName(config.target.c_str());
        if (pid)
        {
            if (injectOnFind && !injected)
            {
                HANDLE process = OpenProcess(
                    PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                    FALSE,
                    *pid);

                if (process)
                {
                    printf("[Injector] Target found (pid %lu); injecting %zu DLL(s).\n",
                           static_cast<unsigned long>(*pid), config.inject_dlls.size());

                    bool allOk = true;
                    for (size_t index = 0; index < config.inject_dlls.size(); ++index)
                    {
                        const std::wstring dllPath = AbsolutePath(config.inject_dlls[index]);
                        events.emit_module("inject_begin", dllPath.c_str());
                        const bool ok = NtInjectDll(process, dllPath.c_str(), config.verbose);
                        if (ok)
                        {
                            events.emit_module("inject_complete", dllPath.c_str());
                        }
                        else
                        {
                            events.emit_error("inject", "inject_failed", "Failed to inject DLL");
                        }
                        allOk = allOk && ok;
                    }

                    if (config.invoke_export)
                    {
                        const RemoteExportRequest &request = *config.invoke_export;
                        if (config.plugin_host_compat)
                        {
                            events.emit("plugin_host_begin");
                        }
                        events.emit_kv("export_call_begin",
                                       {{"dll", Narrow(request.dll)},
                                        {"export", Narrow(request.export_name)}});
                        DWORD exportExitCode = 0;
                        const bool ok = StartRemoteExport(
                            process, *pid, AbsolutePath(request.dll),
                            request.export_name, request.argument,
                            config.verbose, exportExitCode);
                        if (ok)
                        {
                            char exitCodeText[16]{};
                            snprintf(exitCodeText, sizeof(exitCodeText), "0x%08lX",
                                     static_cast<unsigned long>(exportExitCode));
                            events.emit_kv("export_call_complete",
                                           {{"dll", Narrow(request.dll)},
                                            {"export", Narrow(request.export_name)},
                                            {"exit_code", exitCodeText}});
                            if (config.plugin_host_compat)
                            {
                                events.emit("plugin_host_ready");
                            }
                        }
                        else
                        {
                            events.emit_error("export_call", "export_call_failed", "Remote export call failed");
                        }
                        allOk = allOk && ok;
                    }

                    injected = allOk;
                    CloseHandle(process);
                }
                else
                {
                    printf("[Injector] OpenProcess(%lu) failed: %lu\n",
                           static_cast<unsigned long>(*pid), GetLastError());
                }
            }

            if (!config.module || !cbtHook)
            {
                // 无 CBT 通道时，进程出现（且可选注入完成）即达成目标。
                found = !injectOnFind ? true : injected;
                break;
            }
            else if (ProcessUtils::ModuleLoadedInProcess(*pid, moduleFullPath.c_str()))
            {
                found = true;
                break;
            }
        }

        Sleep(1000);
        if (seconds == 3 && !injectOnFind)
        {
            printf("\nStill waiting for the target process to start...\n"
                   "If it does not start automatically, leave this window open and run it manually.\n\n");
        }
    }

    for (int i = delay; i > 0; --i)
    {
        printf("Shutting down injector in %i...\r", i);
        Sleep(1000);
    }
    printf("\n");

    if (cbtHook)
    {
        UnhookWindowsHookEx(cbtHook);
    }

    if (found)
    {
        events.emit("runtime_detected");
        events.emit("launch_complete");
    }
    else
    {
        events.emit_error("runtime", "runtime_not_detected", "Target module was not detected");
    }
    return found;
}

int Injector::ParseDelay(const std::wstring &delayStr)
{
    int delay = 5;
    try
    {
        delay = std::stoi(delayStr);
    }
    catch (...)
    {
        delay = 5;
    }
    return (delay <= 0 && delay != -1) ? 5 : delay;
}

bool Injector::WaitBeforeResume(
    const ResolvedConfig &config,
    const PROCESS_INFORMATION &process,
    LaunchEventEmitter &events)
{
    if (!config.launch_barrier_id)
    {
        return true;
    }

    const std::wstring prefix = L"Local\\SSMT4.Launch." + *config.launch_barrier_id;
    const std::wstring readyName = prefix + L".Ready";
    const std::wstring releaseName = prefix + L".Release";
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName.c_str());
    HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, releaseName.c_str());
    if (!ready || !release)
    {
        events.emit_error("before_resume", "barrier_open_failed", "Could not open launch barrier events");
        if (ready)
            CloseHandle(ready);
        if (release)
            CloseHandle(release);
        return false;
    }

    events.emit("waiting_before_resume");
    const bool signaled = SetEvent(ready) != FALSE;
    const DWORD waitResult = signaled ? WaitForSingleObject(release, INFINITE) : WAIT_FAILED;
    CloseHandle(ready);
    CloseHandle(release);
    if (waitResult != WAIT_OBJECT_0)
    {
        events.emit_error("before_resume", "barrier_wait_failed", "Launch barrier wait failed");
        return false;
    }
    return true;
}

void Injector::AutoExitOrWait(const std::wstring &delayStr)
{
    int delay = ParseDelay(delayStr);
    if (delay == -1)
    {
        printf("\nPress Enter to close...\n");
        getchar();
        return;
    }

    for (int i = delay; i > 0; --i)
    {
        printf("Closing injector in %i...\r", i);
        Sleep(1000);
    }
    printf("\n");
}

ModuleWaitResult Injector::WaitForModuleLoaded(
    DWORD pid,
    const wchar_t *modulePath,
    DWORD timeoutMs,
    bool verbose)
{
    const wchar_t *base = wcsrchr(modulePath, L'\\');
    base = base ? base + 1 : modulePath;

    DWORD waited = 0;
    bool verificationDenied = false;

    while (waited < timeoutMs)
    {
        HANDLE snap = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);

        if (snap == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();

            if (error == ERROR_ACCESS_DENIED)
            {
                if (!verificationDenied)
                {
                    printf("[Injector] Module verification denied; keeping CBT hook active briefly.\n");
                }
                verificationDenied = true;
            }
            else if (verbose)
            {
                printf("[Injector] Module snapshot failed: %lu\n", error);
            }

            Sleep(500);
            waited += 500;

            // 枚举被明确拒绝时无需等满超时：给恢复后的目标与 CBT 钩子约 5 秒即可。
            if (verificationDenied && waited >= 5000)
            {
                return ModuleWaitResult::VerificationDenied;
            }

            continue;
        }

        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);

        if (Module32FirstW(snap, &me))
        {
            do
            {
                if (_wcsicmp(me.szModule, base) == 0)
                {
                    CloseHandle(snap);
                    return ModuleWaitResult::Loaded;
                }
            } while (Module32NextW(snap, &me));
        }

        CloseHandle(snap);

        Sleep(500);
        waited += 500;
    }

    return verificationDenied
               ? ModuleWaitResult::VerificationDenied
               : ModuleWaitResult::TimedOut;
}

bool Injector::NtInjectDll(HANDLE process, const wchar_t *dllPath, bool verbose)
{
    printf("[Injector] NtInjectDll begin: %ls\n", dllPath);

    auto ntAlloc = ResolveNtAllocateVirtualMemory();
    auto ntWrite = ResolveNtWriteVirtualMemory();
    auto ntThread = ResolveNtCreateThreadEx();

    if (!ntAlloc || !ntWrite || !ntThread)
    {
        printf("[Injector] Failed to resolve NT injection functions.\n");
        return false;
    }

    if (verbose)
    {
        printf("[Injector] NT functions resolved.\n");
    }

    SIZE_T len = (wcslen(dllPath) + 1) * sizeof(wchar_t);

    // 同 WriteRemoteWideString：RegionSize 是入出参会被取整到页，写入口径
    // 必须保持为字符串本身长度，否则是随机源越界读。
    SIZE_T regionSize = len;

    PVOID remote = nullptr;
    NTSTATUS status = ntAlloc(
        process,
        &remote,
        0,
        &regionSize,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);

    if (status != 0)
    {
        printf("[Injector] NtAllocateVirtualMemory failed: 0x%lX\n", (unsigned long)status);
        return false;
    }

    if (verbose)
    {
        printf("[Injector] Remote memory allocated at %p.\n", remote);
    }

    SIZE_T written = 0;
    printf("[Injector] Writing DLL path to target...\n");

    status = ntWrite(
        process,
        remote,
        const_cast<wchar_t *>(dllPath),
        len,
        &written);

    if (status != 0 || written != len)
    {
        printf("[Injector] NtWriteVirtualMemory failed: status=0x%lX, written=%zu/%zu\n",
               (unsigned long)status, written, len);
        return false;
    }

    PVOID loadLibrary = reinterpret_cast<PVOID>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));

    if (!loadLibrary)
    {
        printf("[Injector] Failed to resolve LoadLibraryW.\n");
        return false;
    }

    if (verbose)
    {
        printf("[Injector] Local LoadLibraryW address: %p\n", loadLibrary);
    }

    HANDLE thread = nullptr;
    status = ntThread(
        &thread,
        THREAD_ALL_ACCESS,
        nullptr,
        process,
        loadLibrary,
        remote,
        0,
        0,
        0,
        0,
        nullptr);

    if (status != 0 || !thread)
    {
        printf("[Injector] NtCreateThreadEx failed: 0x%lX\n", (unsigned long)status);
        return false;
    }

    if (verbose)
    {
        printf("[Injector] Remote thread created: %p\n", thread);
        printf("[Injector] Waiting for remote LoadLibraryW...\n");
    }

    const DWORD waitResult = WaitForSingleObject(thread, INFINITE);
    if (verbose)
    {
        printf("[Injector] WaitForSingleObject returned: 0x%08lX\n", waitResult);
    }

    DWORD exitCode = 0;
    if (!GetExitCodeThread(thread, &exitCode))
    {
        printf("[Injector] GetExitCodeThread failed: %lu\n", GetLastError());
        CloseHandle(thread);
        return false;
    }

    if (verbose)
    {
        printf("[Injector] Remote thread exit code: 0x%08lX\n", exitCode);
    }

    CloseHandle(thread);

    // 注意：线程退出码是 64 位 HMODULE 的低 32 位；低 32 位恰好为 0 的
    // 概率约 1/65536，届时成功会被误判为失败（与原实现一致，接受该限制）。
    if (exitCode == 0)
    {
        printf("[Injector] Remote LoadLibraryW failed for %ls.\n", dllPath);
        return false;
    }

    printf("[Injector] NtInjectDll success: %ls\n", dllPath);
    return true;
}

std::uintptr_t Injector::GetExportRva(
    const std::wstring &dllPath,
    const char *exportName)
{
    HMODULE module = LoadLibraryW(dllPath.c_str());
    if (!module)
    {
        printf("[Injector] Failed to load local DLL for export lookup: %ls, error=%lu\n",
               dllPath.c_str(), GetLastError());
        return 0;
    }

    FARPROC proc = GetProcAddress(module, exportName);
    if (!proc)
    {
        printf("[Injector] Export not found: %s, error=%lu\n", exportName, GetLastError());
        FreeLibrary(module);
        return 0;
    }

    const std::uintptr_t rva =
        reinterpret_cast<std::uintptr_t>(proc) -
        reinterpret_cast<std::uintptr_t>(module);

    FreeLibrary(module);
    return rva;
}

std::uintptr_t Injector::FindRemoteModuleBase(
    DWORD processId,
    const std::wstring &modulePath,
    bool verbose)
{
    const std::wstring moduleName = ProcessUtils::BasenameOf(modulePath);

    constexpr int maxAttempts = 20;

    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);

        if (snapshot == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();

            if (error == ERROR_BAD_LENGTH)
            {
                Sleep(50);
                continue;
            }

            printf("[Injector] Failed to enumerate remote modules: %lu\n", error);
            return 0;
        }

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);

        if (Module32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szModule, moduleName.c_str()) == 0)
                {
                    const auto base =
                        reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);

                    CloseHandle(snapshot);
                    return base;
                }
            } while (Module32NextW(snapshot, &entry));
        }

        CloseHandle(snapshot);

        Sleep(50);
    }

    printf("[Injector] Remote module not found: %ls\n", moduleName.c_str());
    return 0;
}

bool Injector::StartRemoteExport(
    HANDLE process,
    DWORD processId,
    const std::wstring &dllPath,
    const std::wstring &exportName,
    const std::optional<std::wstring> &argument,
    bool verbose,
    DWORD &exitCode)
{
    const std::uintptr_t remoteBase = FindRemoteModuleBase(processId, dllPath, verbose);
    if (remoteBase == 0)
    {
        return false;
    }

    const std::string exportNameNarrow = Narrow(exportName);
    const std::uintptr_t entryRva = GetExportRva(dllPath, exportNameNarrow.c_str());
    if (entryRva == 0)
    {
        return false;
    }

    const std::uintptr_t remoteEntry = remoteBase + entryRva;

    printf("[Injector] Remote module base: 0x%llX\n",
           static_cast<unsigned long long>(remoteBase));
    printf("[Injector] Export RVA: 0x%llX\n",
           static_cast<unsigned long long>(entryRva));
    printf("[Injector] Remote entry: 0x%llX\n",
           static_cast<unsigned long long>(remoteEntry));

    PVOID remoteArgument = nullptr;
    if (argument)
    {
        // 参数原样传递（任意字符串）；绝对化是 PluginHost 别名在 CLI 层的职责。
        remoteArgument = WriteRemoteWideString(process, *argument);
        if (!remoteArgument)
        {
            return false;
        }
    }

    printf("[Injector] Remote argument: %p\n", remoteArgument);

    return RunRemoteEntryPoint(process, reinterpret_cast<PVOID>(remoteEntry), remoteArgument, exitCode);
}

PVOID Injector::WriteRemoteWideString(HANDLE process, const std::wstring &value)
{
    auto ntAlloc = ResolveNtAllocateVirtualMemory();
    auto ntWrite = ResolveNtWriteVirtualMemory();

    if (!ntAlloc || !ntWrite)
    {
        printf("[Injector] Failed to resolve NT memory functions.\n");
        return nullptr;
    }

    SIZE_T size = (value.size() + 1) * sizeof(wchar_t);

    // NtAllocateVirtualMemory 的 RegionSize 是入出参：返回时被向上取整到页粒度。
    // 必须用独立副本接分配尺寸，后续只写字符串本身的字节数 —— 直接复用会被
    // 取整值放大成越界读，在源页未映射时报 STATUS_PARTIAL_COPY（上游潜伏 bug）。
    SIZE_T regionSize = size;

    PVOID remote = nullptr;
    NTSTATUS status = ntAlloc(
        process,
        &remote,
        0,
        &regionSize,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);

    if (status != 0)
    {
        printf("[Injector] Failed to allocate remote string: 0x%lX\n", (unsigned long)status);
        return nullptr;
    }

    SIZE_T written = 0;
    status = ntWrite(process, remote, const_cast<wchar_t *>(value.c_str()), size, &written);

    if (status != 0 || written != size)
    {
        printf("[Injector] Failed to write remote string: status=0x%lX, written=%zu/%zu\n",
               (unsigned long)status, written, size);
        return nullptr;
    }

    return remote;
}

bool Injector::RunRemoteEntryPoint(
    HANDLE process,
    PVOID entryPoint,
    PVOID parameter,
    DWORD &exitCode)
{
    auto ntThread = ResolveNtCreateThreadEx();
    if (!ntThread)
    {
        printf("[Injector] Failed to resolve NtCreateThreadEx.\n");
        return false;
    }

    HANDLE thread = nullptr;
    const NTSTATUS status = ntThread(
        &thread,
        THREAD_ALL_ACCESS,
        nullptr,
        process,
        entryPoint,
        parameter,
        0, 0, 0, 0,
        nullptr);

    if (status != 0 || !thread)
    {
        printf("[Injector] Failed to create remote export thread: 0x%lX\n", (unsigned long)status);
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(thread, INFINITE);
    if (waitResult != WAIT_OBJECT_0)
    {
        printf("[Injector] Waiting for remote export failed: 0x%08lX\n", waitResult);
        CloseHandle(thread);
        return false;
    }

    if (!GetExitCodeThread(thread, &exitCode))
    {
        printf("[Injector] GetExitCodeThread failed: %lu\n", GetLastError());
        CloseHandle(thread);
        return false;
    }

    CloseHandle(thread);
    return true;
}
