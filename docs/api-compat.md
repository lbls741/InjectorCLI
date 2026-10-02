# Wine / Proton API 兼容矩阵

hybrid-inject 使用的全部系统调用在 Wine/Proton 中的实现状态。
逐项核对过 Wine 源码（gitlab.winehq.org/wine/wine，Wine 10.x 时代）与 WineHQ Bugzilla。

结论：**无任何 API 处于 stub/未实现状态**。主工具链选 MinGW-w64 兼容编译器
（Wine 自身 PE 侧即用 MinGW-w64 构建），静态链接 libc++/libgcc，单文件无运行时依赖。

| API | 用途（源码位置） | Wine 状态 | 备注 | 置信度 |
|---|---|---|---|---|
| `NtAllocateVirtualMemory` / `NtWriteVirtualMemory` | 跨进程写 DLL 路径（Injector.cpp NtInjectDll） | ✅ 完整实现（`dlls/ntdll/unix/virtual.c`） | 权限规则同 Windows（PROCESS_VM_*） | 高 |
| `NtCreateThreadEx` | 远程线程（LoadLibraryW / 导出调用） | ✅ Wine 3.4（2018）起实现（bug 44616） | 同位宽远程线程无限制；64→32 位跨位宽 Windows/Wine 均不可行 | 高 |
| `CreateProcessW(CREATE_SUSPENDED)` + `ResumeThread` | 挂起启动目标（Injector.cpp LaunchAndInject） | ✅ 核心 API | suspended-before-entry 语义与 Windows 一致 | 高 |
| `ShellExecuteW` | 无 --inject 时的启动路径 | ✅（shell32/shlexec.c） | `runas` 动词在 Wine 下不做真实提权 | 高 |
| `SetWindowsHookExW(WH_CBT)` 全局钩子 | 钩子通道（Injector.cpp TryInstallGlobalCbtHook） | ✅（user32/hook.c `get_hook_proc` 在接收进程内 `LoadLibraryExW` 钩子 DLL） | 机制与 Windows 相同：钩子 DLL 被映射进会话内触发事件的 GUI 进程 | 高（机制）/ 中（游戏内送达，见下） |
| `CreateToolhelp32Snapshot(SNAPPROCESS)` + `Process32FirstW/NextW` | 目标进程轮询（ProcessUtils.hpp） | ✅ 服务端实现，Cheat Engine 等长期验证 | | 高 |
| `CreateToolhelp32Snapshot(SNAPMODULE)` + `Module32FirstW/NextW`（跨进程） | 注入验证 / 远程模块基址（Injector.cpp WaitForModuleLoaded / FindRemoteModuleBase） | ✅ 与 psapi `EnumProcessModules` 同机制，有 conformance tests | 新 WoW64 下 32 位模块枚举有已知缺口（Wine MR 9371）；本工具 x64↔x64 场景不受影响 | 中高 |
| `CreateMutexW` + `ERROR_ALREADY_EXISTS` | 单实例互斥体（Main.cpp） | ✅（kernelbase/sync.c，wineserver 对象） | `Local\` 前缀即一个 wineprefix 作用域 | 高 |
| `OpenEventW` / `SetEvent` / `WaitForSingleObject`（命名事件） | launch-barrier（Injector.cpp WaitBeforeResume） | ✅ | 跨进程同 wineprefix 可用 | 高 |
| `DuplicateHandle` / `GetStdHandle` / `WriteFile` | JSONL 事件流句柄分离（Main.cpp / LaunchEvents.hpp） | ✅ | | 高 |
| msvcrt `_dup2` / `_fileno` / `_get_osfhandle` | stdout/stderr 分流（Main.cpp） | ✅（msvcrt.spec 导出 + msvcrt/file.c） | MinGW msvcrt flavor 与 ucrtbase 均实现 | 高 |
| `GetProcAddress(GetModuleHandleW("ntdll.dll"), ...)` | NT 函数动态解析（Injector.cpp） | ✅ Wine ntdll 导出全量 Nt*/Zw* | 亦用于 `wine_get_version` 探测（仅 Wine 有此导出） | 高 |
| `GetExitCodeThread` | 远程 LoadLibraryW 成败判断 | ✅ | 退出码 = HMODULE 低 32 位（x64 截断，与 Windows 相同的既有限制） | 高 |
| `SetEnvironmentVariableW` / 子进程环境继承 | WINEDLLOVERRIDES 自动设置（WineCompat.cpp） | ✅ | | 高 |
| `FreeConsole` | `--quiet` 脱离控制台（Main.cpp） | ✅（kernelbase/console.c） | GUI 启动时随进程创建的控制台随脱离关闭；已有终端窗口保留；文件/管道重定向句柄不受影响 | 高 |
| `requireAdministrator` manifest（外部 .manifest 文件） | Windows 提权语义 | ✅ 被解析但忽略 | Wine 无 UAC：不提权也不报错，直接以当前令牌运行 | 高 |

## 需要按 Wine 版本实测的两个点（empirical）

1. **游戏进程内的 CBT 钩子送达**。钩子 DLL 进 GUI 进程的机制在 Wine 源码中与 Windows
   一致，且自 2008 年起有公开成功案例；但 D3D 全屏游戏（高消息量、无边框全屏）下的
   `WH_CBT` 送达是整个矩阵中实测样本最少的路径。验证方法：
   `hybrid-inject --launch <game.exe> --module testpayload.dll --events jsonl`，
   观察 `runtime_detected` 与 `%TEMP%\hybrid-inject-test.log`。
   兜底：CBT 不可用时改用 NT 通道（`--inject` / `--direct-module`）。
2. **跨进程 SNAPMODULE**。影响注入验证与远程导出基址定位（`--invoke-export`）。
   NT 注入本身不依赖它。失败症状是 30s 验证超时 / export 调用报 remote module not found；
   注入实际可能已成功。缓解：`--direct-module` + 信任 `inject_complete` 事件。

## 参考来源

- Wine `dlls/ntdll/unix/virtual.c`、`dlls/user32/hook.c`、`dlls/kernelbase/{process,thread,toolhelp,sync,handle,console}.c`、`dlls/ntdll/ntdll.spec`、`dlls/msvcrt/msvcrt.spec`（gitlab.winehq.org/wine/wine）
- WineHQ Bugzilla：[44616（NtCreateThreadEx）](https://bugs.winehq.org/show_bug.cgi?id=44616)、[2981（全局钩子历史）](https://bugs.winehq.org/show_bug.cgi?id=2981)、[39262（runas）](https://bugs.winehq.org/show_bug.cgi?id=39262)
- WineHQ：[Building Wine（MinGW PE 构建）](https://gitlab.winehq.org/wine/wine/-/wikis/Building-Wine)、[FAQ（无 UAC）](https://wiki.winehq.org/FAQ)
- Proton 场景实践：[proton runinprefix 指南](https://steamcommunity.com/sharedfiles/filedetails/?id=2896924859)、[protontricks](https://github.com/Matoking/protontricks)、[XXMI-Launcher](https://github.com/SpectrumQT/XXMI-Launcher)
- 同类先例：[haunt（MinGW 交叉编译注入器，Wine 下可用）](https://github.com/sebastian-heinz/haunt)、[decant](https://github.com/RobbyV2/decant)
