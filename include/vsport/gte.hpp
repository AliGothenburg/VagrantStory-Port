#pragma once

#include "vsport/pgxp.hpp"
#include "vsport/types.hpp"

#include <array>

namespace vsport {

class StateIO;

// PlayStation COP2 - Geometry Transform Engine (GTE)
// Implements 32 Data Registers, 32 Control Registers, and fixed-point 3D commands
// with optional native-port widescreen horizontal projection scaling.
class GteEngine {
public:
    GteEngine() { reset(); }

    void reset() noexcept;

    // Host enhancement: Widescreen X-axis scale (1.0f = native 4:3, 0.75f = 16:9 hor+ field expansion)
    void set_widescreen_scale(float x_scale) noexcept { widescreen_x_scale_ = x_scale; }
    // Exact projected positions are recorded here (upscaled picture precision); null = off
    void set_pgxp(PgxpCache* cache) noexcept { pgxp_ = cache; }
    [[nodiscard]] float widescreen_scale() const noexcept { return widescreen_x_scale_; }

    // MFC2 / MTC2 (Data registers 0..31)
    [[nodiscard]] u32 read_data(u32 reg_idx) const noexcept;
    void write_data(u32 reg_idx, u32 value) noexcept;

    // CFC2 / CTC2 (Control registers 0..31)
    [[nodiscard]] u32 read_ctrl(u32 reg_idx) const noexcept;
    void write_ctrl(u32 reg_idx, u32 value) noexcept;

    // Execute raw 32-bit COP2 instruction word (opcode 0x12 with bit 25 = 1)
    void execute(u32 cmd_word) noexcept;

    // Save state: writes or reads the whole state (see state_io.hpp)
    void serialize(StateIO& io);

private:
    void cmd_rtps(int v_idx, bool sf, bool lm, bool last_vertex) noexcept;
    void cmd_nclip() noexcept;
    void cmd_op(bool sf, bool lm) noexcept;
    void cmd_sqr(bool sf, bool lm) noexcept;
    void cmd_avsz3() noexcept;
    void cmd_avsz4() noexcept;
    void cmd_mvmva(u8 mx, u8 v, u8 cv, bool sf, bool lm) noexcept;
    void cmd_ncs(int v_idx, bool sf, bool lm, bool depth_cue, bool use_ir_color) noexcept;
    void cmd_gpf(bool sf, bool lm, bool add_mac) noexcept;

    s16 clamp_ir(int idx, s64 val, bool lm) noexcept;
    s16 clamp_sxy(int idx, s64 val) noexcept;
    u16 clamp_sz3(s64 val) noexcept;
    u8  clamp_rgb(int idx, s64 val) noexcept;
    s64 check_mac(int idx, s64 val) noexcept;
    void finalize_flag() noexcept;

    static u32 count_leading_bits(u32 val) noexcept;

    // Raw storage backed by structured accessors
    std::array<u32, 32> data_{};
    std::array<u32, 32> ctrl_{};
    float widescreen_x_scale_ = 1.0f;
    PgxpCache* pgxp_ = nullptr;
};

} // namespace vsport
