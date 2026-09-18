// SurvivorAbilitiesMod.cpp
#include <UE4SSProgram.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Input/Handler.hpp>

#include <filesystem>
#include <string>

#include "SurvivorAbilitiesHooks.hpp"
#include "Config.hpp"
#include "KeyMapper.hpp"

using SurvivorAbilities::Config;
using SurvivorAbilities::KeyFromString;

namespace
{
    RC::Input::Key ReadHotkey(Config& cfg,
                              const wchar_t* name,
                              const wchar_t* defaultValue,
                              RC::Input::Key fallbackEnum)
    {
        std::wstring w = cfg.GetWString(L"Hotkeys", name, defaultValue);
        std::string narrow(w.begin(), w.end());
        RC::Input::Key k = KeyFromString(narrow, fallbackEnum);
        RC::Output::send<RC::LogLevel::Verbose>(
            STR("[SurvivorAbilities] hotkey {}='{}'\n"), name, defaultValue);
        return k;
    }
}

class SurvivorAbilitiesMod : public RC::CppUserModBase
{
public:
    SurvivorAbilitiesMod()
    {
        ModName        = STR("SurvivorAbilities");
        ModVersion     = SurvivorAbilities::SA_VERSION;
        ModDescription = STR("Active abilities unlocked by passive skills");
        ModAuthors     = STR("madiko12");

        RC::Output::send<RC::LogLevel::Verbose>(
            STR("[SurvivorAbilities] {} constructed\n"), ModVersion);
    }

    ~SurvivorAbilitiesMod() override
    {
        SurvivorAbilities::Shutdown();
        RC::Output::send<RC::LogLevel::Verbose>(
            STR("[SurvivorAbilities] destroyed\n"));
    }

    auto on_unreal_init() -> void override
    {
        RC::Output::send<RC::LogLevel::Verbose>(
            STR("[SurvivorAbilities] {} Install()\n"), ModVersion);

        std::wstring iniPath = L"SurvivorAbilities.ini";
        try
        {
            auto modsDir = UE4SSProgram::get_program().get_mods_directory();
            std::filesystem::path p(modsDir);
            p /= L"SurvivorAbilities";
            p /= L"SurvivorAbilities.ini";
            iniPath = p.wstring();
        }
        catch (...) {}

        RC::Output::send<RC::LogLevel::Verbose>(
            STR("[SurvivorAbilities] config path: {}\n"), iniPath.c_str());

        SurvivorAbilities::SetConfigPath(iniPath);
        SurvivorAbilities::Install();

        Config& cfg = Config::Get();

        const RC::Input::Key kSprint = ReadHotkey(cfg, L"SprintBurst",   L"X", RC::Input::Key::X);
        const RC::Input::Key kPower  = ReadHotkey(cfg, L"PowerStrike",   L"C", RC::Input::Key::C);
        const RC::Input::Key kIron   = ReadHotkey(cfg, L"IronSkin",      L"J", RC::Input::Key::J);
        const RC::Input::Key kMedic  = ReadHotkey(cfg, L"FieldMedic",    L"N", RC::Input::Key::N);
        const RC::Input::Key kQuick  = ReadHotkey(cfg, L"QuickHands",    L"G", RC::Input::Key::G);
        const RC::Input::Key kFish   = ReadHotkey(cfg, L"FishWhisperer", L"K", RC::Input::Key::K);
        const RC::Input::Key kLoot   = ReadHotkey(cfg, L"LootSense",     L"H", RC::Input::Key::H);
        const RC::Input::Key kUltim  = ReadHotkey(cfg, L"Ultimate",      L"U", RC::Input::Key::U);
        const RC::Input::Key kBook   = ReadHotkey(cfg, L"OpenSpellbook", L"F1", RC::Input::Key::F1);

        register_keydown_event(kSprint, []() {
            SurvivorAbilities::OnHotkeyPressed(SurvivorAbilities::Hotkey::SprintBurst);
        });
        register_keydown_event(kPower, []() {
            SurvivorAbilities::OnHotkeyPressed(SurvivorAbilities::Hotkey::PowerStrike);
        });
        register_keydown_event(kIron, []() {
            SurvivorAbilities::OnHotkeyPressed(SurvivorAbilities::Hotkey::IronSkin);
        });
        register_keydown_event(kMedic, []() {
            SurvivorAbilities::OnHotkeyPressed(SurvivorAbilities::Hotkey::FieldMedic);
        });
        register_keydown_event(kQuick, []() {
            SurvivorAbilities::OnHotkeyPressed(SurvivorAbilities::Hotkey::QuickHands);
        });
        register_keydown_event(kFish, []() {
            SurvivorAbilities::OnHotkeyPressed(SurvivorAbilities::Hotkey::FishWhisperer);
        });
        register_keydown_event(kLoot, []() {
            SurvivorAbilities::OnHotkeyPressed(SurvivorAbilities::Hotkey::LootSense);
        });
        register_keydown_event(kUltim, []() {
            SurvivorAbilities::OnHotkeyPressed(SurvivorAbilities::Hotkey::Ultimate);
        });
        register_keydown_event(kBook, []() {
            SurvivorAbilities::TriggerSpellbook();
        });

        RC::Output::send<RC::LogLevel::Verbose>(
            STR("[SurvivorAbilities] 8 abilities + F1 spellbook registered\n"));
    }

    auto on_ui_init() -> void override {}

    auto on_update() -> void override
    {
        SurvivorAbilities::Update();
    }
};

#define MOD_API __declspec(dllexport)

extern "C"
{
    MOD_API RC::CppUserModBase* start_mod()
    {
        return new SurvivorAbilitiesMod();
    }

    MOD_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}