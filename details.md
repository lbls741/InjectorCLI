# hybrid-inject

通用 Windows DLL 注入器 CLI。提取自 [SSMT4](https://github.com/Perxenic-Acid/SSMT4) 的
`Run.exe`（3Dmigoto-Injector-V2），去除 3DMigoto/SSMT 专属耦合，以纯命令行参数驱动。

核心是 **Hybrid 双通道注入**，两条通道互补覆盖"启动器链拉起游戏"与"精确 earliest-time 注入"
两类场景（详见下文[工作原理](#工作原理)）。

> ⚠️ 本工具会修改其他进程的内存空间，**必然触发杀软启发式误报**。请仅用于自己拥有的
> 环境（游戏 Mod 加载、调试、自动化测试等）。本仓库只提供源码，安全性自行评估。

## 特性

- **CBT 钩子通道**：全局 `WH_CBT` 钩子由 Windows 主动把载荷 DLL 映射进（启动器链上的）
  目标进程——注入器不需要拿到游戏进程句柄。仅当 `--module` 导出 `CBTProc` 时启用
  （3DMigoto runtime 与本仓库 testpayload 均满足），通用 DLL 自动跳过。
- **NT 远程线程通道**：`CREATE_SUSPENDED` 启动目标 → `NtAllocateVirtualMemory` +
  `NtWriteVirtualMemory` + `NtCreateThreadEx(→LoadLibraryW)`，在目标主线程运行前完成注入。
- **通用导出调用**：注入后在目标进程内远程调用任意 DLL 导出（`--invoke-export`），
  参数为原样字符串；SSMT 的 PluginHost 成为该能力的一个用例（`--plugin-host-config` 别名保留）。
- **JSONL 事件流 + 启动屏障**：`--events jsonl` 输出机器可读启动事件；
  `--launch-barrier` 提供与 SSMT 主程序相同的 Ready/Release 命名事件握手，
  便于任何 GUI 包装器在"已注入、未恢复"的窗口期插入逻辑。
- **等待模式**：不启动进程，轮询目标进程名；配合 `--wait-inject` 可对已在运行的
  进程直接 OpenProcess + 注入。
- **Wine/Proton 就绪**：只使用 Wine 已实现的 Win32/NT API（兼容矩阵见
  [docs/api-compat.md](docs/api-compat.md)）；MinGW/zig 构建、静态链接、单文件免依赖；
  在 Wine 下自动设置 `WINEDLLOVERRIDES`，Windows 下惰性无效果。

## 快速开始

```powershell
# Windows：MSVC / MinGW / zig 三选一，自动探测
.\build.ps1            # 产物在 dist\
```

```bash
# Linux 交叉编译（需要 x86_64-w64-mingw32-g++ 或 zig）
./build.sh
```

产物：`hybrid-inject.exe`（x64）、`testpayload.dll`（验收载荷）、
`hybrid-inject.exe.manifest`（requireAdministrator；Wine 下被忽略）。

## 用法

完整参数见 `hybrid-inject --help`。以下命令均已在本机（Windows 11 x64）实测通过：

```bash
# 1. 启动程序并注入（挂起 + NT 通道；注入后恢复主线程）
hybrid-inject --launch "C:\Path\game.exe" --launch-args "-windowed" \
              --inject "C:\Path\payload.dll"

# 2. CBT 钩子通道（原 3DMigoto ShellExecute 路径；module 须导出 CBTProc）
hybrid-inject --launch "C:\Path\launcher.exe" --module "C:\Path\d3d11.dll"

# 3. 等待模式：轮询已在运行的目标并直接注入
hybrid-inject --wait --wait-inject --target notepad.exe --inject payload.dll

# 4. 注入后远程调用导出（参数原样传递；事件流回显退出码）
hybrid-inject --launch game.exe --inject payload.dll \
              --invoke-export "payload.dll!MyExport" --export-arg "any string"

# 5. 驱动 3DMigoto/SSMT 运行时目录（读取 d3dx.ini [Loader]，CLI 参数优先）
hybrid-inject --ini "C:\Games\GIMI\d3dx.ini"

# 6. SSMT 兼容别名（等价于 inject SSMT-PluginHost.dll + invoke-export SSMTPluginHost_Main）
hybrid-inject --ini d3dx.ini --plugin-host-config "C:\Games\GIMI\PluginHost.json"

# 7. 机器可读事件流（供 GUI 包装器消费）
hybrid-inject --launch game.exe --inject payload.dll --events jsonl
```

### JSONL 事件流

```
launch_started → target_created{pid} → inject_begin/inject_complete{module}
  → export_call_begin/complete{dll,export,exit_code}
  → waiting_before_resume → before_resume → target_resumed
  → runtime_detected → launch_complete
任意阶段失败：launch_error{stage,code,message}
Wine 环境：wine_detected{version}
```

## 工作原理

双通道分别解决两个问题：

| 通道 | 机制 | 解决的问题 |
|---|---|---|
| CBT 钩子 | `SetWindowsHookExW(WH_CBT, CBTProc_of_module, module, 0)`；Windows 把模块映射进所有触发钩子事件的 GUI 进程 | 目标由官方启动器内部拉起时，注入器拿不到游戏进程句柄；由操作系统代为注入 |
| NT 远程线程 | `CREATE_SUSPENDED` → 写远程内存 → `NtCreateThreadEx(LoadLibraryW)` → `ResumeThread` | 精确、最早时机的注入：运行时 DLL 在任何游戏代码执行前就位 |

验证：Toolhelp 模块快照确认载荷已进入目标（枚举被拒的受保护进程按原语义视为成功）。

## 在 Wine / Proton 下运行

注入器需要与目标游戏运行在**同一 wineprefix**、**位数一致**（x64 工具 ↔ x64 游戏）：

```bash
# Steam Proton（在游戏前缀内运行）
protontricks <appid> -c "Z:\path\to\hybrid-inject.exe --launch ... --inject ..."

# Steam Proton 脚本直用
WINEPREFIX=<prefix> PROTONPATH=<proton> ./proton runinprefix hybrid-inject.exe ...

# 裸 wine
WINEPREFIX=<prefix> wine hybrid-inject.exe --wait --wait-inject --target game.exe --inject payload.dll
```

- **d3d11.dll 伪装问题自动处理**：探测到 Wine 时（`ntdll!wine_get_version`），注入器自动为
  子进程设置 `WINEDLLOVERRIDES="<module>=n,b"`，避免伪装系统 DLL 名的载荷被 Wine 内置实现
  抢占；Windows 完全忽略该变量。`--no-winedll-override` 可关闭，`--winedll-override <spec>` 可自定义。
- requireAdministrator manifest 在 Wine 下被忽略（Wine 无 UAC），无副作用。
- 首次在特定 Wine 版本上使用时建议先跑两个实测项（见 docs/api-compat.md 的"empirical"条目）：
  游戏进程内的 CBT 钩子送达、跨进程模块快照。后者失败不影响 NT 通道本身。

## 与上游的关系

- 源码移植自 SSMT4 的 `native/3Dmigoto-Injector-V2`（SSMT-Native 子模块），
  上游为 3DMigoto（bo3b/3Dmigoto → SpectrumQT/XXMI-Libs-Package → Perxenic-Acid/SSMT-3DMigoto-Runtime）。
- 3DMigoto runtime（d3d11.dll）**无需任何修改**即可被本工具驱动：runtime 的
  DllMain 自带目标进程判定（`verify_intended_target`，读 d3dx.ini `[Loader] target`），
  与注入器无其他耦合；`dll_initialization_delay` 等延迟逻辑由 runtime 自行实现。
- 被移除的 SSMT 特化：Player-Tweaks 自动附加、PluginHost 硬编码入口（泛化为
  `--invoke-export`，别名保留）、锁死的 d3dx.ini 依赖（改为 `--ini` 显式启用）。

## 许可证

GPL-3.0（继承上游 3DMigoto / SSMT 链条，见 [LICENSE](LICENSE)）。
