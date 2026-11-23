#pragma once

#include "vsport/types.hpp"

namespace vsport {

class PsxRuntime;

// Right-stick camera for Vagrant Story: turns the field camera freely instead of the game's
// 45 degree L1/R1 steps, and tilts it up and down.
//
// The game keeps its field camera in a struct of BATTLE/BATTLE.PRG, the field and battle engine
// (the same address in the European SLES-02754 and the US SLUS-01040 release; found by comparing
// RAM before and after L1/R1 presses):
//   +0x00 s32 yaw (4096 = full turn)     +0x14 s32 yaw the L1/R1 turn is heading for
//   +0x04 s32 pitch (about -384)         +0x1C s32 1 while the field camera is in use
//   +0x08 s32 distance                   +0x2C     pointer, 0 outside the field (title, ...)
// Nothing is written unless that engine is loaded (its camera code is found) and the field
// camera is active, and only while the stick is pushed.
class FreeCamera {
public:
    static constexpr u32 CAMERA = 0x800F19D0u;

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

    // True while the field / battle engine (BATTLE.PRG) that owns the camera struct is loaded
    [[nodiscard]] bool supported(PsxRuntime& rt);

    // Raw stick value -> [-1, 1] with a dead zone and a soft start (quadratic)
    [[nodiscard]] static float shape(float raw, float dead_zone) noexcept;

private:
    u32 sig_addr_ = 0; // Where the engine's camera code was last found
    Status status_ = Status::Idle;
};

} // namespace vsport
