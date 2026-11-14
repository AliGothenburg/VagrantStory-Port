#include "vsport/gte.hpp"
#include "vsport/mips_decoder.hpp"

#include <algorithm>
#include <cmath>

namespace vsport {

namespace {

constexpr s16 lo16s(u32 v) noexcept { return static_cast<s16>(v & 0xFFFFu); }
constexpr s16 hi16s(u32 v) noexcept { return static_cast<s16>((v >> 16) & 0xFFFFu); }

constexpr u32 pack16(s16 lo, s16 hi) noexcept {
    return static_cast<u32>(static_cast<u16>(lo)) |
           (static_cast<u32>(static_cast<u16>(hi)) << 16);
}

} // namespace

void GteEngine::reset() noexcept {
    data_.fill(0);
    ctrl_.fill(0);
    data_[31] = 1; // LZCR of 0 is 32, reset state safe default
    widescreen_x_scale_ = 1.0f;
}

u32 GteEngine::count_leading_bits(u32 val) noexcept {
    if (val & 0x80000000u) {
        val = ~val;
    }
    if (val == 0) {
        return 32;
    }
    u32 count = 0;
    for (u32 mask = 0x80000000u; (val & mask) == 0; mask >>= 1) {
        ++count;
    }
    return count;
}

u32 GteEngine::read_data(u32 idx) const noexcept {
    idx &= 31u;
    switch (idx) {
        case 1:
        case 3:
        case 5:
        case 8:
        case 9:
        case 10:
        case 11:
            // Sign-extended 16-bit registers (VZ0..2, IR0..3)
            return static_cast<u32>(static_cast<s32>(lo16s(data_[idx])));
        case 7:
        case 16:
        case 17:
        case 18:
        case 19:
            // Zero-extended 16-bit registers (OTZ, SZ0..3)
            return data_[idx] & 0xFFFFu;
        case 15:
            // SXYP mirrors SXY2
            return data_[14];
        case 28:
        case 29: {
            // IRGB / ORGB: packs IR1..IR3 / 0x80 saturated to 0..31 into 15-bit RGB
            const auto sat5 = [](s16 ir) -> u32 {
                const s32 v = ir / 0x80;
                return static_cast<u32>(std::clamp(v, 0, 31));
            };
            const u32 r = sat5(lo16s(data_[9]));
            const u32 g = sat5(lo16s(data_[10]));
            const u32 b = sat5(lo16s(data_[11]));
            return r | (g << 5) | (b << 10);
        }
        default:
            return data_[idx];
    }
}

void GteEngine::write_data(u32 idx, u32 val) noexcept {
    idx &= 31u;
    switch (idx) {
        case 1:
        case 3:
        case 5:
        case 8:
        case 9:
        case 10:
        case 11:
            data_[idx] = static_cast<u32>(static_cast<s32>(lo16s(val)));
            break;
        case 7:
        case 16:
        case 17:
        case 18:
        case 19:
            data_[idx] = val & 0xFFFFu;
            break;
        case 15:
            // Writing SXYP pushes the SXY0 <- SXY1 <- SXY2 <- val FIFO!
            data_[12] = data_[13];
            data_[13] = data_[14];
            data_[14] = val;
            data_[15] = val;
            break;
        case 28: {
            // IRGB expands 5-bit components * 0x80 into IR1..IR3
            data_[28] = val & 0x7FFFu;
            data_[9]  = static_cast<u32>((val & 0x1Fu) * 0x80u);
            data_[10] = static_cast<u32>(((val >> 5) & 0x1Fu) * 0x80u);
            data_[11] = static_cast<u32>(((val >> 10) & 0x1Fu) * 0x80u);
            break;
        }
        case 29:
        case 31:
            // ORGB and LZCR are read-only
            break;
        case 30:
            data_[30] = val;
            data_[31] = count_leading_bits(val);
            break;
        default:
            data_[idx] = val;
            break;
    }
}

u32 GteEngine::read_ctrl(u32 idx) const noexcept {
    idx &= 31u;
    switch (idx) {
        case 4:
        case 12:
        case 20:
        case 26:
        case 27:
        case 29:
        case 30:
            // Sign-extended 16-bit control registers (R33, L33, C33, H, DQA, ZSF3, ZSF4)
            return static_cast<u32>(static_cast<s32>(lo16s(ctrl_[idx])));
        default:
            return ctrl_[idx];
    }
}

void GteEngine::write_ctrl(u32 idx, u32 val) noexcept {
    idx &= 31u;
    if (idx == 31) {
        ctrl_[31] = val & 0x7FFFF000u;
        finalize_flag();
    } else {
        ctrl_[idx] = val;
    }
}

void GteEngine::finalize_flag() noexcept {
    // Bit 31 is logical OR of error summary bits (30..23 and 18..13)
    constexpr u32 ERR_MASK = 0x7F87E000u;
    if ((ctrl_[31] & ERR_MASK) != 0) {
        ctrl_[31] |= 0x80000000u;
    } else {
        ctrl_[31] &= ~0x80000000u;
    }
}

s64 GteEngine::check_mac(int idx, s64 val) noexcept {
    // idx 0 = 32-bit MAC0, idx 1..3 = 44-bit MAC1..3
    if (idx == 0) {
        if (val > 0x7FFFFFFFLL) ctrl_[31] |= (1u << 16);
        if (val < -0x80000000LL) ctrl_[31] |= (1u << 15);
    } else {
        const s64 max44 = (1LL << 43) - 1;
        const s64 min44 = -(1LL << 43);
        if (val > max44) ctrl_[31] |= (1u << (31 - idx)); // bits 30, 29, 28
        if (val < min44) ctrl_[31] |= (1u << (28 - idx)); // bits 27, 26, 25
        // Sign-extend 44-bit accumulator
        val = (val << 20) >> 20;
    }
    return val;
}

s16 GteEngine::clamp_ir(int idx, s64 val, bool lm) noexcept {
    if (idx == 0) {
        if (val < 0) {
            ctrl_[31] |= (1u << 12);
            return 0;
        }
        if (val > 0x1000) {
            ctrl_[31] |= (1u << 12);
            return 0x1000;
        }
        return static_cast<s16>(val);
    }

    const s64 lo = lm ? 0LL : -0x8000LL;
    const s64 hi = 0x7FFFLL;
    if (val < lo || val > hi) {
        ctrl_[31] |= (1u << (25 - idx)); // bits 24, 23, 22
    }
    return static_cast<s16>(std::clamp(val, lo, hi));
}

s16 GteEngine::clamp_sxy(int idx, s64 val) noexcept {
    if (val < -1024LL || val > 1023LL) {
        ctrl_[31] |= (1u << (14 - (idx - 1))); // idx 1->bit 14 (SX2), idx 2->bit 13 (SY2)
    }
    return static_cast<s16>(std::clamp(val, -1024LL, 1023LL));
}

u16 GteEngine::clamp_sz3(s64 val) noexcept {
    if (val < 0LL || val > 0xFFFFLL) {
        ctrl_[31] |= (1u << 18);
    }
    return static_cast<u16>(std::clamp(val, 0LL, 0xFFFFLL));
}

u8 GteEngine::clamp_rgb(int idx, s64 val) noexcept {
    if (val < 0LL || val > 255LL) {
        ctrl_[31] |= (1u << (21 - (idx - 1))); // bits 21, 20, 19
    }
    return static_cast<u8>(std::clamp(val, 0LL, 255LL));
}

void GteEngine::cmd_rtps(int v_idx, bool sf, bool lm, bool last_vertex) noexcept {
    const s64 vx = lo16s(data_[v_idx * 2]);
    const s64 vy = hi16s(data_[v_idx * 2]);
    const s64 vz = lo16s(data_[v_idx * 2 + 1]);

    // Rotation Matrix (ctrl 0..4) & Translation Vector (ctrl 5..7)
    const s64 r11 = lo16s(ctrl_[0]), r12 = hi16s(ctrl_[0]);
    const s64 r13 = lo16s(ctrl_[1]), r21 = hi16s(ctrl_[1]);
    const s64 r22 = lo16s(ctrl_[2]), r23 = hi16s(ctrl_[2]);
    const s64 r31 = lo16s(ctrl_[3]), r32 = hi16s(ctrl_[3]);
    const s64 r33 = lo16s(ctrl_[4]);

    const s64 trx = static_cast<s32>(ctrl_[5]) * 4096LL;
    const s64 try_ = static_cast<s32>(ctrl_[6]) * 4096LL;
    const s64 trz = static_cast<s32>(ctrl_[7]) * 4096LL;

    const int shift = sf ? 12 : 0;

    const s64 m1 = check_mac(1, trx + r11 * vx + r12 * vy + r13 * vz) >> shift;
    const s64 m2 = check_mac(2, try_ + r21 * vx + r22 * vy + r23 * vz) >> shift;
    const s64 m3_full = check_mac(3, trz + r31 * vx + r32 * vy + r33 * vz);
    const s64 m3 = m3_full >> shift;

    data_[25] = static_cast<u32>(m1);
    data_[26] = static_cast<u32>(m2);
    data_[27] = static_cast<u32>(m3);

    const s16 ir1 = clamp_ir(1, m1, lm);
    const s16 ir2 = clamp_ir(2, m2, lm);
    // Note: On real PS1 GTE, RTPS checks IR3 saturation on (m3_full >> 12) before writing clamped m3!
    (void)clamp_ir(3, m3_full >> 12, false);
    const s16 ir3 = static_cast<s16>(std::clamp(m3, lm ? 0LL : -0x8000LL, 0x7FFFLL));

    data_[9]  = static_cast<u32>(static_cast<s32>(ir1));
    data_[10] = static_cast<u32>(static_cast<s32>(ir2));
    data_[11] = static_cast<u32>(static_cast<s32>(ir3));

    // Push SZ FIFO: SZ0 <- SZ1 <- SZ2 <- SZ3
    data_[16] = data_[17];
    data_[17] = data_[18];
    data_[18] = data_[19];
    const u16 sz3 = clamp_sz3(m3_full >> 12);
    data_[19] = sz3;

    // Perspective divide factor: min(0x1FFFF, (H * 0x20000 / SZ3 + 1) / 2)
    const u16 h = static_cast<u16>(ctrl_[26] & 0xFFFFu);
    u32 div_factor = 0x1FFFFu;
    if (sz3 > (h / 2u)) {
        const u64 num = (static_cast<u64>(h) << 17) / sz3;
        div_factor = static_cast<u32>(std::min<u64>(0x1FFFFull, (num + 1ull) >> 1));
    } else {
        ctrl_[31] |= (1u << 17); // Divide overflow flag
    }

    const s64 ofx = static_cast<s32>(ctrl_[24]);
    const s64 ofy = static_cast<s32>(ctrl_[25]);

    s64 proj_x = static_cast<s64>(div_factor) * ir1;
    if (widescreen_x_scale_ != 1.0f) {
        proj_x = static_cast<s64>(std::llround(static_cast<double>(proj_x) * widescreen_x_scale_));
    }
    const s64 proj_y = static_cast<s64>(div_factor) * ir2;

    const s64 sx_raw = check_mac(0, ofx + proj_x) >> 16;
    const s64 sy_raw = check_mac(0, ofy + proj_y) >> 16;

    const s16 sx2 = clamp_sxy(1, sx_raw);
    const s16 sy2 = clamp_sxy(2, sy_raw);

    // Push SXY FIFO
    data_[12] = data_[13];
    data_[13] = data_[14];
    data_[14] = pack16(sx2, sy2);
    data_[15] = data_[14];
    // Exact position for the upscaled picture (only when the point was not clamped)
    if (pgxp_ && sx2 == sx_raw && sy2 == sy_raw) {
        pgxp_->store(data_[14], static_cast<float>(static_cast<double>(ofx + proj_x) / 65536.0),
                     static_cast<float>(static_cast<double>(ofy + proj_y) / 65536.0), static_cast<float>(sz3));
    }

    if (last_vertex) {
        const s64 dqa = lo16s(ctrl_[27]);
        const s64 dqb = static_cast<s32>(ctrl_[28]);
        const s64 mac0 = check_mac(0, dqb + dqa * static_cast<s64>(div_factor));
        data_[24] = static_cast<u32>(mac0);
        data_[8]  = static_cast<u32>(static_cast<s32>(clamp_ir(0, mac0 >> 12, true)));
    }
}

void GteEngine::cmd_nclip() noexcept {
    const s64 sx0 = lo16s(data_[12]), sy0 = hi16s(data_[12]);
    const s64 sx1 = lo16s(data_[13]), sy1 = hi16s(data_[13]);
    const s64 sx2 = lo16s(data_[14]), sy2 = hi16s(data_[14]);

    const s64 det = sx0 * (sy1 - sy2) + sx1 * (sy2 - sy0) + sx2 * (sy0 - sy1);
    const s64 mac0 = check_mac(0, det);
    data_[24] = static_cast<u32>(mac0);
}

void GteEngine::cmd_op(bool sf, bool lm) noexcept {
    const s64 d1 = lo16s(ctrl_[0]); // R11
    const s64 d2 = lo16s(ctrl_[2]); // R22
    const s64 d3 = lo16s(ctrl_[4]); // R33

    const s64 ir1 = lo16s(data_[9]);
    const s64 ir2 = lo16s(data_[10]);
    const s64 ir3 = lo16s(data_[11]);

    const int shift = sf ? 12 : 0;
    const s64 m1 = check_mac(1, ir3 * d2 - ir2 * d3) >> shift;
    const s64 m2 = check_mac(2, ir1 * d3 - ir3 * d1) >> shift;
    const s64 m3 = check_mac(3, ir2 * d1 - ir1 * d2) >> shift;

    data_[25] = static_cast<u32>(m1);
    data_[26] = static_cast<u32>(m2);
    data_[27] = static_cast<u32>(m3);

    data_[9]  = static_cast<u32>(static_cast<s32>(clamp_ir(1, m1, lm)));
    data_[10] = static_cast<u32>(static_cast<s32>(clamp_ir(2, m2, lm)));
    data_[11] = static_cast<u32>(static_cast<s32>(clamp_ir(3, m3, lm)));
}

void GteEngine::cmd_sqr(bool sf, bool lm) noexcept {
    const int shift = sf ? 12 : 0;
    for (int i = 1; i <= 3; ++i) {
        const s64 ir = lo16s(data_[8 + i]);
        const s64 m  = check_mac(i, ir * ir) >> shift;
        data_[24 + i] = static_cast<u32>(m);
        data_[8 + i]  = static_cast<u32>(static_cast<s32>(clamp_ir(i, m, lm)));
    }
}

void GteEngine::cmd_avsz3() noexcept {
    const s64 zsf3 = lo16s(ctrl_[29]);
    const s64 sum  = static_cast<s64>(data_[17] & 0xFFFFu) +
                     static_cast<s64>(data_[18] & 0xFFFFu) +
                     static_cast<s64>(data_[19] & 0xFFFFu);
    const s64 mac0 = check_mac(0, zsf3 * sum);
    data_[24] = static_cast<u32>(mac0);
    data_[7]  = clamp_sz3(mac0 >> 12);
}

void GteEngine::cmd_avsz4() noexcept {
    const s64 zsf4 = lo16s(ctrl_[30]);
    const s64 sum  = static_cast<s64>(data_[16] & 0xFFFFu) +
                     static_cast<s64>(data_[17] & 0xFFFFu) +
                     static_cast<s64>(data_[18] & 0xFFFFu) +
                     static_cast<s64>(data_[19] & 0xFFFFu);
    const s64 mac0 = check_mac(0, zsf4 * sum);
    data_[24] = static_cast<u32>(mac0);
    data_[7]  = clamp_sz3(mac0 >> 12);
}

void GteEngine::cmd_mvmva(u8 mx, u8 v, u8 cv, bool sf, bool lm) noexcept {
    std::array<std::array<s64, 3>, 3> mat{};
    if (mx < 3) {
        const u32 base = static_cast<u32>(mx) * 8u;
        mat[0] = {lo16s(ctrl_[base + 0]), hi16s(ctrl_[base + 0]), lo16s(ctrl_[base + 1])};
        mat[1] = {hi16s(ctrl_[base + 1]), lo16s(ctrl_[base + 2]), hi16s(ctrl_[base + 2])};
        mat[2] = {lo16s(ctrl_[base + 3]), hi16s(ctrl_[base + 3]), lo16s(ctrl_[base + 4])};
    }

    std::array<s64, 3> vec{};
    if (v < 3) {
        vec[0] = lo16s(data_[v * 2]);
        vec[1] = hi16s(data_[v * 2]);
        vec[2] = lo16s(data_[v * 2 + 1]);
    } else {
        vec[0] = lo16s(data_[9]);
        vec[1] = lo16s(data_[10]);
        vec[2] = lo16s(data_[11]);
    }

    std::array<s64, 3> tr{0, 0, 0};
    if (cv == 0) {
        tr = {static_cast<s32>(ctrl_[5]) * 4096LL,
              static_cast<s32>(ctrl_[6]) * 4096LL,
              static_cast<s32>(ctrl_[7]) * 4096LL};
    } else if (cv == 1) {
        tr = {static_cast<s32>(ctrl_[13]) * 4096LL,
              static_cast<s32>(ctrl_[14]) * 4096LL,
              static_cast<s32>(ctrl_[15]) * 4096LL};
    } else if (cv == 2) {
        tr = {static_cast<s32>(ctrl_[21]) * 4096LL,
              static_cast<s32>(ctrl_[22]) * 4096LL,
              static_cast<s32>(ctrl_[23]) * 4096LL};
    }

    const int shift = sf ? 12 : 0;
    for (int i = 0; i < 3; ++i) {
        const s64 sum = check_mac(i + 1, tr[i] + mat[i][0] * vec[0] + mat[i][1] * vec[1] + mat[i][2] * vec[2]) >> shift;
        data_[25 + i] = static_cast<u32>(sum);
        data_[9 + i]  = static_cast<u32>(static_cast<s32>(clamp_ir(i + 1, sum, lm)));
    }
}

void GteEngine::cmd_ncs(int v_idx, bool sf, bool lm, bool depth_cue, bool use_rgb_mod) noexcept {
    // 1. LLM * V[v_idx]
    cmd_mvmva(1, static_cast<u8>(v_idx), 3, sf, lm);
    // 2. BK + LCM * IR
    cmd_mvmva(2, 3, 1, sf, lm);

    if (use_rgb_mod) {
        const s64 r = static_cast<s64>(data_[6] & 0xFFu) << 4;
        const s64 g = static_cast<s64>((data_[6] >> 8) & 0xFFu) << 4;
        const s64 b = static_cast<s64>((data_[6] >> 16) & 0xFFu) << 4;
        const std::array<s64, 3> rgb{r, g, b};
        const int shift = sf ? 12 : 0;

        for (int i = 0; i < 3; ++i) {
            const s64 ir = lo16s(data_[9 + i]);
            s64 m = check_mac(i + 1, rgb[i] * ir);
            if (depth_cue) {
                const s64 fc  = static_cast<s32>(ctrl_[21 + i]) * 4096LL;
                const s64 ir0 = lo16s(data_[8]);
                const s16 diff = clamp_ir(i + 1, check_mac(i + 1, fc - m) >> shift, false);
                m = check_mac(i + 1, m + static_cast<s64>(diff) * ir0);
            }
            m >>= shift;
            data_[25 + i] = static_cast<u32>(m);
            data_[9 + i]  = static_cast<u32>(static_cast<s32>(clamp_ir(i + 1, m, lm)));
        }
    }

    const u8 cr = clamp_rgb(1, static_cast<s32>(data_[25]) >> 4);
    const u8 cg = clamp_rgb(2, static_cast<s32>(data_[26]) >> 4);
    const u8 cb = clamp_rgb(3, static_cast<s32>(data_[27]) >> 4);
    const u8 code = static_cast<u8>((data_[6] >> 24) & 0xFFu);

    // Push RGB FIFO: RGB0 <- RGB1 <- RGB2 <- new RGBC
    data_[20] = data_[21];
    data_[21] = data_[22];
    data_[22] = static_cast<u32>(cr) | (static_cast<u32>(cg) << 8) |
                (static_cast<u32>(cb) << 16) | (static_cast<u32>(code) << 24);
}

void GteEngine::cmd_gpf(bool sf, bool lm, bool add_mac) noexcept {
    const s64 ir0 = lo16s(data_[8]);
    const int shift = sf ? 12 : 0;

    for (int i = 0; i < 3; ++i) {
        const s64 ir = lo16s(data_[9 + i]);
        const s64 base = add_mac ? (static_cast<s64>(static_cast<s32>(data_[25 + i])) << shift) : 0LL;
        const s64 m = check_mac(i + 1, base + ir * ir0) >> shift;
        data_[25 + i] = static_cast<u32>(m);
        data_[9 + i]  = static_cast<u32>(static_cast<s32>(clamp_ir(i + 1, m, lm)));
    }

    const u8 cr = clamp_rgb(1, static_cast<s32>(data_[25]) >> 4);
    const u8 cg = clamp_rgb(2, static_cast<s32>(data_[26]) >> 4);
    const u8 cb = clamp_rgb(3, static_cast<s32>(data_[27]) >> 4);
    const u8 code = static_cast<u8>((data_[6] >> 24) & 0xFFu);

    data_[20] = data_[21];
    data_[21] = data_[22];
    data_[22] = static_cast<u32>(cr) | (static_cast<u32>(cg) << 8) |
                (static_cast<u32>(cb) << 16) | (static_cast<u32>(code) << 24);
}

void GteEngine::execute(u32 cmd_word) noexcept {
    ctrl_[31] = 0; // Reset FLAG per command

    const u8 fn   = static_cast<u8>(cmd_word & 0x3Fu);
    const bool sf = ((cmd_word >> 19) & 1u) != 0;
    const u8 mx   = static_cast<u8>((cmd_word >> 17) & 0x03u);
    const u8 v    = static_cast<u8>((cmd_word >> 15) & 0x03u);
    const u8 cv   = static_cast<u8>((cmd_word >> 13) & 0x03u);
    const bool lm = ((cmd_word >> 10) & 1u) != 0;

    switch (fn) {
        case gte_fn::RTPS:
            cmd_rtps(0, sf, lm, true);
            break;
        case gte_fn::RTPT:
            cmd_rtps(0, sf, lm, false);
            cmd_rtps(1, sf, lm, false);
            cmd_rtps(2, sf, lm, true);
            break;
        case gte_fn::NCLIP:
            cmd_nclip();
            break;
        case gte_fn::OP:
            cmd_op(sf, lm);
            break;
        case gte_fn::SQR:
            cmd_sqr(sf, lm);
            break;
        case gte_fn::AVSZ3:
            cmd_avsz3();
            break;
        case gte_fn::AVSZ4:
            cmd_avsz4();
            break;
        case gte_fn::MVMVA:
            cmd_mvmva(mx, v, cv, sf, lm);
            break;
        case gte_fn::NCS:
            cmd_ncs(0, sf, lm, false, false);
            break;
        case gte_fn::NCT:
            cmd_ncs(0, sf, lm, false, false);
            cmd_ncs(1, sf, lm, false, false);
            cmd_ncs(2, sf, lm, false, false);
            break;
        case gte_fn::NCCS:
            cmd_ncs(0, sf, lm, false, true);
            break;
        case gte_fn::NCCT:
            cmd_ncs(0, sf, lm, false, true);
            cmd_ncs(1, sf, lm, false, true);
            cmd_ncs(2, sf, lm, false, true);
            break;
        case gte_fn::NCDS:
            cmd_ncs(0, sf, lm, true, true);
            break;
        case gte_fn::NCDT:
            cmd_ncs(0, sf, lm, true, true);
            cmd_ncs(1, sf, lm, true, true);
            cmd_ncs(2, sf, lm, true, true);
            break;
        case gte_fn::GPF:
            cmd_gpf(sf, lm, false);
            break;
        case gte_fn::GPL:
            cmd_gpf(sf, lm, true);
            break;
        default:
            break;
    }

    finalize_flag();
}

} // namespace vsport
