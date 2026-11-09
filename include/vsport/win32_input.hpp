#pragma once

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
    [[nodiscard]] u16 poll();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vsport
