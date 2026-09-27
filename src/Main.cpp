#include <windows.h>
#include <stdio.h>
#include <io.h>
#include <string_view>
#include <string>

#include "CliOptions.hpp"
#include "IniCompat.hpp"
#include "Injector.hpp"
#include "LaunchEvents.hpp"
#include "ProcessUtils.hpp"
#include "WineCompat.hpp"

namespace
{

bool MachineOutputRequested(int argc, wchar_t *argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::wstring_view{argv[i]} == L"--machine-readable" ||
            (std::wstring_view{argv[i]} == L"--events" && i + 1 < argc &&
             std::wstring_view{argv[i + 1]} == L"jsonl"))
        {
            return true;
        }
    }
    return false;
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

// 将 CLI 与 d3dx.ini 合并并填充默认值，产出注入器最终配置。
// 合并规则：CLI 显式给出的字段优先；ini 只填充 CLI 未指定的字段。
bool ResolveConfig(
    const CliOptions &cli,
    ResolvedConfig &config,
    LaunchEventEmitter &events)
{
    if (cli.ini_path)
    {
        try
        {
            const IniCompatValues ini = LoadIniCompat(*cli.ini_path);

            if (!cli.target)
                config.target = ini.target;
            if (!cli.module)
                config.module = ini.module;
            if (!cli.launch)
                config.launch = ini.launch;
            if (!cli.launch_args)
                config.launch_args = ini.launch_args;
            if (cli.inject_dlls.empty())
                config.inject_dlls = ini.inject_dlls;
            if (!cli.delay)
                config.delay = ini.delay;
        }
        catch (const IniCompatError &error)
        {
            events.emit_error("configuration", "d3dx_ini_invalid", "Failed to parse d3dx.ini");
            printf("[Injector] Failed to parse d3dx.ini: %s\n%s\n",
                   error.what(), Narrow(error.detail()).c_str());
            return false;
        }
    }

    if (cli.target)
        config.target = *cli.target;
    if (cli.module)
        config.module = *cli.module;
    if (cli.launch)
        config.launch = *cli.launch;
    if (cli.launch_args)
        config.launch_args = *cli.launch_args;
    if (cli.working_dir)
        config.working_dir = *cli.working_dir;
    if (cli.delay)
        config.delay = *cli.delay;
    if (!cli.inject_dlls.empty())
        config.inject_dlls = cli.inject_dlls;
    if (cli.invoke_export)
        config.invoke_export = *cli.invoke_export;

    config.wait_mode = cli.wait_mode;
    config.wait_inject = cli.wait_inject;
    config.direct_module = cli.direct_module;
    config.plugin_host_compat = cli.plugin_host_alias;
    config.launch_barrier_id = cli.launch_barrier_id;
    config.verify_timeout_ms = cli.verify_timeout_ms;
    config.cbt_mode = cli.cbt_mode;
    config.verbose = cli.verbose;

    // 默认值与派生。
    if (config.delay.empty())
    {
        config.delay = L"5";
    }

    if (config.launch && !config.launch->empty() && config.target.empty())
    {
        config.target = ProcessUtils::BasenameOf(*config.launch);
    }

    if (!config.target.empty())
    {
        // 允许传入完整路径；等待/验证只关心进程名。
        config.target = ProcessUtils::BasenameOf(config.target);
    }

    return true;
}

bool ValidateConfig(
    const ResolvedConfig &config,
    LaunchEventEmitter &events)
{
    if (config.wait_mode && config.launch && !config.launch->empty())
    {
        events.emit_error("arguments", "invalid_arguments", "--wait cannot be combined with --launch");
        printf("ERROR: --wait cannot be combined with --launch\n");
        return false;
    }

    if (!config.wait_mode && (!config.launch || config.launch->empty()))
    {
        events.emit_error("arguments", "invalid_arguments", "nothing to do: pass --launch or --wait");
        printf("ERROR: nothing to do: pass --launch or --wait (see --help)\n");
        return false;
    }

    if (config.wait_mode && !config.module)
    {
        // wait 模式下 CBT 是唯一的被动注入通道；wait-inject 亦需要 module 作为
        // 验证对象或明确放弃验证。
        if (!config.wait_inject)
        {
            events.emit_error("arguments", "invalid_arguments", "--wait requires --module");
            printf("ERROR: --wait requires --module (the hook-host DLL), see --help\n");
            return false;
        }
    }

    if (config.wait_mode && config.target.empty())
    {
        events.emit_error("arguments", "invalid_arguments", "--wait requires --target");
        printf("ERROR: --wait requires --target (process name to poll for)\n");
        return false;
    }

    if (!config.wait_mode && config.target.empty())
    {
        events.emit_error("arguments", "invalid_arguments", "--target could not be derived from --launch");
        printf("ERROR: target process name is empty; pass --target explicitly\n");
        return false;
    }

    return true;
}

