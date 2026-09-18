// Config.cpp
#include "Config.hpp"

#include <Windows.h>
#include <cstdlib>
#include <cwchar>

namespace SurvivorAbilities
{
    Config& Config::Get()
    {
        static Config instance;
        return instance;
    }

    void Config::SetPath(const std::wstring& path)
    {
        m_path = path;
    }

    std::wstring Config::GetWString(const std::wstring& section,
                                    const std::wstring& key,
                                    const std::wstring& defaultValue) const
    {
        if (m_path.empty())
        {
            return defaultValue;
        }

        wchar_t buffer[2048]{};
        GetPrivateProfileStringW(
            section.c_str(),
            key.c_str(),
            defaultValue.c_str(),
            buffer,
            static_cast<DWORD>(sizeof(buffer) / sizeof(wchar_t)),
            m_path.c_str());

        return buffer;
    }

    std::string Config::GetString(const std::wstring& section,
                                  const std::wstring& key,
                                  const std::string& defaultValue) const
    {
        // Convert default from utf-8-ish narrow to wide (best effort).
        std::wstring wdefault(defaultValue.begin(), defaultValue.end());
        std::wstring w = GetWString(section, key, wdefault);

        // Convert wide to narrow (best effort, no code page dance).
        return std::string(w.begin(), w.end());
    }

    double Config::GetDouble(const std::wstring& section,
                             const std::wstring& key,
                             double defaultValue) const
    {
        std::wstring w = GetWString(section, key, L"");
        if (w.empty())
        {
            return defaultValue;
        }
        wchar_t* end = nullptr;
        double value = std::wcstod(w.c_str(), &end);
        if (end == w.c_str())
        {
            return defaultValue;
        }
        return value;
    }

    int Config::GetInt(const std::wstring& section,
                       const std::wstring& key,
                       int defaultValue) const
    {
        std::wstring w = GetWString(section, key, L"");
        if (w.empty())
        {
            return defaultValue;
        }
        wchar_t* end = nullptr;
        long value = std::wcstol(w.c_str(), &end, 10);
        if (end == w.c_str())
        {
            return defaultValue;
        }
        return static_cast<int>(value);
    }

    bool Config::GetBool(const std::wstring& section,
                         const std::wstring& key,
                         bool defaultValue) const
    {
        std::wstring w = GetWString(section, key, L"");
        if (w.empty())
        {
            return defaultValue;
        }
        // Accept: 1/0, true/false, yes/no, on/off (case-insensitive)
        if (w == L"1" || w == L"true" || w == L"TRUE" || w == L"True" ||
            w == L"yes"  || w == L"YES"  || w == L"Yes"  ||
            w == L"on"   || w == L"ON"   || w == L"On")
        {
            return true;
        }
        if (w == L"0" || w == L"false" || w == L"FALSE" || w == L"False" ||
            w == L"no"  || w == L"NO"    || w == L"No"    ||
            w == L"off" || w == L"OFF"   || w == L"Off")
        {
            return false;
        }
        return defaultValue;
    }
}