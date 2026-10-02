#include "CliOptions.hpp"

#include <memory>

namespace
{

std::wstring_view ArgvView(wchar_t *argv[], int index)
{
    return std::wstring_view{argv[index]};
}

std::filesystem::path GetExecutableDirectory()
{
    wchar_t executablePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        return {};
    }
    return std::filesystem::path{executablePath}.parent_path();
}

// --plugin-host-config 兼容别名：等价于
//   --inject <exe目录>\SSMT-PluginHost.dll
//   --invoke-export SSMT-PluginHost.dll!SSMTPluginHost_Main --export-arg <config>
// 事件流上仍会补发 plugin_host_* 事件，保持与 SSMT 原启动器的对接能力。
void ApplyPluginHostAlias(CliOptions &options, const std::wstring &configPath)
{
    const std::filesystem::path hostPath =
        GetExecutableDirectory() / L"SSMT-PluginHost.dll";

    const std::wstring hostName = hostPath.wstring();

    const bool alreadyListed = std::any_of(
        options.inject_dlls.begin(),
        options.inject_dlls.end(),
        [&](const std::wstring &dll)
        { return _wcsicmp(dll.c_str(), hostName.c_str()) == 0; });

    if (!alreadyListed)
    {
        options.inject_dlls.push_back(hostName);
    }

    // 原实现（StartPluginHost）向导出传递的是绝对化的配置路径。
    options.invoke_export = RemoteExportRequest{
        hostName,
        L"SSMTPluginHost_Main",
        std::filesystem::absolute(configPath).wstring()};

    options.plugin_host_alias = true;
}

} // namespace

const char *GetUsageText()
{
    return
        "hybrid-inject - hybrid DLL injector (global CBT hook + NT remote thread)\n"
        "\n"
        "Launch mode (choose one):\n"
        "  --launch <exe>            Start <exe> (suspended) and inject. This is the default\n"
        "                            unified path: CreateProcessW(CREATE_SUSPENDED) + NT\n"
        "                            injection + resume, mirroring upstream.\n"
        "  --shell                   Launch via ShellExecuteW instead (CBT-hook only;\n"
        "                            needed when the target launcher requires UAC elevation).\n"
        "  --wait                    Wait mode: poll for an already-running target.\n"
        "  --wait-inject             Wait mode: when the target appears, OpenProcess + NT-inject\n"
        "                            all --inject DLLs (no CBT hook needed for delivery).\n"
        "\n"
        "Payload:\n"
        "  --inject <dll>            DLL to inject via NT remote thread (repeatable).\n"
        "  --module <dll>            Runtime/hook-host module (CBT channel + load verification).\n"
        "                            Required for --wait.\n"
        "  --invoke-export <dll!Export>[--export-arg <string>]\n"
        "                            After injection, call <Export> in the target via a\n"
        "                            remote thread, passing the arg as a wide string.\n"
        "                            <dll> is added to the inject list automatically.\n"
        "  --direct-module           Also NT-inject --module itself before resume\n"
        "                            (fallback when CBT delivery is unavailable).\n"
        "\n"
        "Target & verification:\n"
        "  --target <name>           Target process name (default: basename of --launch).\n"
        "  --timeout <ms>            Module-load verification timeout (default 30000).\n"
        "  --delay <sec>             Injector linger after verification (default 5; -1 = wait for Enter).\n"
        "  --working-dir <dir>       Working directory for the launched process.\n"
        "  --launch-args <args>      Arguments passed to the launched process.\n"
        "\n"
        "Integration:\n"
        "  --events jsonl | --machine-readable\n        Emit JSONL launch events on stdout (human logs move to stderr).\n"
        "  --launch-barrier <id>     Signal Local\\SSMT4.Launch.<id>.Ready and wait for .Release\n"
        "                            while the target is created+injected but still suspended.\n"
        "  --mutex-name <name>       Single-instance mutex name (default Local\\hybrid-inject).\n"
        "  --verbose                 Enable verbose internal logging.\n"
        "  --quiet                   Detach from the console before doing anything: no console\n"
        "                            window is created for GUI launches (FreeConsole).\n"
        "                            Pair with --events jsonl under file redirection.\n"
        "  --test-mode               Replay the full event sequence without touching processes.\n"
        "\n"
        "CBT hook:\n"
        "  --cbt-hook / --no-cbt-hook\n        Default auto: install the global WH_CBT hook only when\n"
        "                            --module exists and exports CBTProc.\n"
        "\n"
        "Wine / Proton:\n"
        "  --winedll-override <spec> Explicit WINEDLLOVERRIDES value for child processes.\n"
        "  --no-winedll-override     Never touch WINEDLLOVERRIDES.\n"
        "                            Default auto: under Wine, set \"<module>=n,b\" so a payload\n"
        "                            masquerading as a system DLL (d3d11.dll) is loaded as native.\n"
        "\n"
        "Compatibility:\n"
        "  --ini <path>              Read defaults from d3dx.ini [Loader] (3DMigoto compat).\n"
        "                            Explicit CLI flags override ini values.\n"
        "  --plugin-host-config <p>  SSMT alias for --inject SSMT-PluginHost.dll +\n"
        "                            --invoke-export ...!SSMTPluginHost_Main --export-arg <p>.\n"
        "\n"
        "  -h, --help                Show this help.\n";
}

