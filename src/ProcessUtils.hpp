#pragma once

#include <windows.h>
#include <stdio.h>
#include <tlhelp32.h>
#include <optional>
#include <set>
#include <string>

enum class ModuleWaitResult
{
    Loaded,
    VerificationDenied,
    TimedOut
};

// 目标进程定位与注入验证工具。
// 全部基于 Toolhelp32 快照 —— Wine/Proton 对进程快照有完整实现，
// 跨进程模块快照亦有实现（底层与 EnumProcessModules 同机制），
// 但属于相对薄弱路径，调用方需处理快照失败的情况。
class ProcessUtils
{
public:
    static wchar_t *DeduceWorkingDirectory(const wchar_t *setting, wchar_t dir[MAX_PATH])
    {
        wchar_t *filePart = nullptr;
        DWORD ret = GetFullPathNameW(setting, MAX_PATH, dir, &filePart);
        if (!ret || ret >= MAX_PATH)
        {
            return nullptr;
        }

        DWORD attributes = GetFileAttributesW(dir);
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            return nullptr;
        }

        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) && filePart)
        {
            *filePart = L'\0';
        }

        printf("Using working directory: \"%S\"\n", dir);
        return dir;
    }

    // 轮询等待名为 target 的进程出现，并（可选地）确认 modulePath 已加载其中。
    // modulePath 为 nullptr 时仅检查进程存在。
    // 与原 3DMigoto loader 行为一致：未找到前无限轮询（wait 模式语义），
    // 找到后执行 delay 秒倒计时（期间持续复查）。
    static bool WaitForTarget(const wchar_t *target, const wchar_t *modulePath, int delay, bool launched)
    {
        bool found = false;
        for (int seconds = 0;; ++seconds)
        {
            found = CheckForRunningTarget(target, modulePath) || found;
            if (found)
            {
                break;
            }

            Sleep(1000);
            if (launched && seconds == 3)
            {
                printf("\nStill waiting for the target process to start...\n"
                       "If it does not launch automatically, leave this window open and run it manually.\n\n");
            }
        }

        for (int i = delay; i > 0; --i)
        {
            printf("Shutting down injector in %i...\r", i);
            Sleep(1000);
            found = CheckForRunningTarget(target, modulePath) || found;
        }
        printf("\n");

        return found;
    }

    // 从 exe 全路径提取文件名（含扩展名）。
    static std::wstring BasenameOf(const std::wstring &path)
    {
        const size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? path : path.substr(slash + 1);
    }

    // 返回第一个名字匹配（不含路径）的进程 PID；找不到返回 nullopt。
    static std::optional<DWORD> FindProcessIdByName(const wchar_t *name)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            printf("Unable to enumerate processes: %lu\n", GetLastError());
            return std::nullopt;
        }

        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        std::optional<DWORD> found;
        if (Process32FirstW(snapshot, &pe))
        {
            do
            {
                if (_wcsicmp(pe.szExeFile, name) == 0)
                {
                    found = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &pe));
        }

        CloseHandle(snapshot);
        return found;
    }

    // 判断模块是否已加载进指定进程（路径完全匹配才视为命中）。
    // 模块枚举被拒绝（受保护进程）时按 Windows/Wine 共有语义视为已加载。
    static bool ModuleLoadedInProcess(DWORD pid, const wchar_t *modulePath)
    {
        PROCESSENTRY32W pe = {};
        pe.th32ProcessID = pid;
        return VerifyInjection(&pe, modulePath, false);
    }

