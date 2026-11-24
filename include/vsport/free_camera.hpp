#pragma once

#include "vsport/types.hpp"

namespace vsport {

class PsxRuntime;

// Right-stick camera for Vagrant Story: turns the field camera freely instead of the game's
// 45 degree L1/R1 steps, and tilts it up and down.
//
// The game keeps its field camera in a struct of BATTLE/BATTLE.PRG, the field and battle engine
// (found by comparing RAM before and after L1/R1 presses; seen at 0x800F19D0 and at 0x800F19C8,
// so the address is read from the engine's own code):
//   +0x00 s32 yaw (4096 = full turn)     +0x14 s32 yaw the L1/R1 turn is heading for
//   +0x04 s32 pitch (about -384)         +0x1C s32 1 while the field camera is in use
//   +0x08 s32 distance                   +0x2C     pointer, 0 outside the field (title, ...)
// Nothing is written unless that engine is loaded (its camera code is found) and the field
// camera is active, and only while the stick is pushed.
class FreeCamera {
public:
    // Range of the tilt (the game's own value is -384; less negative = flatter view)
    static constexpr s32 PITCH_MIN = -704;
    static constexpr s32 PITCH_MAX = -160;

    // stick_x / stick_y in [-1, 1] after the dead zone, +x right, +y down. fps: 50 or 60.
    // Returns true when it moved the camera.
    bool apply(PsxRuntime& rt, float stick_x, float stick_y, u32 fps);

    // Why the last apply() did or did not move the camera (for the game log)
    enum class Status : u8 { Idle, Moved, OtherProgram, NotField, AtLimit };
    [[nodiscard]] Status status() const noexcept { return status_; }
    [[nodiscard]] static const char* status_text(Status s) noexcept;

    // True while the field / battle engine (BATTLE.PRG) that owns the camera struct is loaded;
    // camera() is then the struct's address
    [[nodiscard]] bool supported(PsxRuntime& rt);
    [[nodiscard]] u32 camera() const noexcept { return camera_; }
    // Struct fields the last apply() checked (for the game log when the camera is not used)
    [[nodiscard]] u32 last_active() const noexcept { return last_active_; }
    [[nodiscard]] u32 last_ptr() const noexcept { return last_ptr_; }
    [[nodiscard]] s32 last_dist() const noexcept { return last_dist_; }

    // Raw stick value -> [-1, 1] with a dead zone, linear past it
    [[nodiscard]] static float shape(float raw, float dead_zone) noexcept;

private:
    u32 sig_addr_ = 0; // Where the engine's camera code was last found
    u32 camera_ = 0;   // Camera struct address read from that code
    u32 last_active_ = 0, last_ptr_ = 0;
    s32 last_dist_ = 0;
    Status status_ = Status::Idle;
};

} // namespace vsport
