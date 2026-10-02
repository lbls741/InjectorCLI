// hybrid-inject 验收测试目标：最小 GUI 进程（真实窗口、无 Store 别名转发）。
// 用途：CBT 钩子通道需要一个"创建窗口"的目标进程来触发钩子事件，
// 而 Windows 11 的 notepad 是应用执行别名（挂起启动的是转发 stub），
// 不适合作为挂起注入路径的验收对象。
#include <windows.h>

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"hybrid-inject-testtarget";
    RegisterClassW(&wc);

    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"hybrid-inject test target",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               120, 120, 360, 240,
                               nullptr, nullptr, hInstance, nullptr);
    (void)wnd;

    // 消息循环保持窗口存活，直到被关闭或外部 taskkill。
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
