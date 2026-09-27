#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// d3dx.ini [Loader] 兼容层：仅当显式传入 --ini 时使用。
// 字段语义与原 3Dmigoto loader / SSMT Run.exe 保持一致；
// 与原实现的差异：
//   * 不再读取 require_admin（原实现解析后从未使用）；
//   * 不再自动附加 Run.exe 同目录的 SSMT-Player-Tweaks.dll。
struct IniCompatValues
{
    std::wstring target;
    std::wstring module;
    std::wstring launch;
    std::wstring launch_args;
    std::wstring delay = L"5";
    std::vector<std::wstring> inject_dlls;
};

class IniCompatError : public std::runtime_error
{
public:
    IniCompatError(const std::string &message, std::wstring detail)
        : std::runtime_error(message)
        , detail_(std::move(detail))
    {
    }

    const std::wstring &detail() const
    {
        return detail_;
    }

private:
    std::wstring detail_;
};

// 解析失败抛 IniCompatError（detail 为面向用户的长说明）。
IniCompatValues LoadIniCompat(const std::filesystem::path &iniPath);
