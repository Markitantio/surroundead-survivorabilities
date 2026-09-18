// SurvivorAbilitiesHooks.cpp
#include "SurvivorAbilitiesHooks.hpp"
#include "Config.hpp"
#include "Notification.hpp"

#include <atomic>

// v1.0.2: Windows.h is needed for SEH (__try/__except/EXCEPTION_EXECUTE_HANDLER).
// NOMINMAX is CRITICAL: without it Windows.h defines min/max macros that break
// std::numeric_limits<...>::max() everywhere in UE4SS headers.
// WIN32_LEAN_AND_MEAN trims the Windows API surface we do not need.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <string>
#include <vector>

#include <UE4SSProgram.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace SurvivorAbilities
{
    // SA_VERSION comes from SurvivorAbilitiesHooks.hpp

    // =================================================================
    // Runtime toggles (populated in Install() from the INI)
    // =================================================================
    static bool g_bEnabled = true;
    static bool g_Verbose = false;

    // =================================================================
    // Cached pointers
    // =================================================================
    static UObject*   g_Player               = nullptr;
    static UObject*   g_PlayerController     = nullptr;
    static UObject*   g_MedicalComp          = nullptr;
    static UObject*   g_PassiveSkills        = nullptr;
    static UObject*   g_CharacterMovement    = nullptr;
    static UObject*   g_JigHelperComp        = nullptr;
    static UObject*   g_JigMultiComp         = nullptr;
    static UFunction* g_FnClientUpdateHealth = nullptr;

    static std::atomic<bool> g_bShutdown{false};

    // v1.0.2: set the first time ANY UE4SS call raises an SEH exception.
    // Once true, every entry point (Update / OnHotkeyPressed / finders)
    // returns immediately and we never touch UE4SS again. This prevents a
    // cascade of AVs during world teardown -- the crash occurs once, gets
    // caught, and the mod effectively unplugs itself for the rest of the
    // process lifetime.
    static std::atomic<bool> g_bTeardownDetected{false};

    // =================================================================
    // Active effect flags -- declared early because FindPlayer() needs to
    // reset them when the pawn changes (death / respawn / save reload).
    // =================================================================
    static bool g_SprintActive   = false;
    static bool g_UltimateActive = false;

    // =================================================================
    // Offsets
    // =================================================================
    namespace Off
    {
        constexpr size_t Player_MedicalComponent    = 0x07A8;
        constexpr size_t Player_FishingComponent    = 0x0678;
        constexpr size_t Player_BP_JigHelperComp    = 0x06D8;
        constexpr size_t Player_CurrentFiringWeapon = 0x08D8;
        constexpr size_t Player_BP_JigMultiComponent= 0x07F0;
        constexpr size_t Player_PlayerController    = 0x13C8;

        constexpr size_t Character_CharacterMovement = 0x0330;

        constexpr size_t CMC_MaxWalkSpeed          = 0x0278;
        constexpr size_t CMC_MaxWalkSpeedCrouched  = 0x027C;

        constexpr size_t Medical_Bleed             = 0x00D8;
        constexpr size_t Medical_HeavyBleed        = 0x00D9;
        constexpr size_t Medical_Health            = 0x00E0;
        constexpr size_t Medical_MaxHealth         = 0x00E8;

        constexpr size_t PassiveSkills_FitnessLvl  = 0x00E8;
        constexpr size_t PassiveSkills_StrengthLvl = 0x0108;
        constexpr size_t PassiveSkills_ToughnessLvl= 0x0128;
        constexpr size_t PassiveSkills_SneakingLvl = 0x0148;
        constexpr size_t PassiveSkills_FirstAidLvl = 0x0168;
        constexpr size_t PassiveSkills_MarksmLvl   = 0x0188;
        constexpr size_t PassiveSkills_ReloadLvl   = 0x01A8;
        constexpr size_t PassiveSkills_ThiefLvl    = 0x0248;
        constexpr size_t PassiveSkills_FishingLvl  = 0x0278;
        constexpr size_t PassiveSkills_ScavengLvl  = 0x02C8;

        constexpr size_t FishingComp_ChanceToCatch = 0x00D8;

        constexpr size_t LootContainer_StaticMesh  = 0x02C8;

        constexpr size_t Firearm_PlayerActiveWeapon = 0x03F8;

        // UObjectBase::EObjectFlags lives at offset 0x08 on UE5.6.
        constexpr size_t Object_Flags               = 0x08;
    }

    // =================================================================
    // Helpers
    // =================================================================
    static double NowSeconds()
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    static double g_LastHeartbeatTime = 0.0;
    static int    g_Heartbeats        = 0;

    struct Cooldown { double lastUsed = 0.0; bool valid = false; };
    static Cooldown g_CD_FieldMedic;
    static Cooldown g_CD_SprintBurst;
    static Cooldown g_CD_QuickHands;
    static Cooldown g_CD_IronSkin;
    static Cooldown g_CD_FishWhisperer;
    static Cooldown g_CD_LootSense;
    static Cooldown g_CD_PowerStrike;
    static Cooldown g_CD_Ultimate;

    static std::wstring Format(const wchar_t* fmt, ...)
    {
        wchar_t buf[512];
        va_list args;
        va_start(args, fmt);
        _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
        va_end(args);
        return buf;
    }

    // Read the current hotkey from INI, trim whitespace, uppercase for
    // display, and return it as a wide string.
    //
    // IMPORTANT: called fresh every time a notification or the spellbook
    // is built. Nothing is cached across frames, so editing SurvivorAbilities.ini
    // is reflected in all in-game notifications on the next interaction.
    // (Key registration still happens once at game start, so the physically
    // working key only updates after a game restart -- but the DISPLAYED key
    // in tooltips updates live.)
    static std::wstring GetHotkeyDisplay(const wchar_t* iniKey, const wchar_t* fallback)
    {
        Config& cfg = Config::Get();
        std::wstring raw = cfg.GetWString(L"Hotkeys", iniKey, fallback);

        // Trim leading/trailing whitespace
        const wchar_t* ws = L" \t\r\n";
        size_t first = raw.find_first_not_of(ws);
        size_t last  = raw.find_last_not_of(ws);
        if (first == std::wstring::npos) return fallback;
        raw = raw.substr(first, last - first + 1);

        // Uppercase for display
        for (auto& c : raw)
            c = static_cast<wchar_t>(towupper(c));

        return raw;
    }

    // ---------- Verbose logging helper ----------
    // STR() is a single-arg macro, so it must only ever see the format string.
    // Variadic args are forwarded straight to Output::send.
    // __VA_OPT__ is the conformant replacement for the old MSVC ##__VA_ARGS__
    // (this file compiles with /Zc:preprocessor).
    //
    // IMPORTANT: the format string and any string arguments MUST be wide
    // (L"..."), because Output::send uses fmt's wformat_context here.
    // Mixing const char* into a wide fmt call triggers
    // "static assertion failed: 'formatting of non-void pointers is disallowed'".
    #define SA_LOG(fmt, ...) \
        Output::send<LogLevel::Verbose>(STR(fmt) __VA_OPT__(,) __VA_ARGS__)

    // =================================================================
    // Read / Write
    // =================================================================
    static bool ReadDouble(UObject* obj, size_t offset, double& out)
    {
        if (!obj) return false;
        out = *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(obj) + offset);
        return true;
    }
    static void WriteDouble(UObject* obj, size_t offset, double v)
    {
        if (!obj) return;
        *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(obj) + offset) = v;
    }
    static bool ReadFloat(UObject* obj, size_t offset, float& out)
    {
        if (!obj) return false;
        out = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + offset);
        return true;
    }
    static void WriteFloat(UObject* obj, size_t offset, float v)
    {
        if (!obj) return;
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + offset) = v;
    }
    static void WriteBool(UObject* obj, size_t offset, bool v)
    {
        if (!obj) return;
        *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(obj) + offset) = v;
    }
    static UObject* ReadObject(UObject* obj, size_t offset)
    {
        if (!obj) return nullptr;
        return *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(obj) + offset);
    }

    // =================================================================
    // v1.0.2: Teardown guards
    // =================================================================
    // Background:
    //   UE4SS keeps firing on_update for several frames after UWorld starts
    //   tearing down. unreal_is_shutting_down is set LATER. During those
    //   frames, UObjectGlobals::FindFirstOf() itself is unsafe -- it iterates
    //   GUObjectArray, which is already partially dismantled, and one of the
    //   entries becomes a dangling pointer. UE4SS then writes to
    //   (nullptr + 0x24), triggering the AV.
    //
    //   v1.0.1 attempted to pre-check via UWorld::bIsTearingDown, but the
    //   bit at offset 0x18D is a bitfield shared by bBegunPlay / bMatchStarted
    //   / bStartup etc. Reading the whole byte (as bool) yields TRUE during
    //   normal gameplay, which incorrectly flagged the world as "dead" and
    //   disabled the whole mod. Removed in v1.0.2.
    //
    // Strategy (v1.0.2):
    //   1) SEH-wrap every call into UE4SS that could touch the dying world.
    //   2) The first time SEH catches an AV, set g_bTeardownDetected.
    //   3) Every entry point checks that flag at the top and returns early.
    //   4) Result: exactly ONE caught AV during teardown (no .dmp), then the
    //      mod quietly goes silent for the rest of the process lifetime.
    //
    // MSVC C2712 constraint:
    //   __try / __except cannot appear in a function that has any C++ object
    //   with a non-trivial destructor on the stack. This includes TEMPORARIES
    //   created by STR("..."). Even `new StringType(STR("World"))` inside an
    //   if-block counts: MSVC sees the STR() temporary and refuses __try in
    //   the same function.
    //
    //   Therefore: every function that contains __try must have ZERO C++
    //   objects with destructors in its body. All actual work goes into a
    //   sibling *Impl function which has no __try and freely uses STR(...).

    // --- IsPendingKill: reads raw flags at o+0x08. If o is a freed pointer,
    //     even this read can AV. Wrap it.
    //     No local objects with destructors -> C2712-safe.
    static bool IsPendingKill(UObject* o)
    {
        if (!o) return true;
        __try
        {
            uint32_t flags = *reinterpret_cast<uint32_t*>(
                reinterpret_cast<uint8_t*>(o) + Off::Object_Flags);
            return (flags & 0x42000000u) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_bTeardownDetected.store(true, std::memory_order_relaxed);
            return true;
        }
    }

    static bool GetActorLocation(UObject* actor, double& x, double& y, double& z)
    {
        if (!actor) return false;
        UFunction* fn = actor->GetFunctionByNameInChain(STR("K2_GetActorLocation"));
        if (!fn) return false;
        uint8_t buf[0x20] = {};
        actor->ProcessEvent(fn, buf);
        x = *reinterpret_cast<double*>(buf + 0x00);
        y = *reinterpret_cast<double*>(buf + 0x08);
        z = *reinterpret_cast<double*>(buf + 0x10);
        return true;
    }

    static bool GetActorForwardVector(UObject* actor, double& fx, double& fy, double& fz)
    {
        if (!actor) return false;
        UFunction* fn = actor->GetFunctionByNameInChain(STR("GetActorForwardVector"));
        if (!fn) return false;
        uint8_t buf[0x20] = {};
        actor->ProcessEvent(fn, buf);
        fx = *reinterpret_cast<double*>(buf + 0x00);
        fy = *reinterpret_cast<double*>(buf + 0x08);
        fz = *reinterpret_cast<double*>(buf + 0x10);
        return true;
    }

    static bool IsInstanceOfClass(UObject* obj, const wchar_t* targetClassName)
    {
        if (!obj) return false;
        UStruct* cls = obj->GetClassPrivate();
        int guard = 0;
        while (cls && guard < 32)
        {
            StringType name = cls->GetName();
            if (name == targetClassName) return true;
            cls = cls->GetSuperStruct();
            ++guard;
        }
        return false;
    }

    static bool TriggerCustomDepth(UObject* primitiveComp, bool enabled, int stencil)
    {
        if (!primitiveComp) return false;
        bool any = false;
        if (UFunction* fn = primitiveComp->GetFunctionByNameInChain(STR("SetRenderCustomDepth")))
        {
            uint8_t buf[8] = {};
            *reinterpret_cast<bool*>(buf) = enabled;
            primitiveComp->ProcessEvent(fn, buf);
            any = true;
        }
        if (UFunction* fn = primitiveComp->GetFunctionByNameInChain(STR("SetCustomDepthStencilValue")))
        {
            uint8_t buf[8] = {};
            *reinterpret_cast<int32*>(buf) = stencil;
            primitiveComp->ProcessEvent(fn, buf);
            any = true;
        }
        return any;
    }

    // =================================================================
    // Speed boost (shared by Sprint Burst and Ultimate)
    // =================================================================
    struct SpeedBoostState
    {
        float  playerBaseW   = 0.0f;
        float  playerBaseWC  = 0.0f;
        float  lastAppliedW  = 0.0f;
        float  lastAppliedWC = 0.0f;
        double mult          = 1.0;
        double lastProbe     = 0.0;
    };

    static SpeedBoostState g_SprintBoost;
    static SpeedBoostState g_UltBoost;

    static void ResetSpeedBoost(SpeedBoostState& st, double mult)
    {
        st.playerBaseW   = 0.0f;
        st.playerBaseWC  = 0.0f;
        st.lastAppliedW  = 0.0f;
        st.lastAppliedWC = 0.0f;
        st.lastProbe     = NowSeconds();
        st.mult          = mult;
    }

    static void ApplySpeedBoost(SpeedBoostState& st)
    {
        if (!g_CharacterMovement) return;

        float curW = 0.0f, curWC = 0.0f;
        if (!ReadFloat(g_CharacterMovement, Off::CMC_MaxWalkSpeed, curW)) return;
        ReadFloat(g_CharacterMovement, Off::CMC_MaxWalkSpeedCrouched, curWC);

        constexpr float EPS = 0.5f;

        if (st.playerBaseW == 0.0f || std::fabs(curW - st.lastAppliedW) > EPS)
            st.playerBaseW = curW;
        if (st.playerBaseWC == 0.0f || std::fabs(curWC - st.lastAppliedWC) > EPS)
            st.playerBaseWC = curWC;

        const float boostedW  = st.playerBaseW  * static_cast<float>(st.mult);
        const float boostedWC = st.playerBaseWC * static_cast<float>(st.mult);

        WriteFloat(g_CharacterMovement, Off::CMC_MaxWalkSpeed,         boostedW);
        WriteFloat(g_CharacterMovement, Off::CMC_MaxWalkSpeedCrouched, boostedWC);

        st.lastAppliedW  = boostedW;
        st.lastAppliedWC = boostedWC;
    }

    static void RestoreSpeedBoost(SpeedBoostState& st)
    {
        if (!g_CharacterMovement) return;
        if (st.playerBaseW > 0.0f)
            WriteFloat(g_CharacterMovement, Off::CMC_MaxWalkSpeed,         st.playerBaseW);
        if (st.playerBaseWC > 0.0f)
            WriteFloat(g_CharacterMovement, Off::CMC_MaxWalkSpeedCrouched, st.playerBaseWC);
    }

    static void ProbeSpeedBoost(const wchar_t* tag, SpeedBoostState& st)
    {
        if (!g_Verbose) return;
        if (!g_CharacterMovement) return;

        const double now = NowSeconds();
        if (st.lastProbe != 0.0 && now - st.lastProbe < 3.0) return;
        st.lastProbe = now;

        float curW = 0.0f, curWC = 0.0f;
        if (!ReadFloat(g_CharacterMovement, Off::CMC_MaxWalkSpeed, curW)) return;
        ReadFloat(g_CharacterMovement, Off::CMC_MaxWalkSpeedCrouched, curWC);

        const float expectW  = st.playerBaseW  * static_cast<float>(st.mult);
        const float expectWC = st.playerBaseWC * static_cast<float>(st.mult);
        const bool  okW      = std::fabs(curW  - expectW)  < 1.0f;
        const bool  okWC     = std::fabs(curWC - expectWC) < 1.0f;

        SA_LOG("[Ability] {} probe: walk base={:.0f} boosted={:.0f} current={:.0f} {} | crouch base={:.0f} boosted={:.0f} current={:.0f} {}\n",
               tag,
               st.playerBaseW,  expectW,  curW,  okW  ? L"OK" : L"MISMATCH",
               st.playerBaseWC, expectWC, curWC, okWC ? L"OK" : L"MISMATCH");
    }

    // =================================================================
    // Discovery
    // =================================================================
    // The actual body of the resolver lives in ...Impl so that the thin
    // SEH wrapper (which contains __try/__except and must NOT have any
    // StringType temporaries on the stack) can call into it.
    static UObject* ResolveCurrentPawnImpl()
    {
        if (UObject* pc = UObjectGlobals::FindFirstOf(STR("PlayerController")))
        {
            if (!IsPendingKill(pc))
            {
                if (UFunction* fnGetPawn = pc->GetFunctionByNameInChain(STR("K2_GetPawn")))
                {
                    uint8_t buf[0x10] = {};
                    *reinterpret_cast<UObject**>(buf + 0x00) = nullptr;
                    pc->ProcessEvent(fnGetPawn, buf);
                    if (UObject* p = *reinterpret_cast<UObject**>(buf + 0x00))
                    {
                        if (!IsPendingKill(p)) return p;
                    }
                }
                if (UFunction* fnGetPawn2 = pc->GetFunctionByNameInChain(STR("GetPawn")))
                {
                    uint8_t buf[0x10] = {};
                    *reinterpret_cast<UObject**>(buf + 0x00) = nullptr;
                    pc->ProcessEvent(fnGetPawn2, buf);
                    if (UObject* p = *reinterpret_cast<UObject**>(buf + 0x00))
                    {
                        if (!IsPendingKill(p)) return p;
                    }
                }
            }
        }

        UObject* fallback = UObjectGlobals::FindFirstOf(STR("BP_PlayerCharacter_C"));
        if (!fallback || IsPendingKill(fallback)) return nullptr;
        return fallback;
    }

    static UObject* ResolveCurrentPawn()
    {
        if (g_bTeardownDetected.load(std::memory_order_relaxed)) return nullptr;

        UObject* result = nullptr;
        __try
        {
            result = ResolveCurrentPawnImpl();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_bTeardownDetected.store(true, std::memory_order_relaxed);
            result = nullptr;
        }
        return result;
    }

    static void FindPlayer()
    {
        // v1.0.2: only the soft teardown flag gates us now. The old
        // IsWorldAlive() check has been removed because UWorld::bIsTearingDown
        // shares its byte with bBegunPlay / bMatchStarted / bStartup and the
        // whole-byte read was returning true during normal gameplay.
        if (g_bTeardownDetected.load(std::memory_order_relaxed)) return;

        UObject* found = ResolveCurrentPawn();

        if (found == g_Player) return;

        g_Player               = found;
        g_PlayerController     = nullptr;
        g_MedicalComp          = nullptr;
        g_PassiveSkills        = nullptr;
        g_CharacterMovement    = nullptr;
        g_JigHelperComp        = nullptr;
        g_JigMultiComp         = nullptr;
        g_FnClientUpdateHealth = nullptr;

        // Cancel any in-flight speed effects: their state belongs to the
        // old pawn's CharacterMovement, which no longer exists.
        g_SprintActive   = false;
        g_UltimateActive = false;

        SA_LOG("[SurvivorAbilities] player changed -> 0x{:X}, caches cleared\n",
               reinterpret_cast<size_t>(g_Player));
    }

    // split for the same C2712 reason as ResolveCurrentPawn.
    static void FindComponentsImpl()
    {
        if (!g_Player) return;

        if (!g_MedicalComp)
            g_MedicalComp = ReadObject(g_Player, Off::Player_MedicalComponent);
        if (!g_PassiveSkills)
            g_PassiveSkills = UObjectGlobals::FindFirstOf(STR("PassiveSkillsComponent_C"));
        if (!g_CharacterMovement)
            g_CharacterMovement = ReadObject(g_Player, Off::Character_CharacterMovement);
        if (!g_JigHelperComp)
            g_JigHelperComp = ReadObject(g_Player, Off::Player_BP_JigHelperComp);
        if (!g_JigMultiComp)
            g_JigMultiComp = ReadObject(g_Player, Off::Player_BP_JigMultiComponent);
        if (!g_PlayerController)
            g_PlayerController = ReadObject(g_Player, Off::Player_PlayerController);
        if (!g_FnClientUpdateHealth)
            g_FnClientUpdateHealth = g_Player->GetFunctionByNameInChain(STR("Client_UpdateHealthUI"));

        Notify::Initialize();
    }

    static void FindComponents()
    {
        if (g_bTeardownDetected.load(std::memory_order_relaxed)) return;

        __try
        {
            FindComponentsImpl();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_bTeardownDetected.store(true, std::memory_order_relaxed);
        }
    }

    static void UpdateHealthUI(double newHP)
    {
        if (!g_Player || !g_FnClientUpdateHealth) return;
        uint8_t buf[0x40] = {};
        *reinterpret_cast<double*>(buf + 0x00) = newHP;
        g_Player->ProcessEvent(g_FnClientUpdateHealth, buf);
    }

    static UObject* GetEquippedFirearm()
    {
        if (!g_JigHelperComp) return nullptr;
        UFunction* fn = g_JigHelperComp->GetFunctionByNameInChain(STR("GetActiveWeapon"));
        if (!fn) return nullptr;
        uint8_t buf[0x10] = {};
        *reinterpret_cast<UObject**>(buf + 0x00) = nullptr;
        g_JigHelperComp->ProcessEvent(fn, buf);
        return *reinterpret_cast<UObject**>(buf + 0x00);
    }

    static int CountSkillsAtLevel(UObject* passive, double minLevel)
    {
        if (!passive) return 0;
        static const size_t offsets[] = {
            Off::PassiveSkills_FitnessLvl,
            Off::PassiveSkills_StrengthLvl,
            Off::PassiveSkills_ToughnessLvl,
            Off::PassiveSkills_SneakingLvl,
            Off::PassiveSkills_FirstAidLvl,
            Off::PassiveSkills_MarksmLvl,
            Off::PassiveSkills_ReloadLvl,
            Off::PassiveSkills_ThiefLvl,
            Off::PassiveSkills_FishingLvl,
            Off::PassiveSkills_ScavengLvl,
        };
        int count = 0;
        for (size_t off : offsets)
        {
            double lvl = 0.0;
            ReadDouble(passive, off, lvl);
            if (lvl >= minLevel) ++count;
        }
        return count;
    }

    // =================================================================
    // Sprint Burst
    // =================================================================
    static double g_SprintEndTime = 0.0;

    static void TryActivateSprintBurst()
    {
        SA_LOG("[Ability] Sprint Burst pressed\n");

        Config& cfg = Config::Get();
        const double unlockLevel = cfg.GetDouble(L"SprintBurst", L"UnlockLevel", 3.0);
        const double cooldown    = cfg.GetDouble(L"SprintBurst", L"Cooldown", 180.0);
        const double multiplier  = cfg.GetDouble(L"SprintBurst", L"SpeedMultiplier", 1.6);
        const double duration    = cfg.GetDouble(L"SprintBurst", L"Duration", 12.0);
        const bool   showNotify  = cfg.GetBool  (L"SprintBurst", L"ShowNotification", true);
        const double dbgForceFit = cfg.GetDouble(L"Debug", L"ForceFitnessLevel", 0.0);
        const double effUnlock   = (dbgForceFit > 0.0) ? 0.0 : unlockLevel;

        if (g_CD_SprintBurst.valid)
        {
            const double elapsed = NowSeconds() - g_CD_SprintBurst.lastUsed;
            if (elapsed < cooldown)
            {
                int remaining = static_cast<int>(cooldown - elapsed);
                SA_LOG("[Ability] Sprint Burst BLOCKED: cooldown {}s left\n", remaining);
                if (showNotify) Notify::Show(Format(L"Sprint Burst: %d s", remaining), 1.5);
                return;
            }
        }
        if (!g_Player || !g_CharacterMovement || !g_PassiveSkills)
        {
            SA_LOG("[Ability] Sprint Burst FAILED: missing components (player={}, cm={}, skills={})\n",
                   g_Player != nullptr, g_CharacterMovement != nullptr, g_PassiveSkills != nullptr);
            return;
        }

        double fitness = 0.0;
        ReadDouble(g_PassiveSkills, Off::PassiveSkills_FitnessLvl, fitness);
        if (fitness < effUnlock)
        {
            SA_LOG("[Ability] Sprint Burst LOCKED: Fitness {:.2f} < {:.2f}\n",
                   fitness, unlockLevel);
            if (showNotify)
            {
                const std::wstring k = GetHotkeyDisplay(L"SprintBurst", L"X");
                Notify::Show(Format(
                    L"SPRINT BURST [%s] - locked\n"
                    L"Fitness %.1f / %.1f\n"
                    L"Sprint and swim to train", k.c_str(), fitness, unlockLevel), 3.0);
            }
            return;
        }
        if (g_SprintActive)
        {
            SA_LOG("[Ability] Sprint Burst BLOCKED: already active\n");
            return;
        }

        ResetSpeedBoost(g_SprintBoost, multiplier);
        ApplySpeedBoost(g_SprintBoost);

        g_SprintActive  = true;
        g_SprintEndTime = NowSeconds() + duration;
        g_CD_SprintBurst.lastUsed = NowSeconds();
        g_CD_SprintBurst.valid    = true;

        SA_LOG("[Ability] Sprint Burst ON: x{:.2f} speed for {:.1f}s (base {:.0f} -> {:.0f}, cd {:.0f}s)\n",
               multiplier, duration,
               g_SprintBoost.playerBaseW,
               g_SprintBoost.playerBaseW * static_cast<float>(multiplier),
               cooldown);

        if (showNotify)
            Notify::Show(Format(L"Sprint Burst\nx%.1f speed for %.0fs",
                                multiplier, duration), 2.5);
    }

    static void TickSprintBurst()
    {
        if (!g_SprintActive) return;
        if (NowSeconds() < g_SprintEndTime)
        {
            ApplySpeedBoost(g_SprintBoost);
            ProbeSpeedBoost(L"Sprint Burst", g_SprintBoost);
            return;
        }
        RestoreSpeedBoost(g_SprintBoost);
        g_SprintActive = false;
        SA_LOG("[Ability] Sprint Burst OFF (speed restored to base {:.0f})\n",
               g_SprintBoost.playerBaseW);
    }

    // =================================================================
    // Field Medic
    // =================================================================
    static void TryActivateFieldMedic()
    {
        SA_LOG("[Ability] Field Medic pressed\n");

        Config& cfg = Config::Get();
        const double unlockLevel = cfg.GetDouble(L"FieldMedic", L"UnlockLevel", 3.0);
        const double cooldown    = cfg.GetDouble(L"FieldMedic", L"Cooldown",    90.0);
        const double hpThreshold = cfg.GetDouble(L"FieldMedic", L"HPThreshold", 0.90);
        const double healAmount  = cfg.GetDouble(L"FieldMedic", L"HealAmount",  40.0);
        const bool   clearBleed  = cfg.GetBool  (L"FieldMedic", L"ClearBleed",      true);
        const bool   clearHeavy  = cfg.GetBool  (L"FieldMedic", L"ClearHeavyBleed", true);
        const bool   showNotify  = cfg.GetBool  (L"FieldMedic", L"ShowNotification", true);
        const double dbgForceFA  = cfg.GetDouble(L"Debug", L"ForceFirstAidLevel", 0.0);
        const double dbgForceHP  = cfg.GetDouble(L"Debug", L"ForceHPThreshold",   0.0);
        const double effUnlock   = (dbgForceFA > 0.0) ? 0.0 : unlockLevel;
        const double effHPThr    = (dbgForceHP > 0.0) ? dbgForceHP : hpThreshold;

        if (g_CD_FieldMedic.valid)
        {
            const double elapsed = NowSeconds() - g_CD_FieldMedic.lastUsed;
            if (elapsed < cooldown)
            {
                int remaining = static_cast<int>(cooldown - elapsed);
                SA_LOG("[Ability] Field Medic BLOCKED: cooldown {}s left\n", remaining);
                if (showNotify) Notify::Show(Format(L"Field Medic: %d s", remaining), 1.5);
                return;
            }
        }
        if (!g_Player || !g_MedicalComp || !g_PassiveSkills)
        {
            SA_LOG("[Ability] Field Medic FAILED: missing components\n");
            return;
        }

        double faLvl = 0.0;
        ReadDouble(g_PassiveSkills, Off::PassiveSkills_FirstAidLvl, faLvl);
        if (faLvl < effUnlock)
        {
            SA_LOG("[Ability] Field Medic LOCKED: FirstAid {:.2f} < {:.2f}\n",
                   faLvl, unlockLevel);
            if (showNotify)
            {
                const std::wstring k = GetHotkeyDisplay(L"FieldMedic", L"N");
                Notify::Show(Format(
                    L"FIELD MEDIC [%s] - locked\n"
                    L"FirstAid %.1f / %.1f\n"
                    L"Use bandages and medkits to train", k.c_str(), faLvl, unlockLevel), 3.0);
            }
            return;
        }

        double hp = 0.0, hpMax = 0.0;
        ReadDouble(g_MedicalComp, Off::Medical_Health,    hp);
        ReadDouble(g_MedicalComp, Off::Medical_MaxHealth, hpMax);
        if (hpMax <= 0.0)
        {
            SA_LOG("[Ability] Field Medic FAILED: hpMax<=0\n");
            return;
        }

        const double hpPct = hp / hpMax;
        if (hpPct > effHPThr)
        {
            SA_LOG("[Ability] Field Medic BLOCKED: HP {:.1f}% > threshold {:.1f}%\n",
                   hpPct * 100.0, effHPThr * 100.0);
            if (showNotify)
                Notify::Show(Format(L"Field Medic: HP too high (%.0f%%)", hpPct * 100.0), 1.5);
            return;
        }

        const double newHP = (hp + healAmount > hpMax) ? hpMax : (hp + healAmount);
        WriteDouble(g_MedicalComp, Off::Medical_Health, newHP);
        if (clearBleed) WriteBool(g_MedicalComp, Off::Medical_Bleed,      false);
        if (clearHeavy) WriteBool(g_MedicalComp, Off::Medical_HeavyBleed, false);
        UpdateHealthUI(newHP);

        g_CD_FieldMedic.lastUsed = NowSeconds();
        g_CD_FieldMedic.valid    = true;

        SA_LOG("[Ability] Field Medic ON: HP {:.0f} -> {:.0f} (+{:.0f}), bleed_cleared={}\n",
               hp, newHP, newHP - hp, (clearBleed || clearHeavy));

        if (showNotify)
            Notify::Show(Format(L"Field Medic\n+%.0f HP, bleed cleared",
                                newHP - hp), 2.5);
    }

    // =================================================================
    // Iron Skin
    // =================================================================
    static double g_IronSkinEndTime = 0.0;
    static double g_IronSkinOrigMaxHP = 0.0;
    static bool   g_IronSkinActive = false;

    static void TryActivateIronSkin()
    {
        SA_LOG("[Ability] Iron Skin pressed\n");

        Config& cfg = Config::Get();
        const double unlockLevel = cfg.GetDouble(L"IronSkin", L"UnlockLevel", 3.0);
        const double cooldown    = cfg.GetDouble(L"IronSkin", L"Cooldown",   300.0);
        const double duration    = cfg.GetDouble(L"IronSkin", L"Duration",     4.0);
        const double bonusHP     = cfg.GetDouble(L"IronSkin", L"BonusHP",   9999.0);
        const bool   showNotify  = cfg.GetBool  (L"IronSkin", L"ShowNotification", true);
        const double dbgForce    = cfg.GetDouble(L"Debug", L"ForceToughnessLevel", 0.0);
        const double effUnlock   = (dbgForce > 0.0) ? 0.0 : unlockLevel;

        if (g_CD_IronSkin.valid)
        {
            const double elapsed = NowSeconds() - g_CD_IronSkin.lastUsed;
            if (elapsed < cooldown)
            {
                int remaining = static_cast<int>(cooldown - elapsed);
                SA_LOG("[Ability] Iron Skin BLOCKED: cooldown {}s left\n", remaining);
                if (showNotify) Notify::Show(Format(L"Iron Skin: %d s", remaining), 1.5);
                return;
            }
        }
        if (!g_Player || !g_MedicalComp || !g_PassiveSkills)
        {
            SA_LOG("[Ability] Iron Skin FAILED: missing components\n");
            return;
        }

        double tough = 0.0;
        ReadDouble(g_PassiveSkills, Off::PassiveSkills_ToughnessLvl, tough);
        if (tough < effUnlock)
        {
            SA_LOG("[Ability] Iron Skin LOCKED: Toughness {:.2f} < {:.2f}\n",
                   tough, unlockLevel);
            if (showNotify)
            {
                const std::wstring k = GetHotkeyDisplay(L"IronSkin", L"J");
                Notify::Show(Format(
                    L"IRON SKIN [%s] - locked\n"
                    L"Toughness %.1f / %.1f\n"
                    L"Take damage to train", k.c_str(), tough, unlockLevel), 3.0);
            }
            return;
        }
        if (g_IronSkinActive)
        {
            SA_LOG("[Ability] Iron Skin BLOCKED: already active\n");
            return;
        }

        double hp = 0.0, maxHP = 0.0;
        ReadDouble(g_MedicalComp, Off::Medical_Health,    hp);
        ReadDouble(g_MedicalComp, Off::Medical_MaxHealth, maxHP);
        g_IronSkinOrigMaxHP = maxHP;

        WriteDouble(g_MedicalComp, Off::Medical_MaxHealth, maxHP + bonusHP);
        WriteDouble(g_MedicalComp, Off::Medical_Health,    hp   + bonusHP);
        UpdateHealthUI(hp + bonusHP);

        g_IronSkinActive  = true;
        g_IronSkinEndTime = NowSeconds() + duration;
        g_CD_IronSkin.lastUsed = NowSeconds();
        g_CD_IronSkin.valid    = true;

        SA_LOG("[Ability] Iron Skin ON: +{:.0f} HP buffer for {:.1f}s (orig max {:.0f}, cd {:.0f}s)\n",
               bonusHP, duration, maxHP, cooldown);

        if (showNotify)
            Notify::Show(Format(L"Iron Skin active\n%.0fs immunity", duration), 2.5);
    }

    static void TickIronSkin()
    {
        if (!g_IronSkinActive) return;
        if (NowSeconds() < g_IronSkinEndTime) return;

        if (g_MedicalComp)
        {
            double hp = 0.0;
            ReadDouble(g_MedicalComp, Off::Medical_Health, hp);
            if (hp > g_IronSkinOrigMaxHP) hp = g_IronSkinOrigMaxHP;
            WriteDouble(g_MedicalComp, Off::Medical_MaxHealth, g_IronSkinOrigMaxHP);
            WriteDouble(g_MedicalComp, Off::Medical_Health,    hp);
            UpdateHealthUI(hp);
        }
        g_IronSkinActive = false;
        SA_LOG("[Ability] Iron Skin OFF (duration expired)\n");
    }

    // =================================================================
    // Fish Whisperer
    // =================================================================
    static double g_FishWhispererEndTime = 0.0;
    static double g_FishWhispererOrigChance = 0.0;
    static bool   g_FishWhispererActive = false;

    static void TryActivateFishWhisperer()
    {
        SA_LOG("[Ability] Fish Whisperer pressed\n");

        Config& cfg = Config::Get();
        const double unlockLevel = cfg.GetDouble(L"FishWhisperer", L"UnlockLevel", 3.0);
        const double cooldown    = cfg.GetDouble(L"FishWhisperer", L"Cooldown",   180.0);
        const double duration    = cfg.GetDouble(L"FishWhisperer", L"Duration",    60.0);
        const bool   showNotify  = cfg.GetBool  (L"FishWhisperer", L"ShowNotification", true);
        const double dbgForce    = cfg.GetDouble(L"Debug", L"ForceFishingLevel", 0.0);
        const double effUnlock   = (dbgForce > 0.0) ? 0.0 : unlockLevel;

        if (g_CD_FishWhisperer.valid)
        {
            const double elapsed = NowSeconds() - g_CD_FishWhisperer.lastUsed;
            if (elapsed < cooldown)
            {
                int remaining = static_cast<int>(cooldown - elapsed);
                SA_LOG("[Ability] Fish Whisperer BLOCKED: cooldown {}s left\n", remaining);
                if (showNotify) Notify::Show(Format(L"Fish Whisperer: %d s", remaining), 1.5);
                return;
            }
        }
        if (!g_Player || !g_PassiveSkills)
        {
            SA_LOG("[Ability] Fish Whisperer FAILED: missing components\n");
            return;
        }

        double fishLvl = 0.0;
        ReadDouble(g_PassiveSkills, Off::PassiveSkills_FishingLvl, fishLvl);
        if (fishLvl < effUnlock)
        {
            SA_LOG("[Ability] Fish Whisperer LOCKED: Fishing {:.2f} < {:.2f}\n",
                   fishLvl, unlockLevel);
            if (showNotify)
            {
                const std::wstring k = GetHotkeyDisplay(L"FishWhisperer", L"K");
                Notify::Show(Format(
                    L"FISH WHISPERER [%s] - locked\n"
                    L"Fishing %.1f / %.1f\n"
                    L"Catch fish with a rod to train", k.c_str(), fishLvl, unlockLevel), 3.0);
            }
            return;
        }

        UObject* fishComp = ReadObject(g_Player, Off::Player_FishingComponent);
        if (!fishComp)
        {
            SA_LOG("[Ability] Fish Whisperer BLOCKED: no FishingComponent (equip rod)\n");
            if (showNotify)
                Notify::Show(L"Fish Whisperer\nEquip a fishing rod first", 2.5);
            return;
        }
        if (g_FishWhispererActive)
        {
            SA_LOG("[Ability] Fish Whisperer BLOCKED: already active\n");
            return;
        }

        ReadDouble(fishComp, Off::FishingComp_ChanceToCatch, g_FishWhispererOrigChance);
        WriteDouble(fishComp, Off::FishingComp_ChanceToCatch, 1.0);

        g_FishWhispererActive  = true;
        g_FishWhispererEndTime = NowSeconds() + duration;
        g_CD_FishWhisperer.lastUsed = NowSeconds();
        g_CD_FishWhisperer.valid    = true;

        SA_LOG("[Ability] Fish Whisperer ON: chance {:.2f} -> 1.00 for {:.0f}s\n",
               g_FishWhispererOrigChance, duration);

        if (showNotify)
            Notify::Show(Format(L"Fish Whisperer active\n100%% catch for %.0fs", duration), 2.5);
    }

    static void TickFishWhisperer()
    {
        if (!g_FishWhispererActive) return;
        if (NowSeconds() < g_FishWhispererEndTime) return;

        if (g_Player)
        {
            UObject* fishComp = ReadObject(g_Player, Off::Player_FishingComponent);
            if (fishComp)
                WriteDouble(fishComp, Off::FishingComp_ChanceToCatch,
                            g_FishWhispererOrigChance);
        }
        g_FishWhispererActive = false;
        SA_LOG("[Ability] Fish Whisperer OFF (chance restored to {:.2f})\n",
               g_FishWhispererOrigChance);
    }

    // =================================================================
    // Loot Sense
    // =================================================================
    static double g_LootSenseEndTime = 0.0;
    static bool   g_LootSenseActive = false;
    static std::vector<UObject*> g_LootSenseHighlighted;

    static void TryActivateLootSense()
    {
        SA_LOG("[Ability] Loot Sense pressed\n");

        Config& cfg = Config::Get();
        const double unlockLevel = cfg.GetDouble(L"LootSense", L"UnlockLevel", 3.0);
        const double cooldown    = cfg.GetDouble(L"LootSense", L"Cooldown",   120.0);
        const double duration    = cfg.GetDouble(L"LootSense", L"Duration",    10.0);
        const double radius      = cfg.GetDouble(L"LootSense", L"Radius",    2500.0);
        const int    stencil     = cfg.GetInt   (L"LootSense", L"StencilValue",   1);
        const bool   showNotify  = cfg.GetBool  (L"LootSense", L"ShowNotification", true);
        const double dbgForce    = cfg.GetDouble(L"Debug", L"ForceScavengingLevel", 0.0);
        const double effUnlock   = (dbgForce > 0.0) ? 0.0 : unlockLevel;

        if (g_CD_LootSense.valid)
        {
            const double elapsed = NowSeconds() - g_CD_LootSense.lastUsed;
            if (elapsed < cooldown)
            {
                int remaining = static_cast<int>(cooldown - elapsed);
                SA_LOG("[Ability] Loot Sense BLOCKED: cooldown {}s left\n", remaining);
                if (showNotify) Notify::Show(Format(L"Loot Sense: %d s", remaining), 1.5);
                return;
            }
        }
        if (!g_Player || !g_PassiveSkills)
        {
            SA_LOG("[Ability] Loot Sense FAILED: missing components\n");
            return;
        }

        double scavLvl = 0.0;
        ReadDouble(g_PassiveSkills, Off::PassiveSkills_ScavengLvl, scavLvl);
        if (scavLvl < effUnlock)
        {
            SA_LOG("[Ability] Loot Sense LOCKED: Scavenging {:.2f} < {:.2f}\n",
                   scavLvl, unlockLevel);
            if (showNotify)
            {
                const std::wstring k = GetHotkeyDisplay(L"LootSense", L"H");
                Notify::Show(Format(
                    L"LOOT SENSE [%s] - locked\n"
                    L"Scavenging %.1f / %.1f\n"
                    L"Loot containers to train", k.c_str(), scavLvl, unlockLevel), 3.0);
            }
            return;
        }
        if (g_LootSenseActive)
        {
            SA_LOG("[Ability] Loot Sense BLOCKED: already active\n");
            return;
        }

        double px = 0.0, py = 0.0, pz = 0.0;
        if (!GetActorLocation(g_Player, px, py, pz))
        {
            SA_LOG("[Ability] Loot Sense FAILED: no player location\n");
            return;
        }

        std::vector<UObject*> all;
        UObjectGlobals::FindAllOf(STR("BP_LootContainer_C"), all);

        std::vector<UObject*> containers;
        containers.reserve(all.size());
        for (UObject* o : all)
            if (IsInstanceOfClass(o, L"BP_LootContainer_C"))
                containers.push_back(o);

        g_LootSenseHighlighted.clear();
        int nearCount = 0;
        int highlightedCount = 0;
        for (UObject* c : containers)
        {
            if (!c) continue;
            double cx = 0.0, cy = 0.0, cz = 0.0;
            if (!GetActorLocation(c, cx, cy, cz)) continue;

            const double dx = cx - px, dy = cy - py, dz = cz - pz;
            const double distSq = dx*dx + dy*dy + dz*dz;
            if (distSq > radius * radius) continue;
            ++nearCount;

            UObject* mesh = ReadObject(c, Off::LootContainer_StaticMesh);
            if (!mesh) continue;

            if (TriggerCustomDepth(mesh, true, stencil))
            {
                g_LootSenseHighlighted.push_back(mesh);
                ++highlightedCount;
            }
        }

        g_LootSenseActive  = true;
        g_LootSenseEndTime = NowSeconds() + duration;
        g_CD_LootSense.lastUsed = NowSeconds();
        g_CD_LootSense.valid    = true;

        SA_LOG("[Ability] Loot Sense ON: {} containers in {:.0f}m, {} highlighted (stencil {}), {:.0f}s\n",
               nearCount, radius / 100.0, highlightedCount, stencil, duration);

        if (showNotify)
        {
            if (nearCount > 0)
                Notify::Show(Format(L"Loot Sense active\n%d containers within %.0fm",
                                    nearCount, radius / 100.0), 2.5);
            else
                Notify::Show(L"Loot Sense active\nNo containers nearby", 2.0);
        }
    }

    static void TickLootSense()
    {
        if (!g_LootSenseActive) return;
        if (NowSeconds() < g_LootSenseEndTime) return;
        const size_t n = g_LootSenseHighlighted.size();
        for (UObject* mesh : g_LootSenseHighlighted)
            TriggerCustomDepth(mesh, false, 0);
        g_LootSenseHighlighted.clear();
        g_LootSenseActive = false;
        SA_LOG("[Ability] Loot Sense OFF ({} meshes restored)\n", n);
    }

    // =================================================================
    // Quick Hands
    // =================================================================
    static void TryActivateQuickHands()
    {
        SA_LOG("[Ability] Quick Hands pressed\n");

        Config& cfg = Config::Get();
        const double unlockLevel = cfg.GetDouble(L"QuickHands", L"UnlockLevel", 3.0);
        const double cooldown    = cfg.GetDouble(L"QuickHands", L"Cooldown",    60.0);
        const bool   showNotify  = cfg.GetBool  (L"QuickHands", L"ShowNotification", true);
        const double dbgForce    = cfg.GetDouble(L"Debug", L"ForceReloadingLevel", 0.0);
        const double effUnlock   = (dbgForce > 0.0) ? 0.0 : unlockLevel;

        if (g_CD_QuickHands.valid)
        {
            const double elapsed = NowSeconds() - g_CD_QuickHands.lastUsed;
            if (elapsed < cooldown)
            {
                int remaining = static_cast<int>(cooldown - elapsed);
                SA_LOG("[Ability] Quick Hands BLOCKED: cooldown {}s left\n", remaining);
                if (showNotify) Notify::Show(Format(L"Quick Hands: %d s", remaining), 1.5);
                return;
            }
        }
        if (!g_Player || !g_PassiveSkills)
        {
            SA_LOG("[Ability] Quick Hands FAILED: missing components\n");
            return;
        }

        double relLvl = 0.0;
        ReadDouble(g_PassiveSkills, Off::PassiveSkills_ReloadLvl, relLvl);
        if (relLvl < effUnlock)
        {
            SA_LOG("[Ability] Quick Hands LOCKED: Reloading {:.2f} < {:.2f}\n",
                   relLvl, unlockLevel);
            if (showNotify)
            {
                const std::wstring k = GetHotkeyDisplay(L"QuickHands", L"G");
                Notify::Show(Format(
                    L"QUICK HANDS [%s] - locked\n"
                    L"Reloading %.1f / %.1f\n"
                    L"Reload weapons to train", k.c_str(), relLvl, unlockLevel), 3.0);
            }
            return;
        }

        UObject* weapon = GetEquippedFirearm();
        if (!weapon)
        {
            SA_LOG("[Ability] Quick Hands BLOCKED: no firearm equipped\n");
            if (showNotify)
                Notify::Show(L"Quick Hands\nEquip a firearm first", 2.5);
            return;
        }

        if (!g_JigMultiComp)
        {
            SA_LOG("[Ability] Quick Hands FAILED: no JigMultiComp\n");
            return;
        }

        uint8_t activeSlot[8] = {};
        for (int i = 0; i < 8; ++i)
            activeSlot[i] =
                reinterpret_cast<uint8_t*>(weapon)[Off::Firearm_PlayerActiveWeapon + i];

        UFunction* fnCheck = weapon->GetFunctionByNameInChain(STR("Local_CheckCanReload"));
        if (!fnCheck)
        {
            SA_LOG("[Ability] Quick Hands FAILED: no Local_CheckCanReload\n");
            return;
        }

        uint8_t b1[0x40] = {};
        *reinterpret_cast<UObject**>(b1 + 0x00) = g_JigMultiComp;
        for (int i = 0; i < 8; ++i) b1[0x08 + i] = activeSlot[i];

        weapon->ProcessEvent(fnCheck, b1);

        const bool canReload    = b1[0x10] != 0;
        UObject* containerMag   = *reinterpret_cast<UObject**>(b1 + 0x18);
        UObject* reloadWith     = *reinterpret_cast<UObject**>(b1 + 0x28);

        if (!canReload)
        {
            SA_LOG("[Ability] Quick Hands BLOCKED: cannot reload (empty mag?)\n");
            if (showNotify)
                Notify::Show(L"Quick Hands\nCannot reload\n(check mag and ammo)", 2.5);
            return;
        }

        UFunction* fnStart = weapon->GetFunctionByNameInChain(STR("LocalStartReload"));
        if (!fnStart)
        {
            SA_LOG("[Ability] Quick Hands FAILED: no LocalStartReload\n");
            return;
        }

        uint8_t b2[0x30] = {};
        *reinterpret_cast<UObject**>(b2 + 0x00) = g_JigMultiComp;
        *reinterpret_cast<UObject**>(b2 + 0x08) = containerMag;
        *reinterpret_cast<UObject**>(b2 + 0x10) = reloadWith;

        weapon->ProcessEvent(fnStart, b2);

        const bool startResult = b2[0x18] != 0;

        g_CD_QuickHands.lastUsed = NowSeconds();
        g_CD_QuickHands.valid    = true;

        SA_LOG("[Ability] Quick Hands ON: weapon=0x{:X}, startResult={}\n",
               reinterpret_cast<size_t>(weapon), startResult);

        if (showNotify)
        {
            if (startResult)
                Notify::Show(L"Quick Hands\nReloaded!", 2.0);
            else
                Notify::Show(L"Quick Hands\nReload failed", 2.0);
        }
    }

    // =================================================================
    // Power Strike
    // =================================================================
    static void TryActivatePowerStrike()
    {
        SA_LOG("[Ability] Power Strike pressed\n");

        Config& cfg = Config::Get();
        const double unlockLevel = cfg.GetDouble(L"PowerStrike", L"UnlockLevel", 3.0);
        const double cooldown    = cfg.GetDouble(L"PowerStrike", L"Cooldown",    60.0);
        const double range       = cfg.GetDouble(L"PowerStrike", L"Range",      300.0);
        const double coneAngle   = cfg.GetDouble(L"PowerStrike", L"ConeAngle",  120.0);
        const double baseDamage  = cfg.GetDouble(L"PowerStrike", L"BaseDamage",  80.0);
        const bool   showNotify  = cfg.GetBool  (L"PowerStrike", L"ShowNotification", true);
        const double dbgForce    = cfg.GetDouble(L"Debug", L"ForceStrengthLevel", 0.0);
        const double effUnlock   = (dbgForce > 0.0) ? 0.0 : unlockLevel;

        if (g_CD_PowerStrike.valid)
        {
            const double elapsed = NowSeconds() - g_CD_PowerStrike.lastUsed;
            if (elapsed < cooldown)
            {
                int remaining = static_cast<int>(cooldown - elapsed);
                SA_LOG("[Ability] Power Strike BLOCKED: cooldown {}s left\n", remaining);
                if (showNotify) Notify::Show(Format(L"Power Strike: %d s", remaining), 1.5);
                return;
            }
        }
        if (!g_Player || !g_PassiveSkills)
        {
            SA_LOG("[Ability] Power Strike FAILED: missing components\n");
            return;
        }

        double strLvl = 0.0;
        ReadDouble(g_PassiveSkills, Off::PassiveSkills_StrengthLvl, strLvl);
        if (strLvl < effUnlock)
        {
            SA_LOG("[Ability] Power Strike LOCKED: Strength {:.2f} < {:.2f}\n",
                   strLvl, unlockLevel);
            if (showNotify)
            {
                const std::wstring k = GetHotkeyDisplay(L"PowerStrike", L"C");
                Notify::Show(Format(
                    L"POWER STRIKE [%s] - locked\n"
                    L"Strength %.1f / %.1f\n"
                    L"Fight in melee to train", k.c_str(), strLvl, unlockLevel), 3.0);
            }
            return;
        }

        double px = 0.0, py = 0.0, pz = 0.0;
        double fx = 1.0, fy = 0.0, fz = 0.0;
        if (!GetActorLocation(g_Player, px, py, pz))
        {
            SA_LOG("[Ability] Power Strike FAILED: no player location\n");
            return;
        }
        GetActorForwardVector(g_Player, fx, fy, fz);

        std::vector<UObject*> zombies;
        UObjectGlobals::FindAllOf(STR("BP_MasterZombie_C"), zombies);

        const double halfAngleRad = (coneAngle * 0.5) * 3.14159265358979323846 / 180.0;
        const double coneCos      = std::cos(halfAngleRad);

        const size_t totalZombies = zombies.size();
        int hitCount = 0;
        for (UObject* z : zombies)
        {
            if (!z) continue;
            if (!IsInstanceOfClass(z, L"BP_MasterZombie_C")) continue;

            double zx = 0.0, zy = 0.0, zz = 0.0;
            if (!GetActorLocation(z, zx, zy, zz)) continue;

            const double dx = zx - px, dy = zy - py, dz = zz - pz;
            const double dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (dist > range) continue;

            if (dist > 0.01)
            {
                const double ndx = dx / dist, ndy = dy / dist;
                const double dotXY = ndx * fx + ndy * fy;
                if (dotXY < coneCos) continue;
            }

            UFunction* fnDmg = z->GetFunctionByNameInChain(STR("Damage_Object"));
            if (!fnDmg) continue;

            uint8_t buf[0x30] = {};
            *reinterpret_cast<double*>(buf + 0x00) = baseDamage;
            *reinterpret_cast<UObject**>(buf + 0x08) = g_Player;
            *reinterpret_cast<UObject**>(buf + 0x10) = g_PlayerController;

            z->ProcessEvent(fnDmg, buf);
            ++hitCount;
        }

        g_CD_PowerStrike.lastUsed = NowSeconds();
        g_CD_PowerStrike.valid    = true;

        SA_LOG("[Ability] Power Strike ON: {} of {} zombies hit (dmg {:.0f}, range {:.0f}cm, cone {:.0f}deg)\n",
               hitCount, totalZombies, baseDamage, range, coneAngle);

        if (showNotify)
        {
            if (hitCount > 0)
                Notify::Show(Format(L"Power Strike\n%d zombies hit (%.0f dmg)",
                                    hitCount, baseDamage), 2.5);
            else
                Notify::Show(L"Power Strike\nNo targets in cone", 2.0);
        }
    }

    // =================================================================
    // Ultimate
    // =================================================================
    static double g_UltimateEndTime = 0.0;
    static double g_UltOrigMaxHP = 0.0;

    static void TryActivateUltimate()
    {
        SA_LOG("[Ability] Ultimate pressed\n");

        Config& cfg = Config::Get();
        const double reqLevel     = cfg.GetDouble(L"Ultimate", L"RequiredSkillLevel", 10.0);
        const int    reqCount     = cfg.GetInt   (L"Ultimate", L"RequiredSkillsAtLevel10", 5);
        const double cooldown     = cfg.GetDouble(L"Ultimate", L"Cooldown",   600.0);
        const double duration     = cfg.GetDouble(L"Ultimate", L"Duration",    20.0);
        const double speedMult    = cfg.GetDouble(L"Ultimate", L"SpeedMultiplier", 1.5);
        const double bonusHP      = cfg.GetDouble(L"Ultimate", L"BonusHP",    200.0);
        const bool   showNotify   = cfg.GetBool  (L"Ultimate", L"ShowNotification", true);
        const bool   dbgForce     = cfg.GetBool  (L"Debug", L"ForceUltimateUnlock", false);

        if (g_CD_Ultimate.valid)
        {
            const double elapsed = NowSeconds() - g_CD_Ultimate.lastUsed;
            if (elapsed < cooldown)
            {
                int remaining = static_cast<int>(cooldown - elapsed);
                SA_LOG("[Ability] Ultimate BLOCKED: cooldown {}s left\n", remaining);
                if (showNotify) Notify::Show(Format(L"Ultimate: %d s", remaining), 1.5);
                return;
            }
        }
        if (!g_Player || !g_PassiveSkills)
        {
            SA_LOG("[Ability] Ultimate FAILED: missing components\n");
            return;
        }

        if (!dbgForce)
        {
            const int count = CountSkillsAtLevel(g_PassiveSkills, reqLevel);
            if (count < reqCount)
            {
                SA_LOG("[Ability] Ultimate LOCKED: {}/{} skills at lvl {:.0f}+\n",
                       count, reqCount, reqLevel);
                if (showNotify)
                {
                    const std::wstring k = GetHotkeyDisplay(L"Ultimate", L"U");
                    Notify::Show(Format(
                        L"ADRENALINE [%s] - locked\n"
                        L"Need %d skills at lvl %.0f+\n"
                        L"Currently: %d / %d",
                        k.c_str(), reqCount, reqLevel, count, reqCount), 3.0);
                }
                return;
            }
        }
        if (g_UltimateActive)
        {
            SA_LOG("[Ability] Ultimate BLOCKED: already active\n");
            return;
        }

        if (g_CharacterMovement)
        {
            ResetSpeedBoost(g_UltBoost, speedMult);
            ApplySpeedBoost(g_UltBoost);
        }

        double hp = 0.0, maxHP = 0.0;
        ReadDouble(g_MedicalComp, Off::Medical_Health,    hp);
        ReadDouble(g_MedicalComp, Off::Medical_MaxHealth, maxHP);
        g_UltOrigMaxHP = maxHP;

        WriteDouble(g_MedicalComp, Off::Medical_MaxHealth, maxHP + bonusHP);
        WriteDouble(g_MedicalComp, Off::Medical_Health,    hp + bonusHP);
        UpdateHealthUI(hp + bonusHP);

        g_UltimateActive  = true;
        g_UltimateEndTime = NowSeconds() + duration;
        g_CD_Ultimate.lastUsed = NowSeconds();
        g_CD_Ultimate.valid    = true;

        SA_LOG("[Ability] Ultimate ON: +{:.0f} HP, x{:.2f} speed, {:.0f}s (base walk {:.0f} -> {:.0f}, cd {:.0f}s)\n",
               bonusHP, speedMult, duration,
               g_UltBoost.playerBaseW,
               g_UltBoost.playerBaseW * static_cast<float>(speedMult),
               cooldown);

        if (showNotify)
            Notify::Show(Format(
                L"ADRENALINE OVERDRIVE\n"
                L"+%.0f HP, x%.1f speed, %.0fs",
                bonusHP, speedMult, duration), 3.0);
    }

    static void TickUltimate()
    {
        if (!g_UltimateActive) return;

        const double now = NowSeconds();

        if (now < g_UltimateEndTime)
        {
            ApplySpeedBoost(g_UltBoost);
            ProbeSpeedBoost(L"Ultimate", g_UltBoost);
            return;
        }

        Config& cfg = Config::Get();
        const double hpPenaltyPct = cfg.GetDouble(L"Ultimate", L"EndHpPenalty", 0.30);

        RestoreSpeedBoost(g_UltBoost);

        double hpBefore    = 0.0;
        double hpAfter     = 0.0;
        double formulaPen  = 0.0;
        if (g_MedicalComp)
        {
            double hp = 0.0;
            ReadDouble(g_MedicalComp, Off::Medical_Health, hp);
            hpBefore = hp;

            formulaPen = g_UltOrigMaxHP * hpPenaltyPct;
            hp = hp - formulaPen;
            if (hp < 1.0) hp = 1.0;
            if (hp > g_UltOrigMaxHP) hp = g_UltOrigMaxHP;
            hpAfter = hp;

            WriteDouble(g_MedicalComp, Off::Medical_MaxHealth, g_UltOrigMaxHP);
            WriteDouble(g_MedicalComp, Off::Medical_Health,    hp);
            UpdateHealthUI(hp);
        }
        g_UltimateActive = false;

        SA_LOG("[Ability] Ultimate OFF: HP {:.0f} -> {:.0f} (actual loss {:.0f}; formula {:.0f} = {:.0f}% of base {:.0f}; speed restored to {:.0f})\n",
               hpBefore, hpAfter, (hpBefore - hpAfter),
               formulaPen, hpPenaltyPct * 100.0, g_UltOrigMaxHP,
               g_UltBoost.playerBaseW);
    }

    // =================================================================
    // Spellbook  (multi-line notification via F1)
    // =================================================================
    void TriggerSpellbook()
    {
        SA_LOG("[Spellbook] hotkey pressed\n");

        FindPlayer();
        if (g_Player && !g_PassiveSkills)
            FindComponents();

        Config& cfg = Config::Get();

        if (!g_PassiveSkills || !g_Player)
        {
            SA_LOG("[Spellbook] FAILED: no player / passive skills yet\n");
            Notify::Show(L"SPELLBOOK\n(enter a save game first)", 3.0);
            return;
        }

        // Read hotkeys fresh from INI. Edit the INI, press F1 again, and the
        // new keys show up immediately (no restart needed for the display).
        const std::wstring kSprint = GetHotkeyDisplay(L"SprintBurst",   L"X");
        const std::wstring kPower  = GetHotkeyDisplay(L"PowerStrike",   L"C");
        const std::wstring kIron   = GetHotkeyDisplay(L"IronSkin",      L"J");
        const std::wstring kMedic  = GetHotkeyDisplay(L"FieldMedic",    L"N");
        const std::wstring kQuick  = GetHotkeyDisplay(L"QuickHands",    L"G");
        const std::wstring kFish   = GetHotkeyDisplay(L"FishWhisperer", L"K");
        const std::wstring kLoot   = GetHotkeyDisplay(L"LootSense",     L"H");
        const std::wstring kUlt    = GetHotkeyDisplay(L"Ultimate",      L"U");

        double fit=0, str=0, tou=0, snk=0, fa=0, mar=0, rel=0, thi=0, fis=0, sca=0;
        ReadDouble(g_PassiveSkills, Off::PassiveSkills_FitnessLvl,  fit);
        ReadDouble(g_PassiveSkills, Off::PassiveSkills_StrengthLvl, str);
        ReadDouble(g_PassiveSkills, Off::PassiveSkills_ToughnessLvl,tou);
        ReadDouble(g_PassiveSkills, Off::PassiveSkills_SneakingLvl, snk);
        ReadDouble(g_PassiveSkills, Off::PassiveSkills_FirstAidLvl, fa);
        ReadDouble(g_PassiveSkills, Off::PassiveSkills_MarksmLvl,   mar);
        ReadDouble(g_PassiveSkills, Off::PassiveSkills_ReloadLvl,   rel);
        ReadDouble(g_PassiveSkills, Off::PassiveSkills_ThiefLvl,    thi);
        ReadDouble(g_PassiveSkills, Off::PassiveSkills_FishingLvl,  fis);
        ReadDouble(g_PassiveSkills, Off::PassiveSkills_ScavengLvl,  sca);

        std::wstring text = L"SPELLBOOK  (F1 to toggle)\n";

        auto addLine = [&](const wchar_t* name, const wchar_t* key,
                           double lvl, double req,
                           const Cooldown& cd, double cdSec,
                           bool isActive)
        {
            wchar_t line[128];
            const wchar_t* status;
            wchar_t statusBuf[64];

            if (isActive)
            {
                swprintf_s(statusBuf, _countof(statusBuf), L"ACTIVE");
                status = statusBuf;
            }
            else if (cd.valid)
            {
                const double elapsed = NowSeconds() - cd.lastUsed;
                if (elapsed < cdSec)
                {
                    int rem = static_cast<int>(cdSec - elapsed);
                    swprintf_s(statusBuf, _countof(statusBuf), L"CD %ds", rem);
                    status = statusBuf;
                }
                else if (lvl < req)
                {
                    swprintf_s(statusBuf, _countof(statusBuf), L"%.1f/%.1f", lvl, req);
                    status = statusBuf;
                }
                else
                {
                    swprintf_s(statusBuf, _countof(statusBuf), L"READY");
                    status = statusBuf;
                }
            }
            else if (lvl < req)
            {
                swprintf_s(statusBuf, _countof(statusBuf), L"%.1f/%.1f", lvl, req);
                status = statusBuf;
            }
            else
            {
                swprintf_s(statusBuf, _countof(statusBuf), L"READY");
                status = statusBuf;
            }

            swprintf_s(line, _countof(line), L"[%s] %-16s %s\n", key, name, status);
            text += line;
        };

        addLine(L"Sprint Burst",    kSprint.c_str(), fit,
                cfg.GetDouble(L"SprintBurst", L"UnlockLevel", 3.0),
                g_CD_SprintBurst, cfg.GetDouble(L"SprintBurst", L"Cooldown", 180.0),
                g_SprintActive);

        addLine(L"Power Strike",    kPower.c_str(), str,
                cfg.GetDouble(L"PowerStrike", L"UnlockLevel", 3.0),
                g_CD_PowerStrike, cfg.GetDouble(L"PowerStrike", L"Cooldown", 60.0),
                false);

        addLine(L"Iron Skin",       kIron.c_str(), tou,
                cfg.GetDouble(L"IronSkin", L"UnlockLevel", 3.0),
                g_CD_IronSkin, cfg.GetDouble(L"IronSkin", L"Cooldown", 300.0),
                g_IronSkinActive);

        addLine(L"Field Medic",     kMedic.c_str(), fa,
                cfg.GetDouble(L"FieldMedic", L"UnlockLevel", 3.0),
                g_CD_FieldMedic, cfg.GetDouble(L"FieldMedic", L"Cooldown", 90.0),
                false);

        addLine(L"Quick Hands",     kQuick.c_str(), rel,
                cfg.GetDouble(L"QuickHands", L"UnlockLevel", 3.0),
                g_CD_QuickHands, cfg.GetDouble(L"QuickHands", L"Cooldown", 60.0),
                false);

        addLine(L"Fish Whisperer",  kFish.c_str(), fis,
                cfg.GetDouble(L"FishWhisperer", L"UnlockLevel", 3.0),
                g_CD_FishWhisperer, cfg.GetDouble(L"FishWhisperer", L"Cooldown", 180.0),
                g_FishWhispererActive);

        addLine(L"Loot Sense",      kLoot.c_str(), sca,
                cfg.GetDouble(L"LootSense", L"UnlockLevel", 3.0),
                g_CD_LootSense, cfg.GetDouble(L"LootSense", L"Cooldown", 120.0),
                g_LootSenseActive);

        // Ultimate line
        {
            const double reqLvl = cfg.GetDouble(L"Ultimate", L"RequiredSkillLevel", 10.0);
            const int    reqCnt = cfg.GetInt   (L"Ultimate", L"RequiredSkillsAtLevel10", 5);
            const bool   force  = cfg.GetBool  (L"Debug", L"ForceUltimateUnlock", false);
            const int    cnt    = force ? reqCnt : CountSkillsAtLevel(g_PassiveSkills, reqLvl);

            wchar_t line[128];
            const wchar_t* status;
            wchar_t statusBuf[64];

            if (g_UltimateActive)
            {
                swprintf_s(statusBuf, _countof(statusBuf), L"ACTIVE");
                status = statusBuf;
            }
            else if (g_CD_Ultimate.valid)
            {
                const double cdSec = cfg.GetDouble(L"Ultimate", L"Cooldown", 600.0);
                const double elapsed = NowSeconds() - g_CD_Ultimate.lastUsed;
                if (elapsed < cdSec)
                {
                    int rem = static_cast<int>(cdSec - elapsed);
                    swprintf_s(statusBuf, _countof(statusBuf), L"CD %ds", rem);
                    status = statusBuf;
                }
                else if (cnt < reqCnt && !force)
                {
                    swprintf_s(statusBuf, _countof(statusBuf), L"%d/%d skills", cnt, reqCnt);
                    status = statusBuf;
                }
                else
                {
                    swprintf_s(statusBuf, _countof(statusBuf), L"READY");
                    status = statusBuf;
                }
            }
            else if (cnt < reqCnt && !force)
            {
                swprintf_s(statusBuf, _countof(statusBuf), L"%d/%d skills", cnt, reqCnt);
                status = statusBuf;
            }
            else
            {
                swprintf_s(statusBuf, _countof(statusBuf), L"READY");
                status = statusBuf;
            }

            swprintf_s(line, _countof(line), L"[%s] %-16s %s\n", kUlt.c_str(), L"Adrenaline", status);
            text += line;
        }

        // Footer with skill levels
        wchar_t footer[256];
        swprintf_s(footer, _countof(footer),
            L"\nFit%.1f Str%.1f Tou%.1f Snk%.1f\nFA%.1f Mrk%.1f Rel%.1f Thi%.1f\nFis%.1f Scv%.1f",
            fit, str, tou, snk, fa, mar, rel, thi, fis, sca);
        text += footer;

        Notify::Show(text, 10.0);

        SA_LOG("[Spellbook] shown in-game (multi-line notification, 10s)\n");
    }

    // =================================================================
    // Unlock notifications
    // =================================================================
    struct UnlockWatch
    {
        size_t  offset;
        double  prevLevel;
        const wchar_t* abilityName;
        const wchar_t* hotkeyIniName;   // for live INI lookup
        const wchar_t* hotkeyFallback;
        double  threshold;
    };

    static UnlockWatch g_Watches[] = {
        { Off::PassiveSkills_FitnessLvl,  0.0, L"Sprint Burst",     L"SprintBurst",   L"X", 0.0 },
        { Off::PassiveSkills_StrengthLvl, 0.0, L"Power Strike",     L"PowerStrike",   L"C", 0.0 },
        { Off::PassiveSkills_ToughnessLvl,0.0, L"Iron Skin",        L"IronSkin",      L"J", 0.0 },
        { Off::PassiveSkills_FirstAidLvl, 0.0, L"Field Medic",      L"FieldMedic",    L"N", 0.0 },
        { Off::PassiveSkills_ReloadLvl,   0.0, L"Quick Hands",      L"QuickHands",    L"G", 0.0 },
        { Off::PassiveSkills_FishingLvl,  0.0, L"Fish Whisperer",   L"FishWhisperer", L"K", 0.0 },
        { Off::PassiveSkills_ScavengLvl,  0.0, L"Loot Sense",       L"LootSense",     L"H", 0.0 },
    };

    static bool g_WatchesInitialized = false;

    static void TickUnlockWatch()
    {
        if (!g_PassiveSkills) return;

        Config& cfg = Config::Get();

        if (!g_WatchesInitialized)
        {
            g_Watches[0].threshold = cfg.GetDouble(L"SprintBurst",   L"UnlockLevel", 3.0);
            g_Watches[1].threshold = cfg.GetDouble(L"PowerStrike",   L"UnlockLevel", 3.0);
            g_Watches[2].threshold = cfg.GetDouble(L"IronSkin",      L"UnlockLevel", 3.0);
            g_Watches[3].threshold = cfg.GetDouble(L"FieldMedic",    L"UnlockLevel", 3.0);
            g_Watches[4].threshold = cfg.GetDouble(L"QuickHands",    L"UnlockLevel", 3.0);
            g_Watches[5].threshold = cfg.GetDouble(L"FishWhisperer", L"UnlockLevel", 3.0);
            g_Watches[6].threshold = cfg.GetDouble(L"LootSense",     L"UnlockLevel", 3.0);

            for (auto& w : g_Watches)
            {
                double lvl = 0.0;
                ReadDouble(g_PassiveSkills, w.offset, lvl);
                w.prevLevel = lvl;
            }
            g_WatchesInitialized = true;
            return;
        }

        for (auto& w : g_Watches)
        {
            double lvl = 0.0;
            ReadDouble(g_PassiveSkills, w.offset, lvl);

            if (w.prevLevel < w.threshold && lvl >= w.threshold)
            {
                // Fresh INI lookup so the shown key matches whatever the user
                // has configured right now.
                const std::wstring k = GetHotkeyDisplay(w.hotkeyIniName, w.hotkeyFallback);

                Notify::Show(Format(
                    L"UNLOCKED!\n%s [%s] available",
                    w.abilityName, k.c_str()), 5.0);

                SA_LOG("[Unlock] {} unlocked at level {:.1f} (threshold {:.1f})\n",
                       w.abilityName, lvl, w.threshold);
            }

            w.prevLevel = lvl;
        }
    }

    // =================================================================
    // Public API
    // =================================================================
    void SetConfigPath(const std::wstring& path) { Config::Get().SetPath(path); }

    void Install()
    {
        Config& cfg = Config::Get();
        g_bEnabled = cfg.GetBool(L"General", L"Enabled",    true);
        g_Verbose  = cfg.GetBool(L"General", L"VerboseLog", false);

        SA_LOG("[SurvivorAbilities] {} Install() -- init OK (enabled={}, verbose={})\n",
               SA_VERSION,
               g_bEnabled ? L"true" : L"false",
               g_Verbose  ? L"true" : L"false");
    }

    void Shutdown()
    {
        g_bShutdown.store(true);
        g_Player               = nullptr;
        g_PlayerController     = nullptr;
        g_MedicalComp          = nullptr;
        g_PassiveSkills        = nullptr;
        g_CharacterMovement    = nullptr;
        g_JigHelperComp        = nullptr;
        g_JigMultiComp         = nullptr;
        g_FnClientUpdateHealth = nullptr;
    }

    void Update()
    {
        if (g_bShutdown.load()) return;
        if (!g_bEnabled) return;
        if (UE4SSProgram::unreal_is_shutting_down) return;
        if (g_bTeardownDetected.load(std::memory_order_relaxed)) return;

        FindPlayer();
        if (!g_Player) return;
        FindComponents();

        TickSprintBurst();
        TickIronSkin();
        TickFishWhisperer();
        TickLootSense();
        TickUltimate();
        TickUnlockWatch();

        if (g_Verbose)
        {
            const double now = NowSeconds();
            if (g_LastHeartbeatTime == 0.0) g_LastHeartbeatTime = now;
            if (now - g_LastHeartbeatTime >= 5.0)
            {
                g_LastHeartbeatTime = now;
                ++g_Heartbeats;
                SA_LOG("[SurvivorAbilities] heartbeat #{} player=0x{:X}\n",
                       g_Heartbeats, reinterpret_cast<size_t>(g_Player));
            }
        }
    }

    void OnHotkeyPressed(Hotkey key)
    {
        if (!g_bEnabled) return;
        if (g_bTeardownDetected.load(std::memory_order_relaxed)) return;

        FindPlayer();
        if (g_Player && (!g_MedicalComp || !g_PassiveSkills || !g_FnClientUpdateHealth))
            FindComponents();

        switch (key)
        {
            case Hotkey::SprintBurst:   TryActivateSprintBurst();   break;
            case Hotkey::PowerStrike:   TryActivatePowerStrike();   break;
            case Hotkey::IronSkin:      TryActivateIronSkin();      break;
            case Hotkey::FieldMedic:    TryActivateFieldMedic();    break;
            case Hotkey::QuickHands:    TryActivateQuickHands();    break;
            case Hotkey::FishWhisperer: TryActivateFishWhisperer(); break;
            case Hotkey::LootSense:     TryActivateLootSense();     break;
            case Hotkey::Ultimate:      TryActivateUltimate();      break;
        }
    }
}