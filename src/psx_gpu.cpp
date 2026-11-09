#include "vsport/psx_gpu.hpp"

#include <algorithm>
#include <array>
#include <fstream>

namespace vsport {

namespace {

constexpr s32 sign_extend_11(u32 v) noexcept {
    v &= 0x7FFu;
    return (v & 0x400u) ? static_cast<s32>(v | ~0x7FFu) : static_cast<s32>(v);
}

u16 blend_psx_abr(u16 bg555, u8 fr, u8 fg, u8 fb, u32 abr) noexcept {
    const s32 br = static_cast<s32>((bg555 & 0x1Fu) << 3);
    const s32 bg = static_cast<s32>(((bg555 >> 5) & 0x1Fu) << 3);
    const s32 bb = static_cast<s32>(((bg555 >> 10) & 0x1Fu) << 3);
    s32 r = fr, g = fg, b = fb;
    switch (abr & 3u) {
        case 0: r = (br + fr) >> 1; g = (bg + fg) >> 1; b = (bb + fb) >> 1; break;
        case 1: r = std::min<s32>(255, br + fr); g = std::min<s32>(255, bg + fg); b = std::min<s32>(255, bb + fb); break;
        case 2: r = std::max<s32>(0, br - fr);   g = std::max<s32>(0, bg - fg);   b = std::max<s32>(0, bb - fb);   break;
        case 3: r = std::min<s32>(255, br + (fr >> 2)); g = std::min<s32>(255, bg + (fg >> 2)); b = std::min<s32>(255, bb + (fb >> 2)); break;
    }
    return static_cast<u16>(((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) | (((b >> 3) & 0x1F) << 10));
}

u32 gp0_command_length(u8 op) noexcept {
    if (op >= 0x20 && op <= 0x3F) {
        // Polygons
        const bool quad     = (op & 0x08u) != 0;
        const bool shaded   = (op & 0x10u) != 0;
        const bool textured = (op & 0x04u) != 0;
        const u32 verts     = quad ? 4u : 3u;
        u32 words = 1u + verts; // cmd+col0 + N vertex XY words
        if (shaded)   words += (verts - 1u);
        if (textured) words += verts;
        return words;
    }
    if (op >= 0x40 && op <= 0x5F) {
        // Lines
        const bool shaded = (op & 0x10u) != 0;
        return shaded ? 4u : 3u;
    }
    if (op >= 0x60 && op <= 0x7F) {
        // Rectangles / Sprites
        const bool textured = (op & 0x04u) != 0;
        const u8 size_code  = (op >> 3) & 0x03u;
        u32 words = 2u; // cmd+col, XY
        if (textured) ++words; // UV+CLUT
        if (size_code == 0) ++words; // Variable WH
        return words;
    }
    switch (op) {
        case 0x02: return 3; // Fill VRAM rectangle
        case 0x80: return 4; // VRAM -> VRAM copy
        case 0xA0: return 3; // CPU -> VRAM transfer header
        case 0xC0: return 3; // VRAM -> CPU transfer header
        default:   return 1; // NOP, Env commands (E1..E6), Clear cache (01), IRQ (1F)
    }
}

} // namespace

PsxGpu::PsxGpu() : vram_(VRAM_WIDTH * VRAM_HEIGHT, 0) {
    reset();
}

void PsxGpu::reset() {
    std::fill(vram_.begin(), vram_.end(), static_cast<u16>(0));
    fifo_.clear();
    expected_words_   = 0;
    mode_             = Gp0Mode::Command;
    in_polyline_      = false;
    gpustat_          = 0x1C802000u; // Ready to receive cmd, VRAM->CPU ready, DMA block ready
    texpage_          = 0;
    clip_x1_          = 0;
    clip_y1_          = 0;
    clip_x2_          = 1023;
    clip_y2_          = 511;
    draw_off_x_       = 0;
    draw_off_y_       = 0;
    disp_vram_x_      = 0;
    disp_vram_y_      = 0;
    disp_width_       = 320;
    disp_height_      = 240;
    primitives_drawn_ = 0;
}

u32 PsxGpu::read_gpustat() const noexcept {
    // Toggle bit 31 interlaced even/odd line so VSync / GPU poll loops never hang!
    static u32 toggle = 0;
    toggle ^= 0x80000000u;
    return (gpustat_ & 0x7FFFFFFFu) | 0x1C000000u | toggle;
}

u32 PsxGpu::read_gpuread() {
    if (mode_ != Gp0Mode::VramToCpuRead) {
        return gpuread_latch_;
    }

    auto next_halfword = [&]() -> u16 {
        const u16 px = read_vram_pixel(xfer_x_ + xfer_cx_, xfer_y_ + xfer_cy_);
        if (++xfer_cx_ >= xfer_w_) {
            xfer_cx_ = 0;
            if (++xfer_cy_ >= xfer_h_) {
                mode_ = Gp0Mode::Command;
            }
        }
        return px;
    };

    const u16 lo = next_halfword();
    const u16 hi = (mode_ == Gp0Mode::VramToCpuRead) ? next_halfword() : 0u;
    gpuread_latch_ = static_cast<u32>(lo) | (static_cast<u32>(hi) << 16);
    return gpuread_latch_;
}

void PsxGpu::write_gp1(u32 word) {
    const u8 cmd = static_cast<u8>((word >> 24) & 0x3Fu);
    const u32 param = word & 0x00FFFFFFu;

    switch (cmd) {
        case 0x00: // Reset GPU
            reset();
            break;
        case 0x01: // Reset Command Buffer
            fifo_.clear();
            expected_words_ = 0;
            mode_           = Gp0Mode::Command;
            in_polyline_    = false;
            break;
        case 0x02: // Ack GPU IRQ
            gpustat_ &= ~(1u << 24);
            break;
        case 0x03: // Display Enable (0 = on, 1 = off)
            if (param & 1u) gpustat_ |= (1u << 23);
            else            gpustat_ &= ~(1u << 23);
            break;
        case 0x04: // DMA Direction
            gpustat_ = (gpustat_ & ~(3u << 29)) | ((param & 3u) << 29);
            break;
        case 0x05: // Start of Display Area in VRAM
            disp_vram_x_ = param & 0x3FEu;
            disp_vram_y_ = (param >> 10) & 0x1FFu;
            break;
        case 0x07: { // Vertical Display Range on Screen (Y1..Y2)
            const u32 y1 = param & 0x3FFu;
            const u32 y2 = (param >> 10) & 0x3FFu;
            if (y2 > y1) {
                const u32 lines   = std::clamp<u32>(y2 - y1, 16u, 288u);
                const bool is_480 = ((gpustat_ & (1u << 19)) != 0) && ((gpustat_ & (1u << 22)) != 0);
                disp_height_      = is_480 ? (lines * 2u) : lines;
            }
            break;
        }
        case 0x08: { // Display Mode
            constexpr std::array<u32, 4> HRES = {256, 320, 512, 640};
            const bool old_is_480 = ((gpustat_ & (1u << 19)) != 0) && ((gpustat_ & (1u << 22)) != 0);
            const bool new_is_480 = ((param & 0x04u) != 0) && ((param & 0x20u) != 0);
            disp_width_ = (param & 0x40u) ? 368u : HRES[param & 3u];
            if (new_is_480 && !old_is_480) {
                disp_height_ *= 2u;
            } else if (!new_is_480 && old_is_480) {
                disp_height_ = std::max<u32>(16u, disp_height_ / 2u);
            }
            gpustat_ &= ~(0x7Fu << 16);
            gpustat_ |= ((param & 0x3Fu) << 17) | (((param >> 6) & 1u) << 16);
            break;
        }
        case 0x10: { // Read internal GPU info into GPUREAD
            switch (param & 7u) {
                case 2: gpuread_latch_ = 0; break;
                case 3: gpuread_latch_ = static_cast<u32>((clip_x1_ & 0x3FF) | ((clip_y1_ & 0x3FF) << 10)); break;
                case 4: gpuread_latch_ = static_cast<u32>((clip_x2_ & 0x3FF) | ((clip_y2_ & 0x3FF) << 10)); break;
                case 5: gpuread_latch_ = static_cast<u32>((draw_off_x_ & 0x7FF) | ((draw_off_y_ & 0x7FF) << 11)); break;
                case 7: gpuread_latch_ = 2; break; // GPU version 2
                default: break;
            }
            break;
        }
        default:
            break;
    }
}

void PsxGpu::write_gp0(u32 word) {
    if (mode_ == Gp0Mode::CpuToVramCopy) {
        for (int half = 0; half < 2 && mode_ == Gp0Mode::CpuToVramCopy; ++half) {
            const u16 px = static_cast<u16>((word >> (half * 16)) & 0xFFFFu);
            write_vram_pixel(xfer_x_ + xfer_cx_, xfer_y_ + xfer_cy_, px);
            if (++xfer_cx_ >= xfer_w_) {
                xfer_cx_ = 0;
                if (++xfer_cy_ >= xfer_h_) {
                    mode_ = Gp0Mode::Command;
                }
            }
        }
        return;
    }

    if (in_polyline_) {
        if ((word & 0xF000F000u) == 0x50005000u) {
            in_polyline_ = false;
            fifo_.clear();
        }
        return;
    }

    if (fifo_.empty()) {
        const u8 op = static_cast<u8>((word >> 24) & 0xFFu);
        expected_words_ = gp0_command_length(op);
    }

    fifo_.push_back(word);
    if (fifo_.size() >= expected_words_) {
        dispatch_gp0_packet();
        fifo_.clear();
        expected_words_ = 0;
    }
}

u16 PsxGpu::sample_texture(u8 u, u8 v, u16 clut_word, u16 texpage_word) const noexcept {
    const u32 tp_x  = (texpage_word & 0x0Fu) * 64u;
    const u32 tp_y  = ((texpage_word >> 4) & 0x01u) * 256u;
    const u32 depth = (texpage_word >> 7) & 0x03u;

    const u32 clut_x = (clut_word & 0x3Fu) * 16u;
    const u32 clut_y = (clut_word >> 6) & 0x1FFu;

    if (depth == 0) {
        // 4bpp indexed
        const u16 texel_word = read_vram_pixel(tp_x + (u / 4u), tp_y + v);
        const u8 idx = static_cast<u8>((texel_word >> ((u & 3u) * 4u)) & 0x0Fu);
        return read_vram_pixel(clut_x + idx, clut_y);
    }
    if (depth == 1) {
        // 8bpp indexed
        const u16 texel_word = read_vram_pixel(tp_x + (u / 2u), tp_y + v);
        const u8 idx = static_cast<u8>((texel_word >> ((u & 1u) * 8u)) & 0xFFu);
        return read_vram_pixel(clut_x + idx, clut_y);
    }
    // 15bpp direct RGB555
    return read_vram_pixel(tp_x + u, tp_y + v);
}

void PsxGpu::rasterize_triangle(const Vertex& v0, const Vertex& v1, const Vertex& v2,
                                bool shaded, bool textured, u16 clut_word, u16 texpage_word, bool semi_trans) {
    s32 min_x = std::min({v0.x, v1.x, v2.x});
    s32 max_x = std::max({v0.x, v1.x, v2.x});
    s32 min_y = std::min({v0.y, v1.y, v2.y});
    s32 max_y = std::max({v0.y, v1.y, v2.y});

    // Reject oversized PS1 hardware polygons (> 1023 wide or > 511 tall)
    if ((max_x - min_x) >= 1024 || (max_y - min_y) >= 512) {
        return;
    }

    min_x = std::max(min_x, clip_x1_);
    max_x = std::min(max_x, clip_x2_);
    min_y = std::max(min_y, clip_y1_);
    max_y = std::min(max_y, clip_y2_);

    if (min_x > max_x || min_y > max_y) {
        return;
    }

    const auto edge = [](s32 ax, s32 ay, s32 bx, s32 by, s32 px, s32 py) -> s64 {
        return static_cast<s64>(px - ax) * (by - ay) - static_cast<s64>(py - ay) * (bx - ax);
    };

    s64 area = edge(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);
    if (area == 0) {
        return;
    }

    const s64 sign = (area < 0) ? -1 : 1;
    area *= sign;

    for (s32 py = min_y; py <= max_y; ++py) {
        for (s32 px = min_x; px <= max_x; ++px) {
            const s64 w0 = edge(v1.x, v1.y, v2.x, v2.y, px, py) * sign;
            const s64 w1 = edge(v2.x, v2.y, v0.x, v0.y, px, py) * sign;
            const s64 w2 = edge(v0.x, v0.y, v1.x, v1.y, px, py) * sign;

            if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                u8 r = v0.r, g = v0.g, b = v0.b;
                if (shaded) {
                    r = static_cast<u8>((w0 * v0.r + w1 * v1.r + w2 * v2.r) / area);
                    g = static_cast<u8>((w0 * v0.g + w1 * v1.g + w2 * v2.g) / area);
                    b = static_cast<u8>((w0 * v0.b + w1 * v1.b + w2 * v2.b) / area);
                }

                if (textured) {
                    const u8 u = static_cast<u8>((w0 * v0.u + w1 * v1.u + w2 * v2.u) / area);
                    const u8 v = static_cast<u8>((w0 * v0.v + w1 * v1.v + w2 * v2.v) / area);
                    const u16 texel = sample_texture(u, v, clut_word, texpage_word);
                    if (texel == 0) continue; // PS1 transparent texel 0x0000

                    const u32 tr = (texel & 0x1Fu) << 3;
                    const u32 tg = ((texel >> 5) & 0x1Fu) << 3;
                    const u32 tb = ((texel >> 10) & 0x1Fu) << 3;

                    // Modulate with vertex color (0x80 = neutral brightness on PS1!)
                    const u8 mr = static_cast<u8>(std::min<u32>(255u, (tr * r) >> 7));
                    const u8 mg = static_cast<u8>(std::min<u32>(255u, (tg * g) >> 7));
                    const u8 mb = static_cast<u8>(std::min<u32>(255u, (tb * b) >> 7));
                    const u16 out_px = (semi_trans && (texel & 0x8000u) != 0)
                        ? blend_psx_abr(read_vram_pixel(static_cast<u32>(px), static_cast<u32>(py)), mr, mg, mb, (texpage_word >> 5) & 3u)
                        : rgb888_to_555(mr, mg, mb);
                    write_vram_pixel(static_cast<u32>(px), static_cast<u32>(py), out_px);
                } else {
                    const u16 out_px = semi_trans
                        ? blend_psx_abr(read_vram_pixel(static_cast<u32>(px), static_cast<u32>(py)), r, g, b, (texpage_word >> 5) & 3u)
                        : rgb888_to_555(r, g, b);
                    write_vram_pixel(static_cast<u32>(px), static_cast<u32>(py), out_px);
                }
            }
        }
    }
}

void PsxGpu::dispatch_gp0_packet() {
    const u32 w0 = fifo_[0];
    const u8 op  = static_cast<u8>((w0 >> 24) & 0xFFu);

    if (op >= 0x20 && op <= 0x3F) {
        // Polygons (3 or 4 vertices)
        const bool quad      = (op & 0x08u) != 0;
        const bool shaded    = (op & 0x10u) != 0;
        const bool textured  = (op & 0x04u) != 0;
        const bool raw_tex   = (op & 0x01u) != 0;
        const bool semi      = (op & 0x02u) != 0;
        const int num_verts  = quad ? 4 : 3;

        std::array<Vertex, 4> verts{};
        u16 clut_word    = 0;
        u16 texpage_word = texpage_;

        std::size_t idx = 0;
        for (int i = 0; i < num_verts; ++i) {
            if (i == 0 || shaded) {
                const u32 col = fifo_[idx++];
                verts[i].r = (textured && raw_tex) ? 0x80u : static_cast<u8>(col & 0xFFu);
                verts[i].g = (textured && raw_tex) ? 0x80u : static_cast<u8>((col >> 8) & 0xFFu);
                verts[i].b = (textured && raw_tex) ? 0x80u : static_cast<u8>((col >> 16) & 0xFFu);
            } else {
                verts[i].r = verts[0].r;
                verts[i].g = verts[0].g;
                verts[i].b = verts[0].b;
            }

            const u32 xy = fifo_[idx++];
            verts[i].x = sign_extend_11(xy & 0xFFFFu) + draw_off_x_;
            verts[i].y = sign_extend_11(xy >> 16) + draw_off_y_;

            if (textured) {
                const u32 uv = fifo_[idx++];
                verts[i].u = static_cast<u8>(uv & 0xFFu);
                verts[i].v = static_cast<u8>((uv >> 8) & 0xFFu);
                if (i == 0) clut_word = static_cast<u16>(uv >> 16);
                if (i == 1) {
                    texpage_word = static_cast<u16>(uv >> 16);
                    texpage_     = texpage_word;
                }
            }
        }

        rasterize_triangle(verts[0], verts[1], verts[2], shaded, textured, clut_word, texpage_word, semi);
        if (quad) {
            rasterize_triangle(verts[1], verts[2], verts[3], shaded, textured, clut_word, texpage_word, semi);
        }
        ++primitives_drawn_;
        return;
    }

    if (op >= 0x40 && op <= 0x5F) {
        if ((op & 0x08u) != 0) {
            in_polyline_ = true;
        }
        ++primitives_drawn_;
        return;
    }

    if (op >= 0x60 && op <= 0x7F) {
        // Rectangles / Sprites
        const bool textured = (op & 0x04u) != 0;
        const bool semi     = (op & 0x02u) != 0;
        const bool raw_tex  = (op & 0x01u) != 0;
        const u8 size_code  = (op >> 3) & 0x03u;

        const u8 cr = (textured && raw_tex) ? 0x80u : static_cast<u8>(w0 & 0xFFu);
        const u8 cg = (textured && raw_tex) ? 0x80u : static_cast<u8>((w0 >> 8) & 0xFFu);
        const u8 cb = (textured && raw_tex) ? 0x80u : static_cast<u8>((w0 >> 16) & 0xFFu);

        std::size_t idx = 1;
        const u32 xy = fifo_[idx++];
        const s32 rx = sign_extend_11(xy & 0xFFFFu) + draw_off_x_;
        const s32 ry = sign_extend_11(xy >> 16) + draw_off_y_;

        u8 base_u = 0, base_v = 0;
        u16 clut_word = 0;
        if (textured) {
            const u32 uv = fifo_[idx++];
            base_u    = static_cast<u8>(uv & 0xFFu);
            base_v    = static_cast<u8>((uv >> 8) & 0xFFu);
            clut_word = static_cast<u16>(uv >> 16);
        }

        s32 rw = 0, rh = 0;
        switch (size_code) {
            case 0: {
                const u32 wh = fifo_[idx++];
                rw = static_cast<s32>(wh & 0x3FFu);
                rh = static_cast<s32>((wh >> 16) & 0x1FFu);
                break;
            }
            case 1: rw = 1;  rh = 1;  break;
            case 2: rw = 8;  rh = 8;  break;
            case 3: rw = 16; rh = 16; break;
        }

        for (s32 dy = 0; dy < rh; ++dy) {
            const s32 py = ry + dy;
            if (py < clip_y1_ || py > clip_y2_) continue;
            for (s32 dx = 0; dx < rw; ++dx) {
                const s32 px = rx + dx;
                if (px < clip_x1_ || px > clip_x2_) continue;

                if (textured) {
                    const u16 texel = sample_texture(static_cast<u8>(base_u + dx),
                                                     static_cast<u8>(base_v + dy),
                                                     clut_word, texpage_);
                    if (texel == 0) continue;
                    const u32 tr = (texel & 0x1Fu) << 3;
                    const u32 tg = ((texel >> 5) & 0x1Fu) << 3;
                    const u32 tb = ((texel >> 10) & 0x1Fu) << 3;
                    const u8 mr = static_cast<u8>(std::min<u32>(255u, (tr * cr) >> 7));
                    const u8 mg = static_cast<u8>(std::min<u32>(255u, (tg * cg) >> 7));
                    const u8 mb = static_cast<u8>(std::min<u32>(255u, (tb * cb) >> 7));
                    const u16 out_px = (semi && (texel & 0x8000u) != 0)
                        ? blend_psx_abr(read_vram_pixel(static_cast<u32>(px), static_cast<u32>(py)), mr, mg, mb, (texpage_ >> 5) & 3u)
                        : rgb888_to_555(mr, mg, mb);
                    write_vram_pixel(static_cast<u32>(px), static_cast<u32>(py), out_px);
                } else {
                    const u16 out_px = semi
                        ? blend_psx_abr(read_vram_pixel(static_cast<u32>(px), static_cast<u32>(py)), cr, cg, cb, (texpage_ >> 5) & 3u)
                        : rgb888_to_555(cr, cg, cb);
                    write_vram_pixel(static_cast<u32>(px), static_cast<u32>(py), out_px);
                }
            }
        }
        ++primitives_drawn_;
        return;
    }

    switch (op) {
        case 0x02: { // Fast Fill VRAM
            const u16 fill_col = rgb888_to_555(static_cast<u8>(w0 & 0xFFu),
                                               static_cast<u8>((w0 >> 8) & 0xFFu),
                                               static_cast<u8>((w0 >> 16) & 0xFFu));
            const u32 fx = fifo_[1] & 0x3F0u;
            const u32 fy = (fifo_[1] >> 16) & 0x1FFu;
            const u32 fw = ((fifo_[2] & 0x3FFu) + 0x0Fu) & ~0x0Fu;
            const u32 fh = (fifo_[2] >> 16) & 0x1FFu;
            for (u32 dy = 0; dy < fh; ++dy) {
                for (u32 dx = 0; dx < fw; ++dx) {
                    write_vram_pixel(fx + dx, fy + dy, fill_col);
                }
            }
            ++primitives_drawn_;
            break;
        }
        case 0x80: { // VRAM -> VRAM Copy
            const u32 sx = fifo_[1] & 0x3FFu, sy = (fifo_[1] >> 16) & 0x1FFu;
            const u32 dx = fifo_[2] & 0x3FFu, dy = (fifo_[2] >> 16) & 0x1FFu;
            u32 cw = fifo_[3] & 0x3FFu, ch = (fifo_[3] >> 16) & 0x1FFu;
            if (cw == 0) cw = 0x400u;
            if (ch == 0) ch = 0x200u;
            std::vector<u16> tmp(cw * ch);
            for (u32 y = 0; y < ch; ++y) {
                for (u32 x = 0; x < cw; ++x) {
                    tmp[y * cw + x] = read_vram_pixel(sx + x, sy + y);
                }
            }
            for (u32 y = 0; y < ch; ++y) {
                for (u32 x = 0; x < cw; ++x) {
                    write_vram_pixel(dx + x, dy + y, tmp[y * cw + x]);
                }
            }
            break;
        }
        case 0xA0: { // CPU -> VRAM Transfer
            xfer_x_  = fifo_[1] & 0x3FFu;
            xfer_y_  = (fifo_[1] >> 16) & 0x1FFu;
            xfer_w_  = fifo_[2] & 0x3FFu;
            xfer_h_  = (fifo_[2] >> 16) & 0x1FFu;
            if (xfer_w_ == 0) xfer_w_ = 0x400u;
            if (xfer_h_ == 0) xfer_h_ = 0x200u;
            xfer_cx_ = 0;
            xfer_cy_ = 0;
            mode_    = Gp0Mode::CpuToVramCopy;
            break;
        }
        case 0xC0: { // VRAM -> CPU Read
            xfer_x_  = fifo_[1] & 0x3FFu;
            xfer_y_  = (fifo_[1] >> 16) & 0x1FFu;
            xfer_w_  = fifo_[2] & 0x3FFu;
            xfer_h_  = (fifo_[2] >> 16) & 0x1FFu;
            if (xfer_w_ == 0) xfer_w_ = 0x400u;
            if (xfer_h_ == 0) xfer_h_ = 0x200u;
            xfer_cx_ = 0;
            xfer_cy_ = 0;
            mode_    = Gp0Mode::VramToCpuRead;
            break;
        }
        case 0xE1: { // Draw Mode Setting (Texpage)
            texpage_ = static_cast<u16>(w0 & 0x7FFu);
            gpustat_ = (gpustat_ & ~0x7FFu) | (w0 & 0x7FFu);
            break;
        }
        case 0xE3: { // Drawing Area Top-Left
            clip_x1_ = static_cast<s32>(w0 & 0x3FFu);
            clip_y1_ = static_cast<s32>((w0 >> 10) & 0x1FFu);
            break;
        }
        case 0xE4: { // Drawing Area Bottom-Right
            clip_x2_ = static_cast<s32>(w0 & 0x3FFu);
            clip_y2_ = static_cast<s32>((w0 >> 10) & 0x1FFu);
            break;
        }
        case 0xE5: { // Drawing Offset
            draw_off_x_ = sign_extend_11(w0 & 0x7FFu);
            draw_off_y_ = sign_extend_11((w0 >> 11) & 0x7FFu);
            break;
        }
        default:
            break;
    }
}

void PsxGpu::export_bgra8888(std::vector<u32>& out_pixels, u32& out_w, u32& out_h, bool full_vram) const {
    const u32 sx = full_vram ? 0u : disp_vram_x_;
    const u32 sy = full_vram ? 0u : disp_vram_y_;
    out_w        = full_vram ? VRAM_WIDTH : disp_width_;
    out_h        = full_vram ? VRAM_HEIGHT : disp_height_;

    out_pixels.resize(static_cast<std::size_t>(out_w) * out_h);
    if (!full_vram && is_24bit_display()) {
        for (u32 y = 0; y < out_h; ++y) {
            const u32 vy = (sy + y) & 511u;
            for (u32 x = 0; x < out_w; ++x) {
                const u32 bx = (sx * 2u + x * 3u) & 2047u;
                auto vram_byte = [&](u32 b_off) -> u32 {
                    const u16 w = vram_[vy * VRAM_WIDTH + ((b_off >> 1) & 1023u)];
                    return ((b_off & 1u) != 0) ? ((w >> 8) & 0xFFu) : (w & 0xFFu);
                };
                const u32 r = vram_byte(bx);
                const u32 g = vram_byte((bx + 1u) & 2047u);
                const u32 b = vram_byte((bx + 2u) & 2047u);
                out_pixels[y * out_w + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
        return;
    }

    for (u32 y = 0; y < out_h; ++y) {
        for (u32 x = 0; x < out_w; ++x) {
            const u16 px = read_vram_pixel(sx + x, sy + y);
            const u32 r  = ((px & 0x1Fu) * 527u + 23u) >> 6;
            const u32 g  = (((px >> 5) & 0x1Fu) * 527u + 23u) >> 6;
            const u32 b  = (((px >> 10) & 0x1Fu) * 527u + 23u) >> 6;
            out_pixels[y * out_w + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

bool PsxGpu::dump_ppm(const std::filesystem::path& path, bool full_vram) const {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return false;
    }

    const u32 sx = full_vram ? 0u : disp_vram_x_;
    const u32 sy = full_vram ? 0u : disp_vram_y_;
    const u32 w  = full_vram ? VRAM_WIDTH : disp_width_;
    const u32 h  = full_vram ? VRAM_HEIGHT : disp_height_;

    out << "P6\n" << w << " " << h << "\n255\n";
    std::vector<u8> row(w * 3u);
    if (!full_vram && is_24bit_display()) {
        for (u32 y = 0; y < h; ++y) {
            const u32 vy = (sy + y) & 511u;
            for (u32 x = 0; x < w; ++x) {
                const u32 bx = (sx * 2u + x * 3u) & 2047u;
                auto vram_byte = [&](u32 b_off) -> u8 {
                    const u16 hw = vram_[vy * VRAM_WIDTH + ((b_off >> 1) & 1023u)];
                    return static_cast<u8>(((b_off & 1u) != 0) ? ((hw >> 8) & 0xFFu) : (hw & 0xFFu));
                };
                row[x * 3u + 0] = vram_byte(bx);
                row[x * 3u + 1] = vram_byte((bx + 1u) & 2047u);
                row[x * 3u + 2] = vram_byte((bx + 2u) & 2047u);
            }
            out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
        }
        return out.good();
    }

    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const u16 px = read_vram_pixel(sx + x, sy + y);
            row[x * 3u + 0] = static_cast<u8>(((px & 0x1Fu) * 527u + 23u) >> 6);
            row[x * 3u + 1] = static_cast<u8>((((px >> 5) & 0x1Fu) * 527u + 23u) >> 6);
            row[x * 3u + 2] = static_cast<u8>((((px >> 10) & 0x1Fu) * 527u + 23u) >> 6);
        }
        out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }
    return out.good();
}

} // namespace vsport
