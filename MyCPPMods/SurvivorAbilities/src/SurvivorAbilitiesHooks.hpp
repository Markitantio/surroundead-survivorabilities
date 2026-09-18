// SurvivorAbilitiesHooks.hpp
#pragma once

#include <string>

namespace SurvivorAbilities
{
    // =================================================================
    // Single source of truth for the mod version.
    // Used by SurvivorAbilitiesHooks.cpp and SurvivorAbilitiesMod.cpp.
    // =================================================================
    inline constexpr const wchar_t* SA_VERSION = L"1.0.0";

    // =================================================================
    // Hotkey identifiers (no OpenSpellbook here -- it is handled
    // separately in SurvivorAbilitiesMod.cpp because the callback
    // goes straight to TriggerSpellbook()).
    // =================================================================
    enum class Hotkey
    {
        SprintBurst,
        PowerStrike,
        IronSkin,
        FieldMedic,
        QuickHands,
        FishWhisperer,
        LootSense,
        Ultimate
    };

    // =================================================================
    // Public API
    // =================================================================
    void Install();
    void Shutdown();
    void Update();
    void OnHotkeyPressed(Hotkey key);
    void TriggerSpellbook();
    void SetConfigPath(const std::wstring& path);
}