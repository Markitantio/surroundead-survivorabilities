// KeyMapper.hpp
#pragma once

#include <string>
#include <cstdlib>
#include <cctype>
#include <Input/Handler.hpp>

namespace SurvivorAbilities
{
    // ---------------------------------------------------------------------
    // KeyFromString
    //
    // Converts a config string to RC::Input::Key.
    //
    // Strategy:
    //   * Letters A-Z  -> use named enum values (proven to work in 3.0.1)
    //   * Everything else (F-keys, NumPad, arrows, misc) -> cast the
    //     corresponding Windows Virtual-Key code to RC::Input::Key.
    //
    // This works because UE4SS 3.0.1 `Input::Key` is an `enum class`
    // whose underlying values match the VK codes for the standard keys.
    //
    // Supported string forms (case-insensitive):
    //   Letters     : "A".."Z"
    //   F-keys      : "F1".."F12"
    //   NumPad 0-9  : "N0".."N9", "NUM0".."NUM9",
    //                 "NUMPAD0".."NUMPAD9", "KP0".."KP9", "KP_0".."KP_9"
    //   NumPad ops  : "NPLUS", "NUMADD", "NUMPLUS"     -> VK_ADD
    //                 "NMINUS", "NUMSUBTRACT"           -> VK_SUBTRACT
    //                 "NMULT", "NUMMULTIPLY"            -> VK_MULTIPLY
    //                 "NDIV", "NUMDIVIDE"               -> VK_DIVIDE
    //                 "NDOT", "NUMDECIMAL", "NUMDOT"    -> VK_DECIMAL
    //                 "NENTER", "NUMENTER"              -> VK_RETURN
    //   Arrows      : "UP", "DOWN", "LEFT", "RIGHT"
    //   Misc        : "SPACE", "TAB", "ESC", "ENTER", "BACKSPACE",
    //                 "HOME", "END", "PGUP", "PGDN", "INSERT", "DELETE"
    //   Mouse       : "LMB", "RMB", "MMB"
    // ---------------------------------------------------------------------
    inline RC::Input::Key KeyFromString(const std::string& in, RC::Input::Key fallback)
    {
        if (in.empty()) return fallback;

        // Uppercase copy for case-insensitive matching
        std::string s;
        s.reserve(in.size());
        for (char c : in)
            s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));

        // -------- Single letters A-Z: use named enum (safe) --------
        if (s.size() == 1)
        {
            switch (s[0])
            {
                case 'A': return RC::Input::Key::A;
                case 'B': return RC::Input::Key::B;
                case 'C': return RC::Input::Key::C;
                case 'D': return RC::Input::Key::D;
                case 'E': return RC::Input::Key::E;
                case 'F': return RC::Input::Key::F;
                case 'G': return RC::Input::Key::G;
                case 'H': return RC::Input::Key::H;
                case 'I': return RC::Input::Key::I;
                case 'J': return RC::Input::Key::J;
                case 'K': return RC::Input::Key::K;
                case 'L': return RC::Input::Key::L;
                case 'M': return RC::Input::Key::M;
                case 'N': return RC::Input::Key::N;
                case 'O': return RC::Input::Key::O;
                case 'P': return RC::Input::Key::P;
                case 'Q': return RC::Input::Key::Q;
                case 'R': return RC::Input::Key::R;
                case 'S': return RC::Input::Key::S;
                case 'T': return RC::Input::Key::T;
                case 'U': return RC::Input::Key::U;
                case 'V': return RC::Input::Key::V;
                case 'W': return RC::Input::Key::W;
                case 'X': return RC::Input::Key::X;
                case 'Y': return RC::Input::Key::Y;
                case 'Z': return RC::Input::Key::Z;
            }
        }

        // Windows Virtual-Key codes (will be cast to Input::Key)
        // https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes
        constexpr int VK_BACK      = 0x08;
        constexpr int VK_TAB       = 0x09;
        constexpr int VK_RETURN    = 0x0D;
        constexpr int VK_ESCAPE    = 0x1B;
        constexpr int VK_SPACE     = 0x20;
        constexpr int VK_PRIOR     = 0x21; // Page Up
        constexpr int VK_NEXT      = 0x22; // Page Down
        constexpr int VK_END       = 0x23;
        constexpr int VK_HOME      = 0x24;
        constexpr int VK_LEFT      = 0x25;
        constexpr int VK_UP        = 0x26;
        constexpr int VK_RIGHT     = 0x27;
        constexpr int VK_DOWN      = 0x28;
        constexpr int VK_INSERT    = 0x2D;
        constexpr int VK_DELETE    = 0x2E;
        constexpr int VK_NUMPAD0   = 0x60;
        constexpr int VK_NUMPAD9   = 0x69;
        constexpr int VK_MULTIPLY  = 0x6A;
        constexpr int VK_ADD       = 0x6B;
        constexpr int VK_SEPARATOR = 0x6C;
        constexpr int VK_SUBTRACT  = 0x6D;
        constexpr int VK_DECIMAL   = 0x6E;
        constexpr int VK_DIVIDE    = 0x6F;
        constexpr int VK_F1        = 0x70;
        constexpr int VK_F12       = 0x7B;

        auto cast = [](int vk) -> RC::Input::Key {
            return static_cast<RC::Input::Key>(vk);
        };

        // -------- Function keys F1-F12 --------
        if (s.size() >= 2 && s[0] == 'F' && std::isdigit(static_cast<unsigned char>(s[1])))
        {
            const int n = std::atoi(s.c_str() + 1);
            if (n >= 1 && n <= 12) return cast(VK_F1 + n - 1);
        }

        // -------- NumPad digits with common prefixes --------
        // strip prefix, then expect a single digit
        auto matchNumpadDigit = [&](const std::string& body) -> int {
            if (body.size() != 1) return -1;
            const char c = body[0];
            if (c >= '0' && c <= '9') return VK_NUMPAD0 + (c - '0');
            return -1;
        };

        struct { const char* prefix; } prefixes[] = {
            { "NUMPAD" }, { "NUM" }, { "KP_" }, { "KP" }, { "N" }
        };
        for (auto& p : prefixes)
        {
            const size_t plen = std::char_traits<char>::length(p.prefix);
            if (s.size() > plen && s.compare(0, plen, p.prefix) == 0)
            {
                const std::string body = s.substr(plen);
                const int vkDigit = matchNumpadDigit(body);
                if (vkDigit >= 0) return cast(vkDigit);

                if (body == "PLUS"  || body == "+" || body == "ADD")        return cast(VK_ADD);
                if (body == "MINUS" || body == "-" || body == "SUBTRACT")   return cast(VK_SUBTRACT);
                if (body == "MULT"  || body == "MUL" || body == "*" || body == "MULTIPLY") return cast(VK_MULTIPLY);
                if (body == "DIV"   || body == "SLASH" || body == "/" || body == "DIVIDE") return cast(VK_DIVIDE);
                if (body == "DOT"   || body == "DEC" || body == "." || body == "DECIMAL" || body == "PERIOD") return cast(VK_DECIMAL);
                if (body == "ENTER" || body == "RETURN")                    return cast(VK_RETURN);
                if (body == "SEP"   || body == "SEPARATOR")                 return cast(VK_SEPARATOR);
            }
        }

        // -------- Arrow keys --------
        if (s == "UP")    return cast(VK_UP);
        if (s == "DOWN")  return cast(VK_DOWN);
        if (s == "LEFT")  return cast(VK_LEFT);
        if (s == "RIGHT") return cast(VK_RIGHT);

        // -------- Misc --------
        if (s == "SPACE")                   return cast(VK_SPACE);
        if (s == "TAB")                     return cast(VK_TAB);
        if (s == "ESC" || s == "ESCAPE")    return cast(VK_ESCAPE);
        if (s == "ENTER" || s == "RETURN")  return cast(VK_RETURN);
        if (s == "BACKSPACE" || s == "BACK")return cast(VK_BACK);
        if (s == "HOME")                    return cast(VK_HOME);
        if (s == "END")                     return cast(VK_END);
        if (s == "PGUP"  || s == "PAGEUP")  return cast(VK_PRIOR);
        if (s == "PGDN"  || s == "PAGEDOWN")return cast(VK_NEXT);
        if (s == "INSERT"|| s == "INS")     return cast(VK_INSERT);
        if (s == "DELETE"|| s == "DEL")     return cast(VK_DELETE);

        // -------- Mouse buttons (VK 0x01-0x04) --------
        if (s == "LMB" || s == "MOUSE1") return cast(0x01);
        if (s == "RMB" || s == "MOUSE2") return cast(0x02);
        if (s == "MMB" || s == "MOUSE3") return cast(0x04);

        // Fallback for unknown strings
        return fallback;
    }
}