#pragma once

#include "vsport/pad_mapping.hpp"
#include "vsport/types.hpp"

#include <memory>
#include <string>

namespace vsport {

// Host gamepad input for vs_host: XInput (Xbox and XInput-compatible pads, up to 4) plus a
// DirectInput fallback for DualShock 4 / DualSense and other generic HID gamepads.
// Controllers can be plugged in or removed at any time.
class Win32GamepadInput {
public:
    Win32GamepadInput();
    ~Win32GamepadInput();
    Win32GamepadInput(const Win32GamepadInput&) = delete;
    Win32GamepadInput& operator=(const Win32GamepadInput&) = delete;

    // hwnd: the host window (DirectInput cooperative level). Returns false only if neither
    // backend could start; keyboard input keeps working either way.
    bool initialize(void* hwnd);
    void shutdown();

    // Polls every connected controller and returns the merged state as ACTIVE-HIGH PS1 bits
    // (see vsport/pad_mapping.hpp). Prints a line when a controller connects or disconnects.
    // Buttons are returned after this remap (default: Xbox/PlayStation layout by position)
    [[nodiscard]] u16 poll();
    void set_button_map(const pad::ButtonMap& map) noexcept { map_ = map; }

    // Vibration of the connected XInput controllers: PS1 small motor (on/off) and large motor
    // (0..255). Only sends to the controllers when the values change.
    void set_rumble(bool small_motor, u8 large_motor);

private:
    pad::ButtonMap map_ = pad::default_button_map();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vsport
