#include "WineCompat.hpp"

#include <windows.h>

namespace
{

const char *ResolveWineVersion()
{
    // wine_get_version 是 Wine 的 ntdll 长期导出的探测点；
    // 真实 Windows 的 ntdll 没有该导出，GetProcAddress 返回 nullptr。
    using WineGetVersionFn = const char *(CDECL *)();

    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
    {
        return nullptr;
    }

    const auto wineGetVersion =
        reinterpret_cast<WineGetVersionFn>(
            GetProcAddress(ntdll, "wine_get_version"));

    return wineGetVersion ? wineGetVersion() : nullptr;
}

} // namespace

std::optional<std::string> WineCompat::DetectWine()
{
    const char *version = ResolveWineVersion();
    if (!version)
    {
        return std::nullopt;
    }
    return std::string(version);
}

std::wstring WineCompat::ApplyDllOverride(const std::wstring &spec)
{
    if (spec.empty())
    {
        return {};
    }

    const std::wstring key = L"WINEDLLOVERRIDES";

    wchar_t existing[2048]{};
    const DWORD existingLength =
        GetEnvironmentVariableW(key.c_str(), existing, ARRAYSIZE(existing));

    std::wstring merged;
    if (existingLength > 0 && existingLength < ARRAYSIZE(existing))
    {
        merged = existing;
        if (!merged.empty() && merged.back() != L';')
        {
            merged += L';';
        }
        merged += spec;
    }
    else
    {
        merged = spec;
    }

    SetEnvironmentVariableW(key.c_str(), merged.c_str());
    return merged;
}
