// hybrid-inject 通用测试载荷：
//   * 导出空 CBTProc —— 可作为 --module 验证 CBT 钩子通道（与 3DMigoto runtime 相同契约）；
//   * DllMain 向 %TEMP%\hybrid-inject-test.log 追加一行（进程 exe 路径），
//     作为自动化验收的送达证据；
//   * 环境变量 HYBRID_TEST_MSGBOX=1 时额外弹窗（人工演示用；注意远程线程
//     会被模态弹窗阻塞直到手动关闭）。
// 改编自 SSMT-Native 的 MessageBoxTestDll（GPLv3）。
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" __declspec(dllexport) LRESULT CALLBACK CBTProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// 通用导出调用（--invoke-export）的验收目标：
// 立即返回（注入器会 INFINITE 等待该线程），返回值可从事件流核对。
extern "C" __declspec(dllexport) DWORD __cdecl TestExport(const wchar_t *argument)
{
    wchar_t tempDir[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tempDir) != 0)
    {
        HANDLE file = CreateFileW((std::wstring(tempDir) + L"hybrid-inject-test.log").c_str(),
                                  FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            char line[MAX_PATH * 2]{};
            const int written = snprintf(line, sizeof(line),
                                         "TestExport called, argument = %ls\n",
                                         argument ? argument : L"(null)");
            if (written > 0)
            {
                SetFilePointer(file, 0, nullptr, FILE_END);
                DWORD unused = 0;
                WriteFile(file, line, static_cast<DWORD>(written), &unused, nullptr);
            }
            CloseHandle(file);
        }
    }
    return 42;
}

static void AppendArrivalRecord()
{
    wchar_t processPath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, processPath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        return;
    }

    wchar_t tempDir[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tempDir) == 0)
    {
        return;
    }

    HANDLE file = CreateFileW((std::wstring(tempDir) + L"hybrid-inject-test.log").c_str(),
                              FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[MAX_PATH + 64]{};
    const int written = snprintf(line, sizeof(line),
                                 "[%04hu-%02hu-%02hu %02hu:%02hu:%02hu] payload arrived in %ls\n",
                                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                                 processPath);
    if (written > 0)
    {
        SetFilePointer(file, 0, nullptr, FILE_END);
        DWORD unused = 0;
        WriteFile(file, line, static_cast<DWORD>(written), &unused, nullptr);
    }
    CloseHandle(file);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        AppendArrivalRecord();
        if (GetEnvironmentVariableW(L"HYBRID_TEST_MSGBOX", nullptr, 0) != 0)
        {
            MessageBoxW(NULL, L"payload loaded", L"hybrid-inject test", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        }
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