int RunTestMode(const ResolvedConfig &config, LaunchEventEmitter &events)
{
    // launch_started 已由 wmain 发出。

    if (config.plugin_host_compat && config.invoke_export && config.invoke_export->argument)
    {
        // 兼容别名携带的 PluginHost 配置路径保持原有存在性校验。
        const std::wstring configPath = *config.invoke_export->argument;
        if (!std::filesystem::exists(configPath))
        {
            events.emit_error("preflight", "plugin_host_config_missing", "PluginHost config does not exist");
            return EXIT_FAILURE;
        }
    }

    events.emit_pid("target_created", 0);

    for (const std::wstring &dll : config.inject_dlls)
    {
        events.emit_module("inject_begin", dll.c_str());
        events.emit_module("inject_complete", dll.c_str());
    }

    if (config.invoke_export)
    {
        if (config.plugin_host_compat)
        {
            events.emit("plugin_host_begin");
            events.emit("plugin_host_ready");
        }
        else
        {
            events.emit_kv("export_call_begin",
                           {{"dll", Narrow(config.invoke_export->dll)},
                            {"export", Narrow(config.invoke_export->export_name)}});
            events.emit_kv("export_call_complete",
                           {{"dll", Narrow(config.invoke_export->dll)},
                            {"export", Narrow(config.invoke_export->export_name)},
                            {"exit_code", "0x00000000"}});
        }
    }
    else if (config.plugin_host_compat)
    {
        events.emit("plugin_host_disabled");
    }

    events.emit("before_resume");
    events.emit("target_resumed");
    events.emit("runtime_detected");
    events.emit("launch_complete");
    return EXIT_SUCCESS;
}

} // namespace

int wmain(
    int argc,
    wchar_t *argv[])
{
    // --machine-readable / --events jsonl：保留 CRT 已重定向的 stdout 句柄给
    // 事件流，随后把人工日志移到 stderr。Start-Process 的文件重定向不保证
    // GetStdHandle 与 CRT 指向同一对象。
    const bool machineRequested = MachineOutputRequested(argc, argv);
    HANDLE machine_output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (machineRequested)
    {
        const auto stdout_handle = _get_osfhandle(_fileno(stdout));
        if (stdout_handle != -1)
        {
            HANDLE duplicate = nullptr;
            if (DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(stdout_handle),
                GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
                machine_output = duplicate;
        }
        _dup2(_fileno(stderr), _fileno(stdout));
    }
    LaunchEventEmitter events(machineRequested, machine_output);

    CliOptions cli;
    bool helpRequested = false;
    try
    {
        cli = ParseCliOptions(argc, argv, helpRequested);
    }
    catch (const CliParseError &error)
    {
        events.emit_error("arguments", "invalid_arguments", error.what());
        printf("ERROR: %s\n%s", error.what(), GetUsageText());
        return EXIT_FAILURE;
    }

    if (helpRequested)
    {
        printf("%s", GetUsageText());
        return EXIT_SUCCESS;
    }

    ResolvedConfig config;
    if (!ResolveConfig(cli, config, events))
    {
        return EXIT_FAILURE;
    }

    if (config.verbose)
    {
        if (const std::optional<std::string> wine = WineCompat::DetectWine())
        {
            printf("[Injector] Wine/Proton detected (version %s).\n", wine->c_str());
        }
    }

    events.emit("launch_started");

    // Wine 探测事件 + 子进程 DLL override。override 只对后续 CreateProcess 的
    // 子进程有意义（wait 模式目标进程已存在），Windows 下该环境变量无效果。
    if (const std::optional<std::string> wine = WineCompat::DetectWine())
    {
        events.emit_kv("wine_detected", {{"version", *wine}});

        if (!config.wait_mode && config.launch && !config.launch->empty())
        {
            std::wstring overrideSpec;
            if (cli.wine_override == WineOverrideMode::Explicit && cli.wine_override_spec)
            {
                overrideSpec = *cli.wine_override_spec;
            }
            else if (cli.wine_override == WineOverrideMode::Auto && config.module)
            {
                // 载荷伪装系统 DLL 名（如 d3d11.dll）时，避免被 Wine 内置实现抢占。
                overrideSpec = ProcessUtils::BasenameOf(*config.module) + L"=n,b";
            }

            if (!overrideSpec.empty())
            {
                const std::wstring applied = WineCompat::ApplyDllOverride(overrideSpec);
                printf("[Injector] WINEDLLOVERRIDES=%ls\n", applied.c_str());
            }
        }
    }

    if (cli.test_mode)
    {
        return RunTestMode(config, events);
    }

    if (!ValidateConfig(config, events))
    {
        return EXIT_FAILURE;
    }

    HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, cli.mutex_name.c_str());
    if (!instanceMutex)
    {
        events.emit_error("preflight", "loader_mutex_failed", "Failed to create loader mutex");
        printf("ERROR: Failed to create loader mutex: %lu\n", GetLastError());
        return EXIT_FAILURE;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        events.emit_error("preflight", "loader_already_running", "Another loader instance is already running");
        printf("ERROR: Another injector instance is already running (mutex %ls). Please close it and try again\n",
               cli.mutex_name.c_str());
        CloseHandle(instanceMutex);
        return EXIT_FAILURE;
    }

    printf("\n------------------------------- hybrid-inject -------------------------------\n\n");
    printf("Hybrid DLL injector: global CBT hook + NT remote-thread injection.\n");
    printf("Based on the 3Dmigoto loader (GPLv3), decoupled as a standalone CLI tool.\n\n");

    if (config.invoke_export)
    {
        printf("[Injector] Remote export call enabled: %ls!%ls\n",
               config.invoke_export->dll.c_str(),
               config.invoke_export->export_name.c_str());
    }
    if (!config.launch || config.launch->empty())
    {
        printf("[Injector] Wait mode: polling for target process '%ls'.\n",
               config.target.c_str());
    }

    const bool ok = Injector::Run(config, events);

    CloseHandle(instanceMutex);

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
