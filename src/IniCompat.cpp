#include "IniCompat.hpp"

#include <windows.h>

#include <fstream>

namespace
{

// 以下 lite 解析器逐行移植自 3Dmigoto 的 d3dx.ini 解析
// （ini_parser_lite.cpp / D3dxIniUtils.hpp），保持相同的匹配规则：
// 键名与段名大小写不敏感、值前后空白被剔除、解析不依赖锁定即可在
// 任意线程使用。仅做最小必要改动（见 IniCompat.hpp 头注释）。

const char *skip_space(const char *buf)
{
    for (; *buf == ' ' || *buf == '\t'; buf++)
    {
    }
    return buf;
}

const char *next_line(const char *buf)
{
    for (; *buf != '\0' && *buf != '\n' && *buf != '\r'; buf++)
    {
    }
    for (; *buf == '\n' || *buf == '\r' || *buf == ' ' || *buf == '\t'; buf++)
    {
    }
    return buf;
}

const char *find_ini_section_lite(const char *buf, const char *section_name)
{
    const char *p;

    for (buf = skip_space(buf); *buf; buf = next_line(buf))
    {
        if (*buf == '[')
        {
            for (buf++, p = section_name; *p && (tolower(static_cast<unsigned char>(*buf)) == *p); buf++, p++)
            {
            }
            if (*buf == ']' && *p == '\0')
                return next_line(buf);
        }
    }

    return nullptr;
}

bool find_ini_setting_lite(const char *buf, const char *setting, char *ret, size_t n)
{
    const char *p;
    char *r;
    size_t i;

    for (buf = skip_space(buf); *buf; buf = next_line(buf))
    {
        if (*buf == '[')
            return false;

        for (p = setting; *p && tolower(static_cast<unsigned char>(*buf)) == *p; buf++, p++)
        {
        }
        buf = skip_space(buf);
        if (*buf != '=' || *p != '\0')
            continue;

        buf = skip_space(buf + 1);
        for (i = 0, r = ret; i < n; i++, buf++, r++)
        {
            *r = *buf;
            if (*buf == '\n' || *buf == '\r' || *buf == '\0')
            {
                for (; r >= ret && (*r == '\0' || *r == '\n' || *r == '\r' || *r == ' ' || *r == '\t'); r--)
                    *r = '\0';
                return true;
            }
        }
        return false;
    }
    return false;
}

std::wstring ToWideString(const std::string &input)
{
    if (input.empty())
        return L"";

    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, nullptr, 0);
    if (sizeNeeded == 0)
    {
        throw std::runtime_error("MultiByteToWideChar failed");
    }

    std::wstring wide(static_cast<size_t>(sizeNeeded), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, &wide[0], sizeNeeded) == 0)
    {
        throw std::runtime_error("MultiByteToWideChar failed");
    }

    wide.pop_back();
    return wide;
}

std::wstring Trim(std::wstring value)
{
    const wchar_t *whitespace = L" \t\r\n";
    const size_t first = value.find_first_not_of(whitespace);
    if (first == std::wstring::npos)
    {
        return L"";
    }
    const size_t last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

std::vector<std::wstring> SplitDllList(const std::wstring &value)
{
    std::vector<std::wstring> result;
    size_t start = 0;
    while (start <= value.size())
    {
        const size_t sep = value.find(L'|', start);
        const size_t end = sep == std::wstring::npos ? value.size() : sep;
        std::wstring item = Trim(value.substr(start, end - start));
        if (!item.empty())
        {
            result.push_back(std::move(item));
        }
        if (sep == std::wstring::npos)
        {
            break;
        }
        start = sep + 1;
    }
    return result;
}

std::vector<char> ReadWholeFile(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw IniCompatError("cannot open d3dx.ini", L"Can't open d3dx.ini file: " + path.wstring());
    }

    std::vector<char> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    buffer.push_back('\0');
    return buffer;
}

} // namespace

IniCompatValues LoadIniCompat(const std::filesystem::path &iniPath)
{
    IniCompatValues values;

    std::vector<char> buffer = ReadWholeFile(iniPath);
    const char *buf = buffer.data();

    const char *iniSection = find_ini_section_lite(buf, "loader");
    if (!iniSection)
    {
        throw IniCompatError("can't find [Loader] section in d3dx.ini",
                             L"can't find [Loader] section in d3dx.ini");
    }

    char value[MAX_PATH];
    char dllList[8192];

    if (!find_ini_setting_lite(iniSection, "target", value, MAX_PATH))
    {
        throw IniCompatError("can't find target= in your d3dx.ini [Loader] section",
                             L"can't find target = in your d3dx.ini [Loader] section");
    }
    values.target = ToWideString(value);

    if (!find_ini_setting_lite(iniSection, "module", value, MAX_PATH))
    {
        throw IniCompatError("can't find module= in your d3dx.ini [Loader] section",
                             L"can't find module = in your d3dx.ini [Loader] section");
    }
    values.module = ToWideString(value);

    if (find_ini_setting_lite(iniSection, "launch", value, MAX_PATH))
    {
        values.launch = ToWideString(value);
    }

    if (find_ini_setting_lite(iniSection, "launch_args", value, MAX_PATH))
    {
        values.launch_args = ToWideString(value);
    }

    if (find_ini_setting_lite(iniSection, "delay", value, MAX_PATH))
    {
        values.delay = ToWideString(value);
    }

    if (find_ini_setting_lite(iniSection, "inject_dlls", dllList, sizeof(dllList)))
    {
        values.inject_dlls = SplitDllList(ToWideString(dllList));
    }

    // 旧版单值键 inject_dll：仅在新键缺失时生效（与原实现一致）。
    if (values.inject_dlls.empty() && find_ini_setting_lite(iniSection, "inject_dll", dllList, sizeof(dllList)))
    {
        values.inject_dlls = SplitDllList(ToWideString(dllList));
    }

    return values;
}
