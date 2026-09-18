// Notification.hpp
#pragma once

#include <string>

namespace SurvivorAbilities
{
    namespace Notify
    {
        // Called once after player is found. Caches function pointers.
        void Initialize();

        // Show a native game notification. Must be called from game thread
        // (on_update or a keydown handler both run on the game thread).
        void Show(const std::wstring& text, double durationSeconds);
    }
}
