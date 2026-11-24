#include "vsport/free_camera.hpp"

#include "vsport/runtime.hpp"

#include <algorithm>
#include <cmath>

namespace vsport {

namespace {
// The camera struct lives in BATTLE/BATTLE.PRG, the field and battle engine loaded at 0x80068800
// (title, ending and other engines put other things there). Its code loads the struct address
// with "lui v0,0x800F / addiu v1,v0,0x19xx"; that pair is looked for in this window and the struct
// address is read from it: it is not always the same (0x800F19D0 in one save, 0x800F19C8 with the
// code 8 bytes earlier in another). Nothing is written unless it is there.
constexpr u32 SIG_FROM = 0x8006D800u, SIG_TO = 0x8006E400u;
constexpr u32 SIG_LUI = 0x3C02800Fu, SIG_ADDIU = 0x24430000u; // addiu v1,v0,imm (imm in the low half)
constexpr u32 CAM_LO = 0x800F1800u, CAM_HI = 0x800F1C00u;     // Where the struct may be

// Camera struct address the pair at a loads, or 0 when a holds no such pair
u32 camera_at(PsxRuntime& rt, u32 a) {
    if (rt.read32(a) != SIG_LUI) return 0;
    const u32 addiu = rt.read32(a + 4u);
    if ((addiu & 0xFFFF0000u) != SIG_ADDIU) return 0;
    const u32 cam = 0x800F0000u + static_cast<u32>(static_cast<s32>(static_cast<s16>(addiu & 0xFFFFu)));
    return cam >= CAM_LO && cam < CAM_HI ? cam : 0;
}

constexpr float YAW_PER_SECOND   = 2048.0f; // Half a turn per second at full tilt
constexpr float PITCH_PER_SECOND = 640.0f;
} // namespace

bool FreeCamera::supported(PsxRuntime& rt) {
    // Checked every time: the engine is swapped (title, ending). Where the pair was last found is
    // tried first; otherwise the window is searched again.
    if (sig_addr_ != 0) {
        const u32 cam = camera_at(rt, sig_addr_);
        if (cam != 0) { camera_ = cam; return true; }
    }
    for (u32 a = SIG_FROM; a < SIG_TO; a += 4u) {
        const u32 cam = camera_at(rt, a);
        if (cam != 0) {
            sig_addr_ = a;
            camera_ = cam;
            return true;
        }
    }
    return false;
}

float FreeCamera::shape(float raw, float dead_zone) noexcept {
    const float a = std::fabs(raw);
    if (a <= dead_zone) return 0.0f;
    // Linear past the dead zone: a half push turns at about half speed (squaring it made light
    // pushes, which is how the stick is mostly held, look like nothing happened)
    const float t = std::min(1.0f, (a - dead_zone) / (1.0f - dead_zone));
    return std::copysign(t, raw);
}

const char* FreeCamera::status_text(Status s) noexcept {
    switch (s) {
        case Status::Idle:         return "cubuk serbest";
        case Status::Moved:        return "kamera dondu";
        case Status::OtherProgram: return "oyun alani motoru (BATTLE.PRG) yuklu degil: acilis, baslik, son";
        case Status::NotField:     return "oyun alani kamerasi kullanilmiyor (menu, ara sahne, baslik)";
        case Status::AtLimit:      return "egim sinirda";
    }
    return "?";
}

bool FreeCamera::apply(PsxRuntime& rt, float stick_x, float stick_y, u32 fps) {
    status_ = Status::Idle;
    if (stick_x == 0.0f && stick_y == 0.0f) return false;
    if (!supported(rt)) {
        status_ = Status::OtherProgram;
        return false;
    }

    // Field camera only: active flag set, a valid pointer next to it and a sane distance
    const u32 active = rt.read32(camera_ + 0x1Cu);
    const u32 ptr    = rt.read32(camera_ + 0x2Cu);
    const s32 dist   = static_cast<s32>(rt.read32(camera_ + 0x08u));
    last_active_ = active; last_ptr_ = ptr; last_dist_ = dist;
    if (active != 1u || ptr < 0x80000000u || ptr >= 0x80200000u || dist <= 0 || dist > 0x4000) {
        status_ = Status::NotField;
        return false;
    }

    const float per_frame = 1.0f / static_cast<float>(fps == 0 ? 60u : fps);
    bool moved = false;
    if (stick_x != 0.0f) {
        // Turn from where the game is heading (an L1/R1 turn in progress ends right here)
        const s32 yaw = static_cast<s32>(rt.read32(camera_ + 0x14u)) +
                        static_cast<s32>(std::lround(stick_x * YAW_PER_SECOND * per_frame));
        rt.write32(camera_ + 0x00u, static_cast<u32>(yaw));
        rt.write32(camera_ + 0x14u, static_cast<u32>(yaw));
        moved = true;
    }
    if (stick_y != 0.0f) {
        const s32 pitch = static_cast<s32>(rt.read32(camera_ + 0x04u));
        // Stick up = look from higher up (more negative pitch)
        const s32 step = static_cast<s32>(std::lround(stick_y * PITCH_PER_SECOND * per_frame));
        // Limits stop the stick only; a room the game itself shows steeper or flatter is kept
        const s32 want = step < 0 ? std::max(pitch + step, std::min(pitch, PITCH_MIN))
                                  : std::min(pitch + step, std::max(pitch, PITCH_MAX));
        if (want != pitch) {
            rt.write32(camera_ + 0x04u, static_cast<u32>(want));
            moved = true;
        }
    }
    status_ = moved ? Status::Moved : Status::AtLimit;
    return moved;
}

} // namespace vsport
