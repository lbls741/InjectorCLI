#pragma once

#include <optional>
#include <string>

// Wine/Proton 兼容支持。
// 依赖的接口均为 Wine 长期稳定实现：
//   * ntdll!wine_get_version —— Wine 探测的标准手法；
//   * SetEnvironmentVariableW/GetEnvironmentVariableW —— kernel32 基础设施。
// 在真实 Windows 上所有路径均为惰性 no-op，行为不变。
class WineCompat
{
public:
    // 探测当前进程是否运行于 Wine/Proton 下。
    // 命中时返回版本串（如 "9.0"），未命中返回 std::nullopt。
    static std::optional<std::string> DetectWine();

    // 为当前进程设置 WINEDLLOVERRIDES（子进程继承）。
    // 已有值时以 ';' 追加而非覆盖，避免破坏用户自身的 override 配置。
    // spec 形如 "d3d11.dll=n,b"。
    // 返回实际生效的完整值（用于日志/事件）。
    static std::wstring ApplyDllOverride(const std::wstring &spec);
};
