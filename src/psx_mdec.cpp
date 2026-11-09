#include "vsport/psx_mdec.hpp"

#include <algorithm>
#include <cstring>

namespace vsport {

namespace {

// Inverse zig-zag table: maps zig-zag stream index k (0..63) -> 2D block index (row * 8 + col)
constexpr std::array<u8, 64> ZAGZIG = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

constexpr std::array<u8, 64> DEFAULT_IQ_ZIGZAG = {
     2, 16, 16, 19, 16, 19, 22, 22,
    22, 22, 22, 22, 26, 24, 26, 27,
    27, 27, 26, 26, 26, 26, 27, 27,
    27, 29, 29, 29, 34, 34, 34, 29,
    29, 29, 27, 27, 29, 29, 32, 32,
    34, 34, 37, 38, 37, 35, 35, 34,
    35, 38, 38, 40, 40, 40, 48, 48,
    46, 46, 56, 56, 58, 69, 69, 83
};

constexpr std::array<s16, 64> DEFAULT_SCALE_TABLE = {
     23170,  23170,  23170,  23170,  23170,  23170,  23170,  23170,
     32138,  27245,  18204,   6392,  -6393, -18205, -27246, -32139,
     30273,  12539, -12540, -30274, -30274, -12540,  12539,  30273,
     27245,  -6393, -32139, -18205,  18204,  32138,   6392, -27246,
     23170, -23171, -23171,  23170,  23170, -23171, -23171,  23170,
     18204, -32139,   6392,  27245, -27246,  -6393,  32138, -18205,
     12539, -30274,  30273, -12540, -12540,  30273, -30274,  12539,
      6392, -18205,  27245, -32139,  32138, -27246,  18204,  -6393
};

constexpr s32 sign_extend_10(u32 v) noexcept {
    return static_cast<s32>(v << 22) >> 22;
}

} // namespace

PsxMdec::PsxMdec() {
    init_default_tables();
    reset();
}

void PsxMdec::init_default_tables() noexcept {
    iq_y_        = DEFAULT_IQ_ZIGZAG;
    iq_uv_       = DEFAULT_IQ_ZIGZAG;
    scale_table_ = DEFAULT_SCALE_TABLE;
}

void PsxMdec::reset() {
    init_default_tables();
    active_cmd_            = 0;
    depth_                 = 0;
    signed_out_            = false;
    set_bit15_             = false;
    iq_color_              = false;
    dma0_in_enable_        = false;
    dma1_out_enable_       = false;
    remaining_param_words_ = 0;
    status_low16_          = 0;
    in_fifo_.clear();
    out_fifo_.clear();
    out_read_pos_        = 0;
    macroblocks_decoded_ = 0;
    commands_executed_   = 0;
}

void PsxMdec::write_ctrl(u32 word) {
    if ((word & 0x80000000u) != 0) {
        // Warm MDEC reset via MDEC1 bit 31: aborts active command & clears FIFOs, preserving IQ/IDCT tables
        active_cmd_            = 0;
        depth_                 = 0;
        signed_out_            = false;
        set_bit15_             = false;
        dma0_in_enable_        = false;
        dma1_out_enable_       = false;
        remaining_param_words_ = 0;
        status_low16_          = 0;
        in_fifo_.clear();
        out_fifo_.clear();
        out_read_pos_ = 0;
    }
    dma0_in_enable_ = (word & 0x40000000u) != 0;
    dma1_out_enable_ = (word & 0x20000000u) != 0;
}

u32 PsxMdec::read_status() const noexcept {
    u32 st = 0;
    const bool out_empty = !has_output();
    const bool busy      = (remaining_param_words_ > 0) || !out_empty;

    if (out_empty) st |= (1u << 31);
    if (busy)      st |= (1u << 29);
    if (dma0_in_enable_ && remaining_param_words_ > 0) st |= (1u << 28);
    if (dma1_out_enable_ && !out_empty)                st |= (1u << 27);

    st |= (static_cast<u32>(depth_ & 3u) << 25);
    if (signed_out_) st |= (1u << 24);
    if (set_bit15_)  st |= (1u << 23);
    st |= (4u << 16); // Current block indicator (4 = Y3 / idle marker -> 0x80040000 after reset)

    const u16 rem_field = (remaining_param_words_ > 0)
                              ? static_cast<u16>((remaining_param_words_ - 1u) & 0xFFFFu)
                              : status_low16_;
    st |= rem_field;
    return st;
}

u32 PsxMdec::read_data() {
    if (out_read_pos_ < out_fifo_.size()) {
        const u32 w = out_fifo_[out_read_pos_++];
        if (out_read_pos_ >= out_fifo_.size()) {
            out_fifo_.clear();
            out_read_pos_ = 0;
            status_low16_ = 0xFFFFu;
        }
        return w;
    }
    return 0;
}

void PsxMdec::write_cmd_or_param(u32 word) {
    if (remaining_param_words_ > 0) {
        in_fifo_.push_back(word);
        --remaining_param_words_;
        if (remaining_param_words_ == 0) {
            finish_command();
        }
        return;
    }

    // Ignore trailing 0xFE00FE00 DMA0 32-word block alignment padding
    if (word == 0xFE00FE00u) {
        return;
    }

    active_cmd_ = static_cast<u8>((word >> 29) & 7u);
    depth_      = static_cast<u8>((word >> 27) & 3u);
    signed_out_ = (word & (1u << 26)) != 0;
    set_bit15_  = (word & (1u << 25)) != 0;
    in_fifo_.clear();

    switch (active_cmd_) {
        case 1: { // Decode Macroblock(s)
            const u32 count = word & 0xFFFFu;
            status_low16_   = static_cast<u16>(count);
            out_fifo_.clear();
            out_read_pos_ = 0;
            if (count == 0) {
                ++commands_executed_;
                status_low16_ = 0xFFFFu;
            } else {
                remaining_param_words_ = count;
                in_fifo_.reserve(count);
            }
            break;
        }
        case 2: { // Set Quantization Table(s)
            iq_color_              = (word & 1u) != 0;
            remaining_param_words_ = iq_color_ ? 32u : 16u;
            in_fifo_.reserve(remaining_param_words_);
            break;
        }
        case 3: { // Set IDCT Scale Table
            remaining_param_words_ = 32u;
            in_fifo_.reserve(32u);
            break;
        }
        default: { // No-op
            status_low16_ = static_cast<u16>(word & 0xFFFFu);
            ++commands_executed_;
            break;
        }
    }
}

void PsxMdec::finish_command() {
    ++commands_executed_;
    switch (active_cmd_) {
        case 1:
            decode_macroblock_stream();
            break;
        case 2: {
            if (in_fifo_.size() >= 16u) {
                for (std::size_t i = 0; i < 16u; ++i) {
                    const u32 w = in_fifo_[i];
                    iq_y_[i * 4u + 0] = static_cast<u8>(w & 0xFFu);
                    iq_y_[i * 4u + 1] = static_cast<u8>((w >> 8) & 0xFFu);
                    iq_y_[i * 4u + 2] = static_cast<u8>((w >> 16) & 0xFFu);
                    iq_y_[i * 4u + 3] = static_cast<u8>((w >> 24) & 0xFFu);
                }
            }
            if (iq_color_ && in_fifo_.size() >= 32u) {
                for (std::size_t i = 0; i < 16u; ++i) {
                    const u32 w = in_fifo_[16u + i];
                    iq_uv_[i * 4u + 0] = static_cast<u8>(w & 0xFFu);
                    iq_uv_[i * 4u + 1] = static_cast<u8>((w >> 8) & 0xFFu);
                    iq_uv_[i * 4u + 2] = static_cast<u8>((w >> 16) & 0xFFu);
                    iq_uv_[i * 4u + 3] = static_cast<u8>((w >> 24) & 0xFFu);
                }
            }
            status_low16_ = 0xFFFFu;
            break;
        }
        case 3: {
            if (in_fifo_.size() >= 32u) {
                for (std::size_t i = 0; i < 32u; ++i) {
                    const u32 w = in_fifo_[i];
                    scale_table_[i * 2u + 0] = static_cast<s16>(w & 0xFFFFu);
                    scale_table_[i * 2u + 1] = static_cast<s16>((w >> 16) & 0xFFFFu);
                }
            }
            status_low16_ = 0xFFFFu;
            break;
        }
        default:
            break;
    }
    in_fifo_.clear();
    active_cmd_ = 0;
}

void PsxMdec::idct_8x8(const std::array<s32, 64>& coeff, std::array<s32, 64>& out_spatial) const noexcept {
    // Pass 1: 1D horizontal IDCT for each row v = 0..7
    std::array<s64, 64> tmp{};
    for (u32 v = 0; v < 8u; ++v) {
        for (u32 x = 0; x < 8u; ++x) {
            s64 sum = 0;
            for (u32 u = 0; u < 8u; ++u) {
                sum += static_cast<s64>(coeff[v * 8u + u]) * static_cast<s64>(scale_table_[u * 8u + x]);
            }
            tmp[v * 8u + x] = sum;
        }
    }

    // Pass 2: 1D vertical IDCT for each column x = 0..7 (combined 2^32 fixed-point normalization)
    for (u32 y = 0; y < 8u; ++y) {
        for (u32 x = 0; x < 8u; ++x) {
            s64 sum = 0;
            for (u32 v = 0; v < 8u; ++v) {
                sum += tmp[v * 8u + x] * static_cast<s64>(scale_table_[v * 8u + y]);
            }
            const s32 val = static_cast<s32>((sum + (1LL << 31)) >> 32);
            out_spatial[y * 8u + x] = std::clamp<s32>(val, -128, 127);
        }
    }
}

bool PsxMdec::decode_rl_block(
    const std::vector<u16>& rle,
    std::size_t& pos,
    const std::array<u8, 64>& iq,
    std::array<s32, 64>& out_spatial) const noexcept {
    // Skip leading 0xFE00 padding halfwords before block start
    while (pos < rle.size() && rle[pos] == 0xFE00u) {
        ++pos;
    }
    if (pos >= rle.size()) {
        return false;
    }

    std::array<s32, 64> coeff{};
    const u16 dc_hw   = rle[pos++];
    const u32 q_scale = (dc_hw >> 10) & 0x3Fu;
    const s32 dc_raw  = sign_extend_10(dc_hw & 0x3FFu);

    if (q_scale == 0) {
        coeff[0] = std::clamp<s32>(dc_raw * 2, -1024, 1023);
        for (u32 k = 1; k < 64u && pos < rle.size(); ++k) {
            const u16 hw = rle[pos++];
            if (hw == 0xFE00u) break;
            coeff[k] = std::clamp<s32>(sign_extend_10(hw & 0x3FFu) * 2, -1024, 1023);
        }
    } else {
        coeff[ZAGZIG[0]] = std::clamp<s32>(dc_raw * static_cast<s32>(iq[0]), -1024, 1023);
        u32 k = 0;
        while (pos < rle.size()) {
            const u16 ac_hw = rle[pos++];
            if (ac_hw == 0xFE00u) {
                break; // End-Of-Block
            }
            const u32 skip = (ac_hw >> 10) & 0x3Fu;
            k += skip + 1u;
            if (k < 64u) {
                const s32 level = sign_extend_10(ac_hw & 0x3FFu);
                const s32 deq   = (level * static_cast<s32>(iq[k]) * static_cast<s32>(q_scale) + 4) >> 3;
                coeff[ZAGZIG[k]] = std::clamp<s32>(deq, -1024, 1023);
            }
        }
    }

    idct_8x8(coeff, out_spatial);
    return true;
}

void PsxMdec::decode_macroblock_stream() {
    std::vector<u16> rle;
    rle.reserve(in_fifo_.size() * 2u);
    for (u32 w : in_fifo_) {
        rle.push_back(static_cast<u16>(w & 0xFFFFu));
        rle.push_back(static_cast<u16>((w >> 16) & 0xFFFFu));
    }

    std::size_t pos = 0;
    if (depth_ == 0 || depth_ == 1) {
        // Monochrome 8x8 blocks (4-bit or 8-bit)
        std::array<s32, 64> y_blk{};
        while (decode_rl_block(rle, pos, iq_y_, y_blk)) {
            ++macroblocks_decoded_;
            if (depth_ == 1) {
                for (u32 i = 0; i < 64u; i += 4u) {
                    auto conv = [&](s32 v) -> u8 {
                        return signed_out_ ? static_cast<u8>(v) : static_cast<u8>(v + 128);
                    };
                    const u32 w = static_cast<u32>(conv(y_blk[i + 0])) |
                                  (static_cast<u32>(conv(y_blk[i + 1])) << 8) |
                                  (static_cast<u32>(conv(y_blk[i + 2])) << 16) |
                                  (static_cast<u32>(conv(y_blk[i + 3])) << 24);
                    out_fifo_.push_back(w);
                }
            } else {
                for (u32 i = 0; i < 64u; i += 8u) {
                    u32 w = 0;
                    for (u32 n = 0; n < 8u; ++n) {
                        const u8 b = signed_out_ ? static_cast<u8>(y_blk[i + n]) : static_cast<u8>(y_blk[i + n] + 128);
                        w |= (static_cast<u32>((b >> 4) & 0x0Fu) << (n * 4u));
                    }
                    out_fifo_.push_back(w);
                }
            }
        }
        return;
    }

    // Color 16x16 Macroblocks: Cr, Cb, Y1 (top-left), Y2 (top-right), Y3 (bottom-left), Y4 (bottom-right)
    std::array<s32, 64> cr_blk{};
    std::array<s32, 64> cb_blk{};
    std::array<std::array<s32, 64>, 4> y_blks{};

    while (true) {
        if (!decode_rl_block(rle, pos, iq_uv_, cr_blk)) break;
        if (!decode_rl_block(rle, pos, iq_uv_, cb_blk)) break;
        if (!decode_rl_block(rle, pos, iq_y_, y_blks[0])) break;
        if (!decode_rl_block(rle, pos, iq_y_, y_blks[1])) break;
        if (!decode_rl_block(rle, pos, iq_y_, y_blks[2])) break;
        if (!decode_rl_block(rle, pos, iq_y_, y_blks[3])) break;

        ++macroblocks_decoded_;

        std::array<u8, 16 * 16 * 3> rgb_bytes{};
        for (u32 py = 0; py < 16u; ++py) {
            for (u32 px = 0; px < 16u; ++px) {
                const u32 blk_idx = ((py >> 3) * 2u) + (px >> 3);
                const s32 y_val   = y_blks[blk_idx][((py & 7u) * 8u) + (px & 7u)];
                const s32 cr      = cr_blk[((py >> 1) * 8u) + (px >> 1)];
                const s32 cb      = cb_blk[((py >> 1) * 8u) + (px >> 1)];

                const s32 r_off = (359 * cr + 128) >> 8;
                const s32 g_off = (-88 * cb - 183 * cr + 128) >> 8;
                const s32 b_off = (454 * cb + 128) >> 8;

                const s32 rs = std::clamp<s32>(y_val + r_off, -128, 127);
                const s32 gs = std::clamp<s32>(y_val + g_off, -128, 127);
                const s32 bs = std::clamp<s32>(y_val + b_off, -128, 127);

                const std::size_t dst = (py * 16u + px) * 3u;
                rgb_bytes[dst + 0] = signed_out_ ? static_cast<u8>(rs) : static_cast<u8>(rs + 128);
                rgb_bytes[dst + 1] = signed_out_ ? static_cast<u8>(gs) : static_cast<u8>(gs + 128);
                rgb_bytes[dst + 2] = signed_out_ ? static_cast<u8>(bs) : static_cast<u8>(bs + 128);
            }
        }

        if (depth_ == 2) {
            // 24-bit RGB888: 768 bytes -> 192 little-endian 32-bit words
            for (std::size_t b = 0; b < rgb_bytes.size(); b += 4u) {
                const u32 w = static_cast<u32>(rgb_bytes[b + 0]) |
                              (static_cast<u32>(rgb_bytes[b + 1]) << 8) |
                              (static_cast<u32>(rgb_bytes[b + 2]) << 16) |
                              (static_cast<u32>(rgb_bytes[b + 3]) << 24);
                out_fifo_.push_back(w);
            }
        } else {
            // 15-bit RGB555: 256 pixels -> 128 little-endian 32-bit words (2 pixels per word)
            const u16 stp = set_bit15_ ? 0x8000u : 0x0000u;
            for (u32 p = 0; p < 256u; p += 2u) {
                auto pack555 = [&](std::size_t off) -> u16 {
                    const u16 r5 = static_cast<u16>(std::min<u32>(31u, (static_cast<u32>(rgb_bytes[off + 0]) + 4u) >> 3));
                    const u16 g5 = static_cast<u16>(std::min<u32>(31u, (static_cast<u32>(rgb_bytes[off + 1]) + 4u) >> 3));
                    const u16 b5 = static_cast<u16>(std::min<u32>(31u, (static_cast<u32>(rgb_bytes[off + 2]) + 4u) >> 3));
                    return static_cast<u16>(stp | r5 | (g5 << 5) | (b5 << 10));
                };
                const u16 p0 = pack555(p * 3u);
                const u16 p1 = pack555((p + 1u) * 3u);
                out_fifo_.push_back(static_cast<u32>(p0) | (static_cast<u32>(p1) << 16));
            }
        }
    }
}

} // namespace vsport
