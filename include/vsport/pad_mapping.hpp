#pragma once

#include "vsport/types.hpp"

#include <cstddef>

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

} // namespace vsport::pad
