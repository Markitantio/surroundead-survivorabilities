#include "SmartMedicHooks.hpp"
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/FFrame.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <fstream>

using namespace RC;
using namespace RC::Unreal;

namespace SmartMedic
{
    // ============================================================
    //  Version marker
    // ============================================================

    static constexpr const wchar_t* SM_VERSION = L"v7.2";

    // ============================================================
    //  Settings
    // ============================================================

    struct MedSettings {
        int    hotkey = VK_F10;
        bool   verboseLogs = false;

        // [Auto]
        bool   autoEnabled = true;
        double autoIntervalSec = 3.0;
        bool   autoOnlyOutOfCombat = true;
        bool   autoOnlyNotInVehicle = true;
        int    maxConsecutiveUsesSameItem = 3;

        // [Auto.Triggers]
        bool   useHeavyBleed = true;
        bool   useBleed = true;
        bool   useBrokenBone = true;
        bool   useInfection = true;

        bool   useRadiation = true;
        bool   useRadiationThreshold = false;
        double radiationThreshold = 0.5;
        bool   useCriticalRadiation = false;
        double criticalRadiationThreshold = 0.15;

        bool   useLowHealth = true;
        double healthThreshold = 0.5;
        bool   useCriticalHealth = true;
        double criticalHealthThreshold = 0.25;

        bool   useHunger = false;
        double hungerThreshold = 0.3;
        bool   useCriticalHunger = false;
        double criticalHungerThreshold = 0.1;

        bool   useThirst = false;
        double thirstThreshold = 0.3;
        bool   useCriticalThirst = false;
        double criticalThirstThreshold = 0.1;

        // [Priorities]
        std::vector<StringType> prioHeavyBleed;
        std::vector<StringType> prioBleed;
        std::vector<StringType> prioBrokenBone;
        std::vector<StringType> prioInfection;
        std::vector<StringType> prioRadiation;
        std::vector<StringType> prioRadiationWarning;
        std::vector<StringType> prioCriticalRadiation;
        std::vector<StringType> prioLowHealth;
        std::vector<StringType> prioCriticalHealth;
        std::vector<StringType> prioLowHunger;
        std::vector<StringType> prioCriticalHunger;
        std::vector<StringType> prioLowThirst;
        std::vector<StringType> prioCriticalThirst;
    };

    static MedSettings G_Settings;
    static StringType  G_ConfigPath;

    // Consecutive-uses трекинг
    static StringType G_LastUsedItemId;
    static int32_t    G_LastUsedItemCount = 0;

    // ============================================================
    //  CONFIG
    // ============================================================