CliOptions ParseCliOptions(
    int argc,
    wchar_t *argv[],
    bool &help_requested)
{
    CliOptions options;
    help_requested = false;

    std::optional<RemoteExportRequest> pendingExport;

    for (int i = 1; i < argc; ++i)
    {
        const std::wstring_view arg = ArgvView(argv, i);

        auto requireValue = [&](const char *flagName) -> std::wstring_view
        {
            if (i + 1 >= argc)
            {
                throw CliParseError(std::string("option ") + flagName + " requires a value");
            }
            return ArgvView(argv, ++i);
        };

        if (arg == L"-h" || arg == L"--help")
        {
            help_requested = true;
            return options;
        }

        if (arg == L"--launch")
        {
            options.launch = std::wstring{requireValue("--launch")};
            continue;
        }

        if (arg == L"--launch-args")
        {
            options.launch_args = std::wstring{requireValue("--launch-args")};
            continue;
        }

        if (arg == L"--working-dir")
        {
            options.working_dir = std::wstring{requireValue("--working-dir")};
            continue;
        }

        if (arg == L"--wait")
        {
            options.wait_mode = true;
            continue;
        }

        if (arg == L"--inject")
        {
            std::wstring dll{requireValue("--inject")};
            if (!dll.empty())
            {
                options.inject_dlls.push_back(std::move(dll));
            }
            continue;
        }

        if (arg == L"--module")
        {
            options.module = std::wstring{requireValue("--module")};
            continue;
        }

        if (arg == L"--target")
        {
            options.target = std::wstring{requireValue("--target")};
            continue;
        }

        if (arg == L"--invoke-export")
        {
            const std::wstring spec{requireValue("--invoke-export")};
            const size_t sep = spec.find(L'!');
            if (sep == std::wstring::npos || sep == 0 || sep + 1 == spec.size())
            {
                throw CliParseError("--invoke-export expects <dll!ExportName>");
            }

            RemoteExportRequest request;
            request.dll = spec.substr(0, sep);
            request.export_name = spec.substr(sep + 1);
            pendingExport = request;
            continue;
        }

        if (arg == L"--export-arg")
        {
            if (!pendingExport)
            {
                throw CliParseError("--export-arg must follow --invoke-export");
            }
            pendingExport->argument = std::wstring{requireValue("--export-arg")};
            continue;
        }

        if (arg == L"--wait-inject")
        {
            options.wait_inject = true;
            continue;
        }

        if (arg == L"--shell")
        {
            options.use_shell = true;
            continue;
        }

        if (arg == L"--quiet")
        {
            options.quiet = true;
            continue;
        }

        if (arg == L"--direct-module")
        {
            options.direct_module = true;
            continue;
        }

        if (arg == L"--cbt-hook")
        {
            options.cbt_mode = CbtHookMode::ForceOn;
            continue;
        }

        if (arg == L"--no-cbt-hook")
        {
            options.cbt_mode = CbtHookMode::ForceOff;
            continue;
        }

        if (arg == L"--timeout")
        {
            const std::wstring value{requireValue("--timeout")};
            const DWORD parsed = static_cast<DWORD>(wcstoul(value.c_str(), nullptr, 10));
            if (parsed == 0)
            {
                throw CliParseError("--timeout expects a positive millisecond value");
            }
            options.verify_timeout_ms = parsed;
            continue;
        }

        if (arg == L"--delay")
        {
            options.delay = std::wstring{requireValue("--delay")};
            continue;
        }

        if (arg == L"--machine-readable")
        {
            options.machine_readable = true;
            continue;
        }

        if (arg == L"--events")
        {
            const std::wstring value{requireValue("--events")};
            if (value != L"jsonl")
            {
                throw CliParseError("--events requires jsonl");
            }
            options.machine_readable = true;
            continue;
        }

        if (arg == L"--launch-barrier")
        {
            options.launch_barrier_id = std::wstring{requireValue("--launch-barrier")};
            continue;
        }

        if (arg == L"--mutex-name")
        {
            std::wstring name{requireValue("--mutex-name")};
            if (name.empty())
            {
                throw CliParseError("--mutex-name requires a non-empty name");
            }
            options.mutex_name = std::move(name);
            continue;
        }

        if (arg == L"--winedll-override")
        {
            options.wine_override = WineOverrideMode::Explicit;
            options.wine_override_spec = std::wstring{requireValue("--winedll-override")};
            continue;
        }

        if (arg == L"--no-winedll-override")
        {
            options.wine_override = WineOverrideMode::Disabled;
            continue;
        }

        if (arg == L"--verbose")
        {
            options.verbose = true;
            continue;
        }

        if (arg == L"--test-mode")
        {
            options.test_mode = true;
            continue;
        }

        if (arg == L"--ini")
        {
            options.ini_path = std::filesystem::path{requireValue("--ini")};
            continue;
        }

        if (arg == L"--plugin-host-config")
        {
            ApplyPluginHostAlias(options, std::wstring{requireValue("--plugin-host-config")});
            continue;
        }

        throw CliParseError("unknown option: " + std::string(arg.begin(), arg.end()));
    }

    if (pendingExport)
    {
        options.invoke_export = pendingExport;

        // 远程调用要求 DLL 已在目标进程中，未显式 --inject 时自动补上。
        const bool listed = std::any_of(
            options.inject_dlls.begin(),
            options.inject_dlls.end(),
            [&](const std::wstring &dll)
            { return _wcsicmp(dll.c_str(), pendingExport->dll.c_str()) == 0; });

        if (!listed)
        {
            options.inject_dlls.push_back(pendingExport->dll);
        }
    }

    return options;
}