private:
    static bool CheckForRunningTarget(const wchar_t *target, const wchar_t *modulePath)
    {
        std::wstring targetName = BasenameOf(target);
        if (targetName.empty())
        {
            return false;
        }

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            printf("Unable to enumerate processes: %lu\n", GetLastError());
            return false;
        }

        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (!Process32FirstW(snapshot, &pe))
        {
            printf("Unable to enumerate processes: %lu\n", GetLastError());
            CloseHandle(snapshot);
            return false;
        }

        bool ok = false;
        static std::set<DWORD> seenPids;
        do
        {
            if (_wcsicmp(pe.szExeFile, targetName.c_str()) != 0)
            {
                continue;
            }

            ok = VerifyInjection(&pe, modulePath, !seenPids.count(pe.th32ProcessID)) || ok;
            seenPids.insert(pe.th32ProcessID);
        } while (Process32NextW(snapshot, &pe));

        CloseHandle(snapshot);
        return ok;
    }

    // modulePath 为 nullptr 时，只要进程存在即视为命中。
    static bool VerifyInjection(PROCESSENTRY32W *pe, const wchar_t *modulePath, bool logName)
    {
        if (!modulePath)
        {
            printf("Target process found (%lu): %S\n", pe->th32ProcessID, pe->szExeFile);
            return true;
        }

        const wchar_t *moduleBase = wcsrchr(modulePath, L'\\');
        moduleBase = moduleBase ? moduleBase + 1 : modulePath;

        HANDLE snapshot = INVALID_HANDLE_VALUE;
        do
        {
            snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pe->th32ProcessID);
        } while (snapshot == INVALID_HANDLE_VALUE && GetLastError() == ERROR_BAD_LENGTH);

        if (snapshot == INVALID_HANDLE_VALUE)
        {
            DWORD lastError = GetLastError();
            if (lastError == ERROR_ACCESS_DENIED)
            {
                printf("%lu: target process found, but module verification was denied; assuming success.\n", pe->th32ProcessID);
                return true;
            }
            printf("%S (%lu): unable to verify module load: %lu\n", pe->szExeFile, pe->th32ProcessID, lastError);
            return false;
        }

        MODULEENTRY32W me = {};
        me.dwSize = sizeof(me);
        if (!Module32FirstW(snapshot, &me))
        {
            DWORD lastError = GetLastError();
            CloseHandle(snapshot);
            if (lastError == ERROR_ACCESS_DENIED)
            {
                printf("%lu: module verification denied; assuming success.\n", pe->th32ProcessID);
                return true;
            }
            printf("%S (%lu): unable to enumerate modules: %lu\n", pe->szExeFile, pe->th32ProcessID, lastError);
            return false;
        }

        wchar_t exeDir[MAX_PATH] = {};
        wcscpy_s(exeDir, MAX_PATH, me.szExePath);
        wchar_t *exeSlash = wcsrchr(exeDir, L'\\');
        if (exeSlash)
        {
            exeSlash[1] = L'\0';
        }

        if (logName)
        {
            printf("Target process found (%lu): %S\n", pe->th32ProcessID, me.szExePath);
        }

        bool loaded = false;
        do
        {
            if (_wcsicmp(me.szModule, moduleBase) != 0)
            {
                continue;
            }

            if (_wcsicmp(me.szExePath, modulePath) == 0)
            {
                printf("%lu: module loaded.\n", pe->th32ProcessID);
                loaded = true;
                continue;
            }

            // 同名模块已加载但来自游戏目录本身 —— 视为冲突（常见于残留的旧 runtime 副本）。
            wchar_t moduleDir[MAX_PATH] = {};
            wcscpy_s(moduleDir, MAX_PATH, me.szExePath);
            wchar_t *moduleSlash = wcsrchr(moduleDir, L'\\');
            if (moduleSlash)
            {
                moduleSlash[1] = L'\0';
            }

            if (_wcsicmp(exeDir, moduleDir) == 0)
            {
                printf("\nWARNING: Found another copy of the module loaded from the game directory:\n%S\n"
                       "Please remove that copy and try again.\n\n",
                       me.szExePath);
                CloseHandle(snapshot);
                return false;
            }
        } while (Module32NextW(snapshot, &me));

        CloseHandle(snapshot);
        return loaded;
    }
};