    static std::string WToU8(const std::wstring& w)
    {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                    nullptr, 0, nullptr, nullptr);
        std::string out(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                            out.data(), n, nullptr, nullptr);
        return out;
    }
    static std::wstring U8ToW(const std::string& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring out(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
        return out;
    }

    static void StripBomAndTrim(StringType& s)
    {
        while (!s.empty() && (s.front() == 0xFEFF || s.front() == 0x200B
                              || s.front() == 0x00A0))
            s.erase(s.begin());
        auto isSpace = [](wchar_t c) {
            return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
        };
        while (!s.empty() && isSpace(s.front())) s.erase(s.begin());
        while (!s.empty() && isSpace(s.back()))  s.pop_back();
    }

    static bool ParseBool(const StringType& v)
    {
        if (v.empty()) return false;
        StringType s = v;
        for (auto& c : s) c = std::towlower(c);
        StripBomAndTrim(s);
        return s == L"true" || s == L"1" || s == L"yes" || s == L"on";
    }

    static bool ParseDouble(const StringType& v, double& out)
    {
        try {
            StringType s = v;
            StripBomAndTrim(s);
            if (s.empty()) return false;
            out = std::stod(s);
            return true;
        } catch (...) {
            return false;
        }
    }

    static bool ParseInt(const StringType& v, int& out)
    {
        try {
            StringType s = v;
            StripBomAndTrim(s);
            if (s.empty()) return false;
            out = std::stoi(s);
            return true;
        } catch (...) {
            return false;
        }
    }

    static int ParseHotkey(const StringType& raw)
    {
        StringType s = raw;
        StripBomAndTrim(s);
        for (auto& c : s) c = std::towupper(c);
        if (s.empty()) return 0;
        if (s.size() >= 2 && s[0] == L'F') {
            int n = _wtoi(s.c_str() + 1);
            if (n >= 1 && n <= 24) return VK_F1 + (n - 1);
        }
        if (s.size() == 1) {
            wchar_t c = s[0];
            if (c >= L'0' && c <= L'9') return (int)c;
            if (c >= L'A' && c <= L'Z') return (int)c;
        }
        if (s == L"INSERT")                     return VK_INSERT;
        if (s == L"HOME")                       return VK_HOME;
        if (s == L"END")                        return VK_END;
        if (s == L"DELETE" || s == L"DEL")      return VK_DELETE;
        if (s == L"PAGEUP" || s == L"PGUP")     return VK_PRIOR;
        if (s == L"PAGEDOWN" || s == L"PGDN")   return VK_NEXT;
        if (s == L"SPACE")                      return VK_SPACE;
        if (s == L"TAB")                        return VK_TAB;
        return 0;
    }

    static std::vector<StringType> ParseCommaList(const StringType& raw)
    {
        std::vector<StringType> out;
        StringType cur;
        for (wchar_t c : raw) {
            if (c == L',') {
                StripBomAndTrim(cur);
                if (!cur.empty()) out.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        StripBomAndTrim(cur);
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    static bool FileExists(const StringType& path)
    {
        DWORD attr = GetFileAttributesW(path.c_str());
        return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
    }

    static StringType GetConfigPath()
    {
        HMODULE hMod = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetConfigPath),
            &hMod);
        if (!hMod) return StringType(L"SmartMedic.ini");

        wchar_t path[MAX_PATH] = {};
        DWORD n = GetModuleFileNameW(hMod, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return StringType(L"SmartMedic.ini");

        StringType p(path);
        size_t p1 = p.find_last_of(L"\\/");
        if (p1 == StringType::npos) return StringType(L"SmartMedic.ini");
        p = p.substr(0, p1);
        size_t p2 = p.find_last_of(L"\\/");
        if (p2 == StringType::npos) return StringType(L"SmartMedic.ini");
        p = p.substr(0, p2);
        p += L"\\SmartMedic.ini";
        return p;
    }

    static void SaveDefaultConfig(const StringType& path)
    {
        std::wstring c;
        c += L"; ============================================================\r\n";
        c += L";  SmartMedic v7.2 - config\r\n";
        c += L"; ============================================================\r\n";
        c += L"; После изменения файла перезапустите игру.\r\n";
        c += L";\r\n";
        c += L"; ВАЖНО про радиацию: CurrentRadiation — это \"защита от радиации\".\r\n";
        c += L"; 100% = норма, 0% = смертельное облучение.\r\n";
        c += L"; Мод срабатывает, когда защита УПАЛА НИЖЕ порога.\r\n";
        c += L";\r\n\r\n";

        c += L"[Settings]\r\n";
        c += L"Hotkey=F10\r\n";
        c += L"VerboseLogs=false\r\n\r\n";

        c += L"[Auto]\r\n";
        c += L"Enabled=true\r\n";
        c += L"IntervalSec=3.0\r\n";
        c += L"OnlyWhenOutOfCombat=true\r\n";
        c += L"OnlyWhenNotInVehicle=true\r\n";
        c += L"MaxConsecutiveUsesSameItem=3\r\n\r\n";

        c += L"[Auto.Triggers]\r\n";
        c += L"UseHeavyBleed=true\r\n";
        c += L"UseBleed=true\r\n";
        c += L"UseBrokenBone=true\r\n";
        c += L"UseInfection=true\r\n";
        c += L"UseRadiation=true\r\n";
        c += L"UseRadiationThreshold=false\r\n";
        c += L"RadiationThreshold=0.5\r\n";
        c += L"UseCriticalRadiation=false\r\n";
        c += L"CriticalRadiationThreshold=0.15\r\n";
        c += L"UseLowHealth=true\r\n";
        c += L"HealthThreshold=0.5\r\n";
        c += L"UseCriticalHealth=true\r\n";
        c += L"CriticalHealthThreshold=0.25\r\n";
        c += L"UseHunger=false\r\n";
        c += L"HungerThreshold=0.3\r\n";
        c += L"UseCriticalHunger=false\r\n";
        c += L"CriticalHungerThreshold=0.1\r\n";
        c += L"UseThirst=false\r\n";
        c += L"ThirstThreshold=0.3\r\n";
        c += L"UseCriticalThirst=false\r\n";
        c += L"CriticalThirstThreshold=0.1\r\n\r\n";

        c += L"[Priorities]\r\n";
        c += L"HeavyBleed=Bandages,SmallMedkit,LargeMedkit\r\n";
        c += L"Bleed=Rags,Bandages,SmallMedkit,LargeMedkit\r\n";
        c += L"BrokenBone=Splint,MakeshiftSplint,LargeMedkit\r\n";
        c += L"Infection=InfectionCure,SmallMedkit,LargeMedkit\r\n";
        c += L"Radiation=RadiationPills,LargeMedkit\r\n";
        c += L"RadiationWarning=RadiationPills,CharcoalTablets\r\n";
        c += L"CriticalRadiation=RadiationPills,CharcoalTablets,LargeMedkit\r\n";
        c += L"LowHealth=Bandages,SmallMedkit,LargeMedkit\r\n";
        c += L"CriticalHealth=LargeMedkit,SmallMedkit\r\n";
        c += L"LowHunger=MRE,CookedMeat,CookedSalmon,CookedCatfish,CookedKingMackerel,CookedRedSnapper,CookedLobster,CookedPike,CookedMuskellunge,CookedLargemouthBass,CookedYellowPerch,CookedWhiteCrappie,CookedBlueGill,CookedCrab,CookedCrawfish,CookedDace,Rice,CannedBakedBeans,CannedSpaghetti,CannedTuna,CannedSardines,CannedSoup,EnergyBar\r\n";
        c += L"CriticalHunger=MRE,CookedMeat\r\n";
        c += L"LowThirst=WaterJug,WaterCanteen,WaterBottle,EnergyBottle,DirtyWaterJug,DirtyWaterCanteen,DirtyWaterBottle\r\n";
        c += L"CriticalThirst=DirtyWaterJug,DirtyWaterCanteen,DirtyWaterBottle\r\n";

        std::string utf8 = WToU8(c);
        std::ofstream f(WToU8(path).c_str(), std::ios::binary);
        if (!f.is_open()) return;
        f.write(utf8.data(), (std::streamsize)utf8.size());
    }

    static bool LoadConfigFromFile(const StringType& path)
    {
        std::ifstream f(WToU8(path).c_str(), std::ios::binary);
        if (!f.is_open()) return false;

        std::string raw;
        StringType curSection;

        while (std::getline(f, raw)) {
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();

            if (raw.size() >= 3
                && (uint8_t)raw[0] == 0xEF
                && (uint8_t)raw[1] == 0xBB
                && (uint8_t)raw[2] == 0xBF)
                raw.erase(0, 3);
            if (raw.size() >= 2
                && (uint8_t)raw[0] == 0xFF
                && (uint8_t)raw[1] == 0xFE)
                raw.erase(0, 2);

            StringType line = U8ToW(raw);
            StripBomAndTrim(line);
            if (line.empty()) continue;
            if (line[0] == L';' || line[0] == L'#') continue;

            if (line.front() == L'[' && line.back() == L']') {
                curSection = line.substr(1, line.size() - 2);
                StripBomAndTrim(curSection);
                continue;
            }

            size_t eq = line.find(L'=');
            if (eq == StringType::npos) continue;

            StringType key   = line.substr(0, eq);
            StringType value = line.substr(eq + 1);
            StripBomAndTrim(key);
            StripBomAndTrim(value);

            if (curSection == L"Settings") {
                if (key == L"Hotkey") {
                    int vk = ParseHotkey(value);
                    if (vk) G_Settings.hotkey = vk;
                } else if (key == L"VerboseLogs") {
                    G_Settings.verboseLogs = ParseBool(value);
                }
            } else if (curSection == L"Auto") {
                if (key == L"Enabled") {
                    G_Settings.autoEnabled = ParseBool(value);
                } else if (key == L"IntervalSec") {
                    double v;
                    if (ParseDouble(value, v) && v >= 0.5 && v <= 60.0)
                        G_Settings.autoIntervalSec = v;
                } else if (key == L"OnlyWhenOutOfCombat") {
                    G_Settings.autoOnlyOutOfCombat = ParseBool(value);
                } else if (key == L"OnlyWhenNotInVehicle") {
                    G_Settings.autoOnlyNotInVehicle = ParseBool(value);
                } else if (key == L"MaxConsecutiveUsesSameItem") {
                    int v;
                    if (ParseInt(value, v) && v >= 0 && v <= 100)
                        G_Settings.maxConsecutiveUsesSameItem = v;
                }
            } else if (curSection == L"Auto.Triggers") {
                if      (key == L"UseHeavyBleed")  G_Settings.useHeavyBleed  = ParseBool(value);
                else if (key == L"UseBleed")       G_Settings.useBleed       = ParseBool(value);
                else if (key == L"UseBrokenBone")  G_Settings.useBrokenBone  = ParseBool(value);
                else if (key == L"UseInfection")   G_Settings.useInfection   = ParseBool(value);
                else if (key == L"UseRadiation")   G_Settings.useRadiation   = ParseBool(value);
                else if (key == L"UseRadiationThreshold") G_Settings.useRadiationThreshold = ParseBool(value);
                else if (key == L"RadiationThreshold") {
                    double v; if (ParseDouble(value, v) && v > 0.0 && v <= 1.0)
                        G_Settings.radiationThreshold = v;
                }
                else if (key == L"UseCriticalRadiation") G_Settings.useCriticalRadiation = ParseBool(value);
                else if (key == L"CriticalRadiationThreshold") {
                    double v; if (ParseDouble(value, v) && v > 0.0 && v <= 1.0)
                        G_Settings.criticalRadiationThreshold = v;
                }
                else if (key == L"UseLowHealth")   G_Settings.useLowHealth   = ParseBool(value);
                else if (key == L"HealthThreshold") {
                    double v; if (ParseDouble(value, v) && v > 0.0 && v <= 1.0)
                        G_Settings.healthThreshold = v;
                }
                else if (key == L"UseCriticalHealth") G_Settings.useCriticalHealth = ParseBool(value);
                else if (key == L"CriticalHealthThreshold") {
                    double v; if (ParseDouble(value, v) && v > 0.0 && v <= 1.0)
                        G_Settings.criticalHealthThreshold = v;
                }
                else if (key == L"UseHunger")      G_Settings.useHunger      = ParseBool(value);
                else if (key == L"HungerThreshold") {
                    double v; if (ParseDouble(value, v) && v > 0.0 && v <= 1.0)
                        G_Settings.hungerThreshold = v;
                }
                else if (key == L"UseCriticalHunger") G_Settings.useCriticalHunger = ParseBool(value);
                else if (key == L"CriticalHungerThreshold") {
                    double v; if (ParseDouble(value, v) && v > 0.0 && v <= 1.0)
                        G_Settings.criticalHungerThreshold = v;
                }
                else if (key == L"UseThirst")      G_Settings.useThirst      = ParseBool(value);
                else if (key == L"ThirstThreshold") {
                    double v; if (ParseDouble(value, v) && v > 0.0 && v <= 1.0)
                        G_Settings.thirstThreshold = v;
                }
                else if (key == L"UseCriticalThirst") G_Settings.useCriticalThirst = ParseBool(value);
                else if (key == L"CriticalThirstThreshold") {
                    double v; if (ParseDouble(value, v) && v > 0.0 && v <= 1.0)
                        G_Settings.criticalThirstThreshold = v;
                }
            } else if (curSection == L"Priorities") {
                auto list = ParseCommaList(value);
                if (list.empty()) continue;
                if      (key == L"HeavyBleed")        G_Settings.prioHeavyBleed        = list;
                else if (key == L"Bleed")             G_Settings.prioBleed             = list;
                else if (key == L"BrokenBone")        G_Settings.prioBrokenBone        = list;
                else if (key == L"Infection")         G_Settings.prioInfection         = list;
                else if (key == L"Radiation")         G_Settings.prioRadiation         = list;
                else if (key == L"RadiationWarning")  G_Settings.prioRadiationWarning  = list;
                else if (key == L"CriticalRadiation") G_Settings.prioCriticalRadiation = list;
                else if (key == L"LowHealth")         G_Settings.prioLowHealth         = list;
                else if (key == L"CriticalHealth")    G_Settings.prioCriticalHealth    = list;
                else if (key == L"LowHunger")         G_Settings.prioLowHunger         = list;
                else if (key == L"CriticalHunger")    G_Settings.prioCriticalHunger    = list;
                else if (key == L"LowThirst")         G_Settings.prioLowThirst         = list;
                else if (key == L"CriticalThirst")    G_Settings.prioCriticalThirst    = list;
            }
        }
        return true;
    }

    static void InstallConfig()
    {
        G_ConfigPath = GetConfigPath();
        Output::send<LogLevel::Verbose>(STR("[SmartMedic] {} config path: {}\n"),
                                        StringType(SM_VERSION), G_ConfigPath);

        if (!FileExists(G_ConfigPath)) {
            Output::send<LogLevel::Verbose>(STR("[SmartMedic] creating default config\n"));
            SaveDefaultConfig(G_ConfigPath);
        }
        LoadConfigFromFile(G_ConfigPath);

        Output::send<LogLevel::Verbose>(
            STR("[SmartMedic] config: hotkey=0x{:X} verbose={} "
                "auto={} interval={:.1f}s combat={} vehicle={} maxSame={}\n"),
            G_Settings.hotkey,
            G_Settings.verboseLogs ? 1 : 0,
            G_Settings.autoEnabled ? 1 : 0,
            G_Settings.autoIntervalSec,
            G_Settings.autoOnlyOutOfCombat ? 1 : 0,
            G_Settings.autoOnlyNotInVehicle ? 1 : 0,
            G_Settings.maxConsecutiveUsesSameItem);

        Output::send<LogLevel::Verbose>(
            STR("[SmartMedic] triggers(auto): hBleed={} bleed={} bone={} inf={}\n"),
            G_Settings.useHeavyBleed ? 1 : 0,
            G_Settings.useBleed ? 1 : 0,
            G_Settings.useBrokenBone ? 1 : 0,
            G_Settings.useInfection ? 1 : 0);

        Output::send<LogLevel::Verbose>(
            STR("[SmartMedic]   rad: flag={} thr={}/{:.2f} crit={}/{:.2f}\n"),
            G_Settings.useRadiation ? 1 : 0,
            G_Settings.useRadiationThreshold ? 1 : 0, G_Settings.radiationThreshold,
            G_Settings.useCriticalRadiation ? 1 : 0, G_Settings.criticalRadiationThreshold);

        Output::send<LogLevel::Verbose>(
            STR("[SmartMedic]   hp: low={}/{:.2f} crit={}/{:.2f}\n"),
            G_Settings.useLowHealth ? 1 : 0, G_Settings.healthThreshold,
            G_Settings.useCriticalHealth ? 1 : 0, G_Settings.criticalHealthThreshold);

        Output::send<LogLevel::Verbose>(
            STR("[SmartMedic]   hunger: low={}/{:.2f} crit={}/{:.2f}\n"),
            G_Settings.useHunger ? 1 : 0, G_Settings.hungerThreshold,
            G_Settings.useCriticalHunger ? 1 : 0, G_Settings.criticalHungerThreshold);

        Output::send<LogLevel::Verbose>(
            STR("[SmartMedic]   thirst: low={}/{:.2f} crit={}/{:.2f}\n"),
            G_Settings.useThirst ? 1 : 0, G_Settings.thirstThreshold,
            G_Settings.useCriticalThirst ? 1 : 0, G_Settings.criticalThirstThreshold);

        auto dump = [](const wchar_t* name, const std::vector<StringType>& v) {
            Output::send<LogLevel::Verbose>(STR("[SmartMedic]   {} ({}): "),
                                            StringType(name), (int)v.size());
            for (auto& s : v) Output::send<LogLevel::Verbose>(STR("'{}' "), s);
            Output::send<LogLevel::Verbose>(STR("\n"));
        };
        dump(L"HeavyBleed",        G_Settings.prioHeavyBleed);
        dump(L"Bleed",             G_Settings.prioBleed);
        dump(L"BrokenBone",        G_Settings.prioBrokenBone);
        dump(L"Infection",         G_Settings.prioInfection);
        dump(L"Radiation",         G_Settings.prioRadiation);
        dump(L"RadiationWarning",  G_Settings.prioRadiationWarning);
        dump(L"CriticalRadiation", G_Settings.prioCriticalRadiation);
        dump(L"LowHealth",         G_Settings.prioLowHealth);
        dump(L"CriticalHealth",    G_Settings.prioCriticalHealth);
        dump(L"LowHunger",         G_Settings.prioLowHunger);
        dump(L"CriticalHunger",    G_Settings.prioCriticalHunger);
        dump(L"LowThirst",         G_Settings.prioLowThirst);
        dump(L"CriticalThirst",    G_Settings.prioCriticalThirst);
    }

    // ============================================================
    //  HELPERS
    // ============================================================

    #define SM_V(...) do { if (G_Settings.verboseLogs) \
        Output::send<LogLevel::Verbose>(__VA_ARGS__); } while(0)

    #define SM_LOG(...) \
        Output::send<LogLevel::Verbose>(__VA_ARGS__)

    static StringType SafeName(UObject* o)
    { return o ? o->GetName() : StringType(STR("<null>")); }

    static uint32_t ReadU32(void* b, int32_t o)
    { return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(b) + o); }
    static bool ReadBool(void* b, int32_t o)
    { return *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(b) + o); }
    static double ReadDouble(void* b, int32_t o)
    { return *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(b) + o); }
    static void* ReadPtr(void* b, int32_t o)
    { return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(b) + o); }
    static StringType ReadFNameAt(void* b, int32_t o)
    {
        FName* f = reinterpret_cast<FName*>(reinterpret_cast<uint8_t*>(b) + o);
        return f->ToString();
    }

    static bool IsPendingKill(UObject* o)
    {
        if (!o) return true;
        uint32_t flags = ReadU32(o, 0x08);
        return (flags & 0x42000000u) != 0;
    }

    // ============================================================
    //  OFFSETS
    // ============================================================

    namespace PlayerOffset {
        constexpr int32_t MedicalComponent       = 0x07A8;
        constexpr int32_t RadiationComponent     = 0x07C8;
        constexpr int32_t HungerThirstComponent  = 0x07D0;
        constexpr int32_t StaminaComponent       = 0x07D8;
        constexpr int32_t PlayerController       = 0x13C8;

        constexpr int32_t IsPlayerRolling        = 0x139B;
        constexpr int32_t Combat                 = 0x14C4;
        constexpr int32_t JournalOpen            = 0x14E0;
        constexpr int32_t RadialMenuOpen         = 0x14E1;
        constexpr int32_t InVehicle              = 0x1508;
        constexpr int32_t Reloading              = 0x1530;
        constexpr int32_t Attacking              = 0x1539;
        constexpr int32_t CurrentlyChangingWeapon= 0x1578;
    }

    namespace MedOffset {
        constexpr int32_t Bleed             = 0x00D8;
        constexpr int32_t HeavyBleed        = 0x00D9;
        constexpr int32_t BrokenBone        = 0x00DA;
        constexpr int32_t Health            = 0x00E0;
        constexpr int32_t MaxHealth         = 0x00E8;
        constexpr int32_t RadiationSickness = 0x0118;
        constexpr int32_t Infection         = 0x0119;
    }

    namespace VitalsOffset {
        constexpr int32_t Current = 0x00D8;
        constexpr int32_t Max     = 0x00E0;
    }

    namespace JsiOffset {
        constexpr int32_t ArrayOfItems = 0x0490;
    }

    namespace SlotOffset {
        constexpr int32_t JigDataAsset = 0x04F0;
    }

    namespace DaOffset {
        constexpr int32_t ItemId   = 0x0030;
        constexpr int32_t ItemType = 0x0068;
    }

    // ============================================================
    //  FIND LOCAL PLAYER
    // ============================================================

    static UObject* FindLocalPlayer()
    {
        std::vector<UObject*> all;
        UObjectGlobals::FindAllOf(STR("BP_PlayerCharacter_C"), all);
        for (UObject* p : all) {
            if (!p) continue;
            if (IsPendingKill(p)) continue;
            UObject* pc = reinterpret_cast<UObject*>(ReadPtr(p, PlayerOffset::PlayerController));
            if (pc) return p;
        }
        for (UObject* p : all) {
            if (p && !IsPendingKill(p)) return p;
        }
        return nullptr;
    }

    // ============================================================
    //  READ VITALS
    // ============================================================

    struct VitalsState {
        bool   bleed = false;
        bool   heavyBleed = false;
        bool   brokenBone = false;
        bool   infection = false;
        bool   radiationSickness = false;
        double health = 0.0;
        double maxHealth = 100.0;
        double healthPct = 1.0;

        bool   hasHungerThirst = false;
        double hunger = 0.0;
        double maxHunger = 100.0;
        double hungerPct = 1.0;
        double thirst = 0.0;
        double maxThirst = 100.0;
        double thirstPct = 1.0;

        bool   hasRadiation = false;
        double radiation = 0.0;
        double maxRadiation = 100.0;
        double radiationPct = 1.0;
    };

    static bool ReadVitalsState(UObject* player, VitalsState& out)
    {
        if (!player) return false;

        UObject* med = reinterpret_cast<UObject*>(ReadPtr(player, PlayerOffset::MedicalComponent));
        if (!med) return false;

        out.bleed             = ReadBool(med, MedOffset::Bleed);
        out.heavyBleed        = ReadBool(med, MedOffset::HeavyBleed);
        out.brokenBone        = ReadBool(med, MedOffset::BrokenBone);
        out.radiationSickness = ReadBool(med, MedOffset::RadiationSickness);
        out.infection         = ReadBool(med, MedOffset::Infection);
        out.health            = ReadDouble(med, MedOffset::Health);
        out.maxHealth         = ReadDouble(med, MedOffset::MaxHealth);
        out.healthPct         = (out.maxHealth > 0.0)
                                ? (out.health / out.maxHealth)
                                : 1.0;

        UObject* ht = reinterpret_cast<UObject*>(
            ReadPtr(player, PlayerOffset::HungerThirstComponent));
        if (ht) {
            out.hasHungerThirst = true;
            out.hunger    = ReadDouble(ht, VitalsOffset::Current);
            out.maxHunger = ReadDouble(ht, VitalsOffset::Max);
            out.hungerPct = (out.maxHunger > 0.0)
                            ? (out.hunger / out.maxHunger) : 1.0;

            out.thirst    = ReadDouble(ht, 0x00E8);
            out.maxThirst = ReadDouble(ht, 0x00F0);
            out.thirstPct = (out.maxThirst > 0.0)
                            ? (out.thirst / out.maxThirst) : 1.0;
        }

        UObject* rad = reinterpret_cast<UObject*>(
            ReadPtr(player, PlayerOffset::RadiationComponent));
        if (rad) {
            out.hasRadiation = true;
            out.radiation    = ReadDouble(rad, VitalsOffset::Current);
            out.maxRadiation = ReadDouble(rad, VitalsOffset::Max);
            out.radiationPct = (out.maxRadiation > 0.0)
                               ? (out.radiation / out.maxRadiation) : 1.0;
        }

        return true;
    }

    // ============================================================
    //  STATE CHECKS
    // ============================================================

    static bool AutoStateRequiresAction(const VitalsState& st)
    {
        if (G_Settings.useHeavyBleed && st.heavyBleed) return true;
        if (G_Settings.useBleed      && st.bleed)      return true;
        if (G_Settings.useBrokenBone && st.brokenBone) return true;
        if (G_Settings.useInfection  && st.infection)  return true;
        if (G_Settings.useRadiation  && st.radiationSickness) return true;

        if (st.hasRadiation) {
            if (G_Settings.useCriticalRadiation
                && st.radiationPct <= G_Settings.criticalRadiationThreshold) return true;
            if (G_Settings.useRadiationThreshold
                && st.radiationPct <= G_Settings.radiationThreshold) return true;
        }

        if (G_Settings.useCriticalHealth
            && st.healthPct < G_Settings.criticalHealthThreshold) return true;
        if (G_Settings.useLowHealth
            && st.healthPct < G_Settings.healthThreshold) return true;

        if (st.hasHungerThirst) {
            if (G_Settings.useCriticalHunger
                && st.hungerPct < G_Settings.criticalHungerThreshold) return true;
            if (G_Settings.useHunger
                && st.hungerPct < G_Settings.hungerThreshold) return true;

            if (G_Settings.useCriticalThirst
                && st.thirstPct < G_Settings.criticalThirstThreshold) return true;
            if (G_Settings.useThirst
                && st.thirstPct < G_Settings.thirstThreshold) return true;
        }

        return false;
    }

    // ============================================================
    //  FIND ITEM IN INVENTORY
    // ============================================================

    struct SimpleTArray { void* Data; int32_t Num; int32_t Max; };

    static void CollectItemsById(const StringType& wantId,
                                 std::vector<UObject*>& out,
                                 int32_t maxItems)
    {
        if ((int32_t)out.size() >= maxItems) return;

        std::vector<UObject*> containers;
        UObjectGlobals::FindAllOf(STR("JSIContainer_C"), containers);

        for (UObject* c : containers) {
            if (!c || IsPendingKill(c)) continue;

            auto* arr = reinterpret_cast<SimpleTArray*>(
                reinterpret_cast<uint8_t*>(c) + JsiOffset::ArrayOfItems);
            if (!arr || arr->Num <= 0 || !arr->Data) continue;

            UObject** items = reinterpret_cast<UObject**>(arr->Data);
            for (int32_t i = 0; i < arr->Num; ++i) {
                UObject* slot = items[i];
                if (!slot || IsPendingKill(slot)) continue;

                UObject* da = reinterpret_cast<UObject*>(ReadPtr(slot, SlotOffset::JigDataAsset));
                if (!da) continue;

                StringType id = ReadFNameAt(da, DaOffset::ItemId);
                if (id == wantId) {
                    out.push_back(slot);
                    if ((int32_t)out.size() >= maxItems) return;
                }
            }
        }
    }

    // Пробуждаем контейнеры, у которых UI ещё не построен.
    // Аналог логики из AutoSortLoot v12.
    static void WakeAllJSIContainers()
    {
        std::vector<UObject*> containers;
        UObjectGlobals::FindAllOf(STR("JSIContainer_C"), containers);

        int woken = 0;
        for (UObject* c : containers) {
            if (!c || IsPendingKill(c)) continue;
            UClass* cls = c->GetClassPrivate();
            if (!cls) continue;

            UFunction* fn = cls->GetFunctionByName(STR("ForceInitSpecialcontainer"));
            if (!fn) continue;

            uint8_t buf[0x40] = {};
            c->ProcessEvent(fn, buf);
            ++woken;
        }
        SM_V(STR("[SmartMedic] woke {} JSIContainers\n"), woken);
    }

    static void CollectItemsByIdWithWake(const StringType& wantId,
                                         std::vector<UObject*>& out,
                                         int32_t maxItems,
                                         bool& didWake)
    {
        CollectItemsById(wantId, out, maxItems);
        if (!out.empty()) return;

        if (didWake) return;
        didWake = true;

        WakeAllJSIContainers();
        CollectItemsById(wantId, out, maxItems);
    }

    // ============================================================
    //  USE ITEM
    // ============================================================

    static bool CallHotBarConsume(UObject* player, UObject* itemRef)
    {
        if (!player || !itemRef) return false;

        UClass* cls = player->GetClassPrivate();
        if (!cls) return false;

        UFunction* fn = cls->GetFunctionByName(STR("HotBarConsume"));
        if (!fn) {
            SM_LOG(STR("[SmartMedic] HotBarConsume NOT FOUND\n"));
            return false;
        }

        uint8_t buf[0x40] = {};
        *reinterpret_cast<UObject**>(buf + 0x00) = itemRef;

        player->ProcessEvent(fn, buf);
        return true;
    }

    // ============================================================
    //  PRIORITY LIST
    // ============================================================

    static void AppendDedup(std::vector<StringType>& dst, const std::vector<StringType>& src)
    {
        for (const auto& s : src) {
            bool exists = false;
            for (const auto& d : dst) if (d == s) { exists = true; break; }
            if (!exists) dst.push_back(s);
        }
    }

    static std::vector<StringType> BuildPriorityList(const VitalsState& st, bool manualMode)
    {
        std::vector<StringType> list;
        auto shouldUse = [manualMode](bool flag) { return manualMode ? true : flag; };

        // ---- Bleed / HeavyBleed ----
        bool heavyHandled = false;
        if (shouldUse(G_Settings.useHeavyBleed) && st.heavyBleed) {
            AppendDedup(list, G_Settings.prioHeavyBleed);
            heavyHandled = true;
        }
        if (!heavyHandled && shouldUse(G_Settings.useBleed) && st.bleed) {
            AppendDedup(list, G_Settings.prioBleed);
        }

        if (shouldUse(G_Settings.useBrokenBone) && st.brokenBone)
            AppendDedup(list, G_Settings.prioBrokenBone);
        if (shouldUse(G_Settings.useInfection) && st.infection)
            AppendDedup(list, G_Settings.prioInfection);

        // ---- Radiation ----
        if (shouldUse(G_Settings.useRadiation) && st.radiationSickness)
            AppendDedup(list, G_Settings.prioRadiation);

        if (st.hasRadiation) {
            bool critRad = shouldUse(G_Settings.useCriticalRadiation)
                && st.radiationPct <= G_Settings.criticalRadiationThreshold;
            bool warnRad = shouldUse(G_Settings.useRadiationThreshold)
                && st.radiationPct <= G_Settings.radiationThreshold;

            if (critRad) {
                AppendDedup(list, G_Settings.prioCriticalRadiation);
                AppendDedup(list, G_Settings.prioRadiationWarning);  // fallback
            } else if (warnRad) {
                AppendDedup(list, G_Settings.prioRadiationWarning);
            }
        }

        // ---- Health (tiered + fallback) ----
        {
            bool critHp = shouldUse(G_Settings.useCriticalHealth)
                && st.healthPct < G_Settings.criticalHealthThreshold;
            bool lowHp = shouldUse(G_Settings.useLowHealth)
                && st.healthPct < G_Settings.healthThreshold;

            if (critHp) {
                AppendDedup(list, G_Settings.prioCriticalHealth);
                AppendDedup(list, G_Settings.prioLowHealth);         // fallback
            } else if (lowHp) {
                AppendDedup(list, G_Settings.prioLowHealth);
            }
        }

        // ---- Hunger / Thirst (tiered + fallback) ----
        if (st.hasHungerThirst) {
            bool critH = shouldUse(G_Settings.useCriticalHunger)
                && st.hungerPct < G_Settings.criticalHungerThreshold;
            bool lowH = shouldUse(G_Settings.useHunger)
                && st.hungerPct < G_Settings.hungerThreshold;

            if (critH) {
                AppendDedup(list, G_Settings.prioCriticalHunger);
                AppendDedup(list, G_Settings.prioLowHunger);         // fallback
            } else if (lowH) {
                AppendDedup(list, G_Settings.prioLowHunger);
            }

            bool critT = shouldUse(G_Settings.useCriticalThirst)
                && st.thirstPct < G_Settings.criticalThirstThreshold;
            bool lowT = shouldUse(G_Settings.useThirst)
                && st.thirstPct < G_Settings.thirstThreshold;

            if (critT) {
                AppendDedup(list, G_Settings.prioCriticalThirst);
                AppendDedup(list, G_Settings.prioLowThirst);         // fallback
            } else if (lowT) {
                AppendDedup(list, G_Settings.prioLowThirst);
            }
        }

        return list;
    }

    // ============================================================
    //  BUSY CHECK
    // ============================================================

    static bool IsPlayerBusyForAutoConsume(UObject* player,
                                           bool& outCombatBlocked,
                                           bool& outVehicleBlocked)
    {
        outCombatBlocked  = false;
        outVehicleBlocked = false;

        if (!player) return true;

        if (G_Settings.autoOnlyOutOfCombat) {
            if (ReadBool(player, PlayerOffset::Combat)) {
                outCombatBlocked = true;
                return true;
            }
        }

        if (G_Settings.autoOnlyNotInVehicle) {
            if (ReadBool(player, PlayerOffset::InVehicle)) {
                outVehicleBlocked = true;
                return true;
            }
        }

        if (ReadBool(player, PlayerOffset::Reloading))               return true;
        if (ReadBool(player, PlayerOffset::Attacking))               return true;
        if (ReadBool(player, PlayerOffset::CurrentlyChangingWeapon)) return true;
        if (ReadBool(player, PlayerOffset::IsPlayerRolling))         return true;

        return false;
    }

    // ============================================================
    //  MAIN
    // ============================================================

    static void TrySmartConsume(bool autoTriggered)
    {
        const wchar_t* mode = autoTriggered ? L"AUTO" : L"MANUAL";
        SM_V(STR("[SmartMedic] === TRY ({}) ===\n"), StringType(mode));

        UObject* player = FindLocalPlayer();
        if (!player) { SM_LOG(STR("[SmartMedic] no player\n")); return; }

        VitalsState st{};
        if (!ReadVitalsState(player, st)) {
            SM_LOG(STR("[SmartMedic] no MedicalComponent\n"));
            return;
        }

        SM_V(STR("[SmartMedic] HP={:.1f}/{:.1f} ({:.0f}%) "),
             st.health, st.maxHealth, st.healthPct * 100.0);

        if (st.hasHungerThirst) {
            SM_V(STR("H={:.1f}/{:.1f} ({:.0f}%) T={:.1f}/{:.1f} ({:.0f}%) "),
                 st.hunger, st.maxHunger, st.hungerPct * 100.0,
                 st.thirst, st.maxThirst, st.thirstPct * 100.0);
        } else {
            SM_V(STR("H=--- T=--- "));
        }

        if (st.hasRadiation) {
            SM_V(STR("Rad={:.1f}/{:.1f} ({:.0f}%) "),
                 st.radiation, st.maxRadiation, st.radiationPct * 100.0);
        } else {
            SM_V(STR("Rad=--- "));
        }

        SM_V(STR("Bleed={} HB={} Bone={} Inf={} RadSick={}\n"),
             st.bleed ? 1 : 0, st.heavyBleed ? 1 : 0,
             st.brokenBone ? 1 : 0, st.infection ? 1 : 0,
             st.radiationSickness ? 1 : 0);

        std::vector<StringType> priority = BuildPriorityList(st, !autoTriggered);

        if (priority.empty()) {
            // Всё в норме — сбрасываем счётчик подряд-использований
            G_LastUsedItemId.clear();
            G_LastUsedItemCount = 0;
            SM_V(STR("[SmartMedic] nothing to treat\n"));
            return;
        }

        SM_V(STR("[SmartMedic] priority ({}): "), (int)priority.size());
        if (G_Settings.verboseLogs) {
            for (auto& s : priority) SM_LOG(STR("'{}' "), s);
            SM_LOG(STR("\n"));
        }

        bool didWake = false;

        for (const auto& itemId : priority) {
            // Проверка: не использовали ли мы этот ItemId подряд N раз
            if (G_Settings.maxConsecutiveUsesSameItem > 0
                && itemId == G_LastUsedItemId
                && G_LastUsedItemCount >= G_Settings.maxConsecutiveUsesSameItem)
            {
                SM_V(STR("[SmartMedic]   '{}' — skipped (used {} consecutive, limit={})\n"),
                     itemId, G_LastUsedItemCount,
                     G_Settings.maxConsecutiveUsesSameItem);
                continue;
            }

            std::vector<UObject*> found;
            CollectItemsByIdWithWake(itemId, found, 4, didWake);

            if (found.empty()) {
                SM_V(STR("[SmartMedic]   '{}' — not found\n"), itemId);
                continue;
            }

            UObject* slot = found[0];
            SM_LOG(STR("[SmartMedic] {} USING '{}' slot={}\n"),
                   StringType(mode), itemId, SafeName(slot));

            if (CallHotBarConsume(player, slot)) {
                SM_LOG(STR("[SmartMedic] {} consume dispatched\n"), StringType(mode));

                if (itemId == G_LastUsedItemId) {
                    G_LastUsedItemCount++;
                } else {
                    G_LastUsedItemId = itemId;
                    G_LastUsedItemCount = 1;
                }
                return;
            } else {
                SM_LOG(STR("[SmartMedic] {} consume FAILED\n"), StringType(mode));
                return;
            }
        }

        SM_LOG(STR("[SmartMedic] {} no matching item in inventory\n"),
               StringType(mode));
    }

    // ============================================================
    //  HOTKEY + AUTO LOOP
    // ============================================================

    static bool   G_HookRegistered = false;
    static bool   G_KeyWasDown = false;
    static bool   G_Busy = false;
    static double G_LastAutoTick = 0.0;

    static double NowSeconds()
    {
        return (double)GetTickCount64() / 1000.0;
    }

    void TryInstall()
    {
        if (!G_HookRegistered) {
            Hook::RegisterProcessLocalScriptFunctionPreCallback(
                [](UObject*, FFrame&, void*) {});
            G_HookRegistered = true;
            SM_LOG(STR("[SmartMedic] {} hook installed\n"), StringType(SM_VERSION));
        }

        if (G_Busy) return;
        G_Busy = true;

        // ---- AUTO ----
        if (G_Settings.autoEnabled) {
            double now = NowSeconds();
            if (now - G_LastAutoTick >= G_Settings.autoIntervalSec) {
                G_LastAutoTick = now;

                UObject* player = FindLocalPlayer();
                bool combatBlocked = false, vehicleBlocked = false;

                if (!IsPlayerBusyForAutoConsume(player, combatBlocked, vehicleBlocked)) {
                    VitalsState st{};
                    if (player && ReadVitalsState(player, st)
                        && AutoStateRequiresAction(st)) {
                        TrySmartConsume(true);
                    }
                } else {
                    if (combatBlocked)  SM_V(STR("[SmartMedic] auto skipped: combat\n"));
                    if (vehicleBlocked) SM_V(STR("[SmartMedic] auto skipped: vehicle\n"));
                }
            }
        }

        // ---- MANUAL ----
        if (G_Settings.hotkey != 0) {
            bool down = (GetAsyncKeyState(G_Settings.hotkey) & 0x8000) != 0;
            if (down && !G_KeyWasDown) {
                TrySmartConsume(false);
            }
            G_KeyWasDown = down;
        } else {
            G_KeyWasDown = false;
        }

        G_Busy = false;
    }

    void Install()
    {
        Output::send<LogLevel::Verbose>(STR("[SmartMedic] {} Install()\n"),
                                        StringType(SM_VERSION));
        InstallConfig();
    }

    void Uninstall()
    {
        Output::send<LogLevel::Verbose>(STR("[SmartMedic] {} Uninstall()\n"),
                                        StringType(SM_VERSION));
    }
}