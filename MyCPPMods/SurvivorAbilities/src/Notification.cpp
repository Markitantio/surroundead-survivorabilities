// Notification.cpp
#include "Notification.hpp"

#include <UE4SSProgram.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace SurvivorAbilities
{
    namespace Notify
    {
        // ---------------------------------------------------------------
        // Cached things that NEVER go stale within a game session:
        //   * g_FnCreateNotification  -- UFunction on BP_PlayerCharacter_C class
        //   * g_KismetTextLibrary     -- engine CDO, lives forever
        //   * g_FnStringToText        -- UFunction on KismetTextLibrary CDO
        //
        // What we deliberately do NOT cache:
        //   * the player pawn. It gets destroyed on death/respawn and the
        //     old pointer became a use-after-free hazard. We resolve it
        //     fresh on every Show() call instead.
        // ---------------------------------------------------------------
        static UFunction* g_FnCreateNotification = nullptr;
        static UFunction* g_FnStringToText       = nullptr;
        static UObject*   g_KismetTextLibrary    = nullptr;

        static bool g_Initialized = false;
        static bool g_InitFailed  = false;

        // ---------------------------------------------------------------
        // Verbose logging helper (same pattern as SurvivorAbilitiesHooks.cpp)
        // ---------------------------------------------------------------
        #define SA_LOG_NOTIFY(fmt, ...) \
            Output::send<LogLevel::Verbose>(STR(fmt) __VA_OPT__(,) __VA_ARGS__)

        // ---------------------------------------------------------------
        // Find KismetTextLibrary. WeaponProgression (Lua) uses:
        //   StaticFindObject("/Script/Engine.Default__KismetTextLibrary")
        // We mirror that approach with multiple fallbacks.
        // ---------------------------------------------------------------
        static UObject* FindKismetTextLibrary()
        {
            // Strategy 1: full CDO path (same as WeaponProgression Lua)
            if (auto* o = UObjectGlobals::StaticFindObject(
                    nullptr, nullptr,
                    STR("/Script/Engine.Default__KismetTextLibrary")))
            {
                SA_LOG_NOTIFY("[Notify] KTL via StaticFindObject(full CDO path)\n");
                return o;
            }

            // Strategy 2: short CDO name
            if (auto* o = UObjectGlobals::StaticFindObject(
                    nullptr, nullptr,
                    STR("Default__KismetTextLibrary")))
            {
                SA_LOG_NOTIFY("[Notify] KTL via StaticFindObject(short CDO name)\n");
                return o;
            }

            // Strategy 3: FindFirstOf by class name (last resort)
            if (auto* o = UObjectGlobals::FindFirstOf(STR("KismetTextLibrary")))
            {
                SA_LOG_NOTIFY("[Notify] KTL via FindFirstOf('KismetTextLibrary')\n");
                return o;
            }

            SA_LOG_NOTIFY("[Notify] KTL: all strategies failed\n");
            return nullptr;
        }

        // ---------------------------------------------------------------
        // Resolve the pawn the local player is ACTUALLY controlling right
        // now. This is critical after death/respawn: FindFirstOf may return
        // a stale/spectator/temp pawn, but PlayerController->GetPawn() only
        // ever returns the one the HUD is currently attached to.
        //
        // Falls back to FindFirstOf("BP_PlayerCharacter_C") if the PC path
        // fails for any reason.
        // ---------------------------------------------------------------
        static UObject* ResolveCurrentPawn()
        {
            // Try PlayerController route first (authoritative)
            if (UObject* pc = UObjectGlobals::FindFirstOf(STR("PlayerController")))
            {
                // K2_GetPawn is the Blueprint-callable name
                if (UFunction* fnGetPawn = pc->GetFunctionByNameInChain(STR("K2_GetPawn")))
                {
                    uint8_t buf[0x10] = {};
                    *reinterpret_cast<UObject**>(buf + 0x00) = nullptr;
                    pc->ProcessEvent(fnGetPawn, buf);
                    if (UObject* p = *reinterpret_cast<UObject**>(buf + 0x00))
                        return p;
                }

                // Native fallback name
                if (UFunction* fnGetPawn2 = pc->GetFunctionByNameInChain(STR("GetPawn")))
                {
                    uint8_t buf[0x10] = {};
                    *reinterpret_cast<UObject**>(buf + 0x00) = nullptr;
                    pc->ProcessEvent(fnGetPawn2, buf);
                    if (UObject* p = *reinterpret_cast<UObject**>(buf + 0x00))
                        return p;
                }
            }

            // Fallback: the old behaviour
            return UObjectGlobals::FindFirstOf(STR("BP_PlayerCharacter_C"));
        }

        // ---------------------------------------------------------------
        // Initialize the class-level caches. Needs a live pawn instance
        // purely as a reference through which to discover the class-level
        // UFunction CreateNotificationUI.
        // ---------------------------------------------------------------
        static void InitializeInternal(UObject* pawn)
        {
            if (g_Initialized || g_InitFailed) return;
            if (!pawn) return;

            g_FnCreateNotification = pawn->GetFunctionByNameInChain(STR("CreateNotificationUI"));
            if (!g_FnCreateNotification)
            {
                g_InitFailed = true;
                SA_LOG_NOTIFY("[Notify] init failed: CreateNotificationUI not found\n");
                return;
            }

            g_KismetTextLibrary = FindKismetTextLibrary();
            if (!g_KismetTextLibrary)
            {
                g_InitFailed = true;
                SA_LOG_NOTIFY("[Notify] init failed: KismetTextLibrary not found\n");
                return;
            }

            g_FnStringToText = g_KismetTextLibrary->GetFunctionByNameInChain(STR("Conv_StringToText"));
            if (!g_FnStringToText)
            {
                g_InitFailed = true;
                SA_LOG_NOTIFY("[Notify] init failed: Conv_StringToText not found on KTL\n");
                return;
            }

            g_Initialized = true;
            SA_LOG_NOTIFY("[Notify] initialized OK: CreateNotificationUI=0x{:X} KTL=0x{:X} Conv_StringToText=0x{:X}\n",
                reinterpret_cast<size_t>(g_FnCreateNotification),
                reinterpret_cast<size_t>(g_KismetTextLibrary),
                reinterpret_cast<size_t>(g_FnStringToText));
        }

        // Public wrapper kept for API compatibility (called by hooks'
        // FindComponents). It just resolves the current pawn and delegates.
        void Initialize()
        {
            UObject* pawn = ResolveCurrentPawn();
            InitializeInternal(pawn);
        }

        // ---------------------------------------------------------------
        // WString -> FText via KismetTextLibrary::Conv_StringToText.
        // The string buffer must outlive ProcessEvent because UFunction
        // reads from it during the call. We free it afterwards.
        // ---------------------------------------------------------------
        static bool StringToFText(const std::wstring& str, uint8_t* outFTextBuffer)
        {
            if (!g_FnStringToText || !g_KismetTextLibrary) return false;

            uint8_t params[0x40] = {};

            auto* fstringData = reinterpret_cast<void**>(params + 0x00);
            auto* fstringNum  = reinterpret_cast<int32*>(params + 0x08);
            auto* fstringMax  = reinterpret_cast<int32*>(params + 0x0C);

            const int32 len = static_cast<int32>(str.size()) + 1;
            wchar_t* buffer = new wchar_t[len];
            if (!buffer) return false;

            for (int32 i = 0; i < static_cast<int32>(str.size()); ++i)
            {
                buffer[i] = str[i];
            }
            buffer[str.size()] = L'\0';

            *fstringData = buffer;
            *fstringNum  = len;
            *fstringMax  = len;

            g_KismetTextLibrary->ProcessEvent(g_FnStringToText, params);

            for (int i = 0; i < 16; ++i)
            {
                outFTextBuffer[i] = params[0x10 + i];
            }

            delete[] buffer;
            return true;
        }

        // ---------------------------------------------------------------
        // Show a native in-game notification.
        //
        // Always resolves a FRESH pawn via PlayerController->GetPawn().
        // Never caches it across frames — doing so caused an access
        // violation in UE4SS::ProcessEvent after the player died and
        // respawned. Using GetPawn() instead of FindFirstOf also makes
        // sure the notification goes to the HUD the player is currently
        // looking at (not a stale corpse / spectator pawn).
        // ---------------------------------------------------------------
        void Show(const std::wstring& text, double durationSeconds)
        {
            UObject* pawn = ResolveCurrentPawn();
            if (!pawn)
            {
                SA_LOG_NOTIFY("[Notify] Show: BAIL, no pawn (dropped notification, len={})\n",
                              text.size());
                return;
            }

            if (!g_Initialized && !g_InitFailed)
            {
                InitializeInternal(pawn);
            }
            if (!g_Initialized) return;

            uint8_t ftextBuffer[0x20] = {};
            if (!StringToFText(text, ftextBuffer))
            {
                return;
            }

            uint8_t params[0x40] = {};

            for (int i = 0; i < 16; ++i) params[0x00 + i] = ftextBuffer[i];

            // UTexture2D* = nullptr (no icon)
            *reinterpret_cast<UObject**>(params + 0x10) = nullptr;

            // FLinearColor 0.95, 0.95, 0.95, 1.0
            *reinterpret_cast<float*>(params + 0x18) = 0.95f;
            *reinterpret_cast<float*>(params + 0x1C) = 0.95f;
            *reinterpret_cast<float*>(params + 0x20) = 0.95f;
            *reinterpret_cast<float*>(params + 0x24) = 1.0f;

            *reinterpret_cast<double*>(params + 0x28) = durationSeconds;

            SA_LOG_NOTIFY("[Notify] Show: pawn=0x{:X} len={} dur={:.1f}s -> ProcessEvent\n",
                          reinterpret_cast<size_t>(pawn),
                          text.size(),
                          durationSeconds);

            pawn->ProcessEvent(g_FnCreateNotification, params);

            SA_LOG_NOTIFY("[Notify] Show: ProcessEvent returned\n");
        }
    }
}