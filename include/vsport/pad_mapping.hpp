#pragma once

#include "vsport/types.hpp"

#include <array>
#include <cstddef>
#include <string_view>

// Host controller -> PS1 Digital Pad mapping (pure functions, no OS headers, unit-testable).
// All mapping functions return ACTIVE-HIGH PS1 button bits; invert (~) before handing the
// state to PsxRuntime::set_pad_buttons(), which expects the PS1's active-low format.
namespace vsport::pad {

// PS1 Digital Pad button bits (SIO0 reply bytes 3..4)
constexpr u16 SELECT   = 1u << 0;
constexpr u16 L3       = 1u << 1;
constexpr u16 R3       = 1u << 2;
constexpr u16 START    = 1u << 3;
constexpr u16 UP       = 1u << 4;
constexpr u16 RIGHT    = 1u << 5;
constexpr u16 DOWN     = 1u << 6;
constexpr u16 LEFT     = 1u << 7;
constexpr u16 L2       = 1u << 8;
constexpr u16 R2       = 1u << 9;
constexpr u16 L1       = 1u << 10;
constexpr u16 R1       = 1u << 11;
constexpr u16 TRIANGLE = 1u << 12;
constexpr u16 CIRCLE   = 1u << 13;
constexpr u16 CROSS    = 1u << 14;
constexpr u16 SQUARE   = 1u << 15;

// XINPUT_GAMEPAD::wButtons bit values (identical to <Xinput.h>)
namespace xi {
constexpr u16 DPAD_UP        = 0x0001;
constexpr u16 DPAD_DOWN      = 0x0002;
constexpr u16 DPAD_LEFT      = 0x0004;
constexpr u16 DPAD_RIGHT     = 0x0008;
constexpr u16 START          = 0x0010;
constexpr u16 BACK           = 0x0020;
constexpr u16 LEFT_THUMB     = 0x0040;
constexpr u16 RIGHT_THUMB    = 0x0080;
constexpr u16 LEFT_SHOULDER  = 0x0100;
constexpr u16 RIGHT_SHOULDER = 0x0200;
constexpr u16 A              = 0x1000;
constexpr u16 B              = 0x2000;
constexpr u16 X              = 0x4000;
constexpr u16 Y              = 0x8000;
} // namespace xi

// Left stick doubles as the D-pad (Vagrant Story reads the digital pad only)
constexpr s32 XINPUT_STICK_DEADZONE = 16000; // of +-32767 (about half travel)
constexpr u8  XINPUT_TRIGGER_THRESHOLD = 64; // of 255
constexpr s32 DINPUT_STICK_DEADZONE = 500;   // of +-1000 (axis range set by the host)

// Xbox layout -> PlayStation layout by position: A=Cross, B=Circle, X=Square, Y=Triangle
[[nodiscard]] constexpr u16 map_xinput(u16 buttons, u8 left_trigger, u8 right_trigger,
                                       s16 stick_x, s16 stick_y) noexcept {
    u16 p = 0;
    if (buttons & xi::DPAD_UP)        p |= UP;
    if (buttons & xi::DPAD_DOWN)      p |= DOWN;
    if (buttons & xi::DPAD_LEFT)      p |= LEFT;
    if (buttons & xi::DPAD_RIGHT)     p |= RIGHT;
    if (buttons & xi::START)          p |= START;
    if (buttons & xi::BACK)           p |= SELECT;
    if (buttons & xi::LEFT_THUMB)     p |= L3;
    if (buttons & xi::RIGHT_THUMB)    p |= R3;
    if (buttons & xi::LEFT_SHOULDER)  p |= L1;
    if (buttons & xi::RIGHT_SHOULDER) p |= R1;
    if (buttons & xi::A)              p |= CROSS;
    if (buttons & xi::B)              p |= CIRCLE;
    if (buttons & xi::X)              p |= SQUARE;
    if (buttons & xi::Y)              p |= TRIANGLE;
    if (left_trigger  >= XINPUT_TRIGGER_THRESHOLD) p |= L2;
    if (right_trigger >= XINPUT_TRIGGER_THRESHOLD) p |= R2;
    // XInput: +Y is up
    if (stick_y >  XINPUT_STICK_DEADZONE) p |= UP;
    if (stick_y < -XINPUT_STICK_DEADZONE) p |= DOWN;
    if (stick_x >  XINPUT_STICK_DEADZONE) p |= RIGHT;
    if (stick_x < -XINPUT_STICK_DEADZONE) p |= LEFT;
    return p;
}

// DirectInput POV hat: hundredths of a degree clockwise from up; centered when LOWORD == 0xFFFF
[[nodiscard]] constexpr u16 map_pov_hat(u32 pov) noexcept {
    if ((pov & 0xFFFFu) == 0xFFFFu) return 0;
    u16 p = 0;
    if (pov < 9000u || pov > 27000u)  p |= UP;
    if (pov > 0u && pov < 18000u)     p |= RIGHT;
    if (pov > 9000u && pov < 27000u)  p |= DOWN;
    if (pov > 18000u)                 p |= LEFT;
    return p;
}

// DirectInput generic gamepad, using the DualShock 4 / DualSense button order that most
// PlayStation-style and generic pads report: 0 Square, 1 Cross, 2 Circle, 3 Triangle,
// 4 L1, 5 R1, 6 L2, 7 R2, 8 Share/Select, 9 Options/Start, 10 L3, 11 R3.
// Stick axes are expected in [-1000, 1000] with +Y pointing down (DirectInput convention).
[[nodiscard]] constexpr u16 map_dinput(u32 pov, s32 stick_x, s32 stick_y,
                                       const u8* buttons, std::size_t button_count) noexcept {
    constexpr u16 BUTTON_ORDER[12] = {SQUARE, CROSS, CIRCLE, TRIANGLE, L1, R1,
                                      L2, R2, SELECT, START, L3, R3};
    u16 p = map_pov_hat(pov);
    for (std::size_t i = 0; i < 12 && i < button_count; ++i) {
        if ((buttons[i] & 0x80u) != 0) p |= BUTTON_ORDER[i];
    }
    if (stick_y < -DINPUT_STICK_DEADZONE) p |= UP;
    if (stick_y >  DINPUT_STICK_DEADZONE) p |= DOWN;
    if (stick_x >  DINPUT_STICK_DEADZONE) p |= RIGHT;
    if (stick_x < -DINPUT_STICK_DEADZONE) p |= LEFT;
    return p;
}

// ---------------------------------------------------------------------------------------------
// Controller button remapping. map_xinput()/map_dinput() report buttons by POSITION (bottom face
// button = Cross, ...). A ButtonMap then says, for every PS1 button, which physical button
// presses it: map[i] = index into REMAP_BITS of the source button, or -1 for "unassigned".
// The D-pad and the left stick are never remapped.
// ---------------------------------------------------------------------------------------------
constexpr std::size_t REMAP_COUNT = 12;
constexpr u16 REMAP_BITS[REMAP_COUNT] = {CROSS, CIRCLE, SQUARE, TRIANGLE, L1, R1,
                                         L2, R2, START, SELECT, L3, R3};
// Settings-file names (ini keys are "pad_<name>", values are a name or "none")
constexpr const char* REMAP_NAMES[REMAP_COUNT] = {"cross", "circle", "square", "triangle", "l1", "r1",
                                                  "l2", "r2", "start", "select", "l3", "r3"};

using ButtonMap = std::array<s8, REMAP_COUNT>;

[[nodiscard]] constexpr ButtonMap default_button_map() noexcept {
    ButtonMap m{};
    for (std::size_t i = 0; i < REMAP_COUNT; ++i) m[i] = static_cast<s8>(i);
    return m;
}

[[nodiscard]] constexpr u16 apply_button_map(u16 positional, const ButtonMap& map) noexcept {
    u16 out = static_cast<u16>(positional & (UP | DOWN | LEFT | RIGHT));
    for (std::size_t i = 0; i < REMAP_COUNT; ++i) {
        const int src = map[i];
        if (src >= 0 && src < static_cast<int>(REMAP_COUNT) && (positional & REMAP_BITS[src]) != 0) {
            out = static_cast<u16>(out | REMAP_BITS[i]);
        }
    }
    return out;
}

// Name -> REMAP_BITS index; -1 for "none"; -2 when the name is unknown
[[nodiscard]] inline int remap_index_from_name(std::string_view name) noexcept {
    if (name == "none") return -1;
    for (std::size_t i = 0; i < REMAP_COUNT; ++i) {
        if (name == REMAP_NAMES[i]) return static_cast<int>(i);
    }
    return -2;
}

} // namespace vsport::pad
