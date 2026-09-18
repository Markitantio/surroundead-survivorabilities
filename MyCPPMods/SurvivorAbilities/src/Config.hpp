// Config.hpp
#pragma once

#include <string>

namespace SurvivorAbilities
{
    // Minimal INI reader based on Windows GetPrivateProfileStringW.
    // No external dependencies, no exceptions, works on Windows only
    // (which is fine, the game is Windows-only).
    class Config
    {
    public:
        static Config& Get();

        void SetPath(const std::wstring& path);

        // Returns value as wide string (native for WinAPI).
        std::wstring GetWString(const std::wstring& section,
                                const std::wstring& key,
                                const std::wstring& defaultValue = L"") const;

        // Convenience wrappers.
        std::string  GetString(const std::wstring& section,
                               const std::wstring& key,
                               const std::string&  defaultValue = "") const;

        double       GetDouble(const std::wstring& section,
                               const std::wstring& key,
                               double              defaultValue = 0.0) const;

        int          GetInt   (const std::wstring& section,
                               const std::wstring& key,
                               int                 defaultValue = 0) const;

        bool         GetBool  (const std::wstring& section,
                               const std::wstring& key,
                               bool                defaultValue = false) const;

    private:
        std::wstring m_path;
    };
}