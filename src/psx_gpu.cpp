#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "vsport/psx_gpu.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <utility>
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
    if (hr_) hr_->clear();
    hr_batch_.clear();
    hr_batch_offs_.clear();
    interp_clear();
    fifo_.clear();
    expected_words_   = 0;
    mode_             = Gp0Mode::Command;
    in_polyline_      = false;
    gpustat_          = 0x1C802000u; // Ready to receive cmd, VRAM->CPU ready, DMA block ready
    texpage_          = 0;
    texwin_           = 0;
    mask_set_         = false;
    mask_check_       = false;
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
        case 0x05: { // Start of Display Area in VRAM
            const u32 nx = param & 0x3FEu, ny = (param >> 10) & 0x1FFu;
            const bool changed = nx != disp_vram_x_ || ny != disp_vram_y_;
            disp_vram_x_ = nx;
            disp_vram_y_ = ny;
            if (changed) interp_on_swap();
            break;
        }
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
                case 2: gpuread_latch_ = texwin_; break;
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
            plot_vram_pixel(xfer_x_ + xfer_cx_, xfer_y_ + xfer_cy_, px);
            if (++xfer_cx_ >= xfer_w_) {
                xfer_cx_ = 0;
                if (++xfer_cy_ >= xfer_h_) {
                    mode_ = Gp0Mode::Command;
                    hr_sync_rect(xfer_x_, xfer_y_, xfer_w_, xfer_h_); // Uploaded image -> upscaled copy
                    track_upload(xfer_x_, xfer_y_, xfer_w_, xfer_h_);
                    if (interp_on_) interp_write(static_cast<s32>(xfer_x_), static_cast<s32>(xfer_y_), static_cast<s32>(xfer_x_ + xfer_w_ - 1), static_cast<s32>(xfer_y_ + xfer_h_ - 1));
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
    // GP0(E2h) texture window: Texcoord = (Texcoord AND NOT(Mask*8)) OR ((Offset AND Mask)*8)
    const u32 mask_x = (texwin_ & 0x1Fu) * 8u;
    const u32 mask_y = ((texwin_ >> 5) & 0x1Fu) * 8u;
    const u32 off_x  = ((texwin_ >> 10) & 0x1Fu) * 8u;
    const u32 off_y  = ((texwin_ >> 15) & 0x1Fu) * 8u;
    u = static_cast<u8>((u & ~mask_x) | (off_x & mask_x));
    v = static_cast<u8>((v & ~mask_y) | (off_y & mask_y));

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

// Internal resolution upscaling: the native 1024x512 VRAM stays authoritative (CPU reads, DMA,
// texture sampling). A display-only N x copy is drawn by an HrBackend (hr_backend.hpp) from the
// queue of primitives built here.


PsxGpu::~PsxGpu() = default;

namespace {
struct TexRects { s32 tx1, ty1, tx2, ty2, cx1, cy1, cx2, cy2; bool has_clut; };
// VRAM areas a textured primitive reads: its texture page and (paletted modes) its CLUT row
TexRects tex_rects(u16 texpage, u16 clut) noexcept {
    const u32 depth = (texpage >> 7) & 3u;
    TexRects r{};
    r.tx1 = static_cast<s32>((texpage & 0x0Fu) * 64u);
    r.ty1 = static_cast<s32>(((texpage >> 4) & 1u) * 256u);
    r.tx2 = r.tx1 + (depth == 0 ? 64 : depth == 1 ? 128 : 256) - 1;
    r.ty2 = r.ty1 + 255;
    r.has_clut = depth < 2u;
    r.cx1 = static_cast<s32>((clut & 0x3Fu) * 16u);
    r.cy1 = r.cy2 = static_cast<s32>((clut >> 6) & 0x1FFu);
    r.cx2 = r.cx1 + (depth == 0 ? 15 : 255);
    return r;
}
bool rects_overlap(s32 ax1, s32 ay1, s32 ax2, s32 ay2, s32 bx1, s32 by1, s32 bx2, s32 by2) noexcept {
    return ax1 <= bx2 && ax2 >= bx1 && ay1 <= by2 && ay2 >= by1;
}
} // namespace

namespace {
// VRAM split into 64x16 cells: row = y / 16 (32 rows), bit = x / 64 (16 columns)
u16 cell_cols(s32 x1, s32 x2) noexcept {
    x1 = std::clamp(x1, 0, 1023);
    x2 = std::clamp(x2, 0, 1023);
    const u32 c1 = static_cast<u32>(x1) / 64u, c2 = static_cast<u32>(x2) / 64u;
    return static_cast<u16>(((2u << c2) - 1u) & ~((1u << c1) - 1u));
}
} // namespace

void PsxGpu::hr_mark_tex_cells(s32 x1, s32 y1, s32 x2, s32 y2) {
    const u16 cols = cell_cols(x1, x2);
    const s32 r1 = std::clamp(y1, 0, 511) / 16, r2 = std::clamp(y2, 0, 511) / 16;
    for (s32 r = r1; r <= r2; ++r) tex_cells_[static_cast<std::size_t>(r)] |= cols;
    batch_has_tex_ = true;
}

void PsxGpu::hr_guard_write(s32 x1, s32 y1, s32 x2, s32 y2) {
    // Queued primitives sample textures from the native VRAM only when the queue is drawn, so
    // any native write into an area they read must wait until they have been drawn.
    if (hr_batch_.empty() || !batch_has_tex_) return;
    const u16 cols = cell_cols(x1, x2);
    const s32 r1 = std::clamp(y1, 0, 511) / 16, r2 = std::clamp(y2, 0, 511) / 16;
    for (s32 r = r1; r <= r2; ++r) {
        if ((tex_cells_[static_cast<std::size_t>(r)] & cols) != 0) {
            flush_hr();
            return;
        }
    }
}

void PsxGpu::hr_queue(const HrPrim& prim, s32 bx1, s32 by1, s32 bx2, s32 by2) {
    HrPrim p = prim;
    p.texwin = texwin_;
    p.cx1 = clip_x1_; p.cy1 = clip_y1_; p.cx2 = clip_x2_; p.cy2 = clip_y2_;
    // Native draw area of this primitive (it is drawn natively right after being queued)
    bx1 = std::max(bx1, p.cx1); by1 = std::max(by1, p.cy1);
    bx2 = std::min(bx2, p.cx2); by2 = std::min(by2, p.cy2);
    if (bx1 <= bx2 && by1 <= by2) hr_guard_write(bx1, by1, bx2, by2);
    // (Render to texture needs no flush: the native VRAM already holds what earlier primitives drew.)

    if (hr_batch_.empty()) {
        batch_x1_ = p.cx1; batch_y1_ = p.cy1; batch_x2_ = p.cx2; batch_y2_ = p.cy2;
        batch_has_tex_ = false;
        tex_cells_.fill(0);
        rim_pending_.clear();
    } else {
        batch_x1_ = std::min(batch_x1_, p.cx1); batch_y1_ = std::min(batch_y1_, p.cy1);
        batch_x2_ = std::max(batch_x2_, p.cx2); batch_y2_ = std::max(batch_y2_, p.cy2);
    }
    // Thin rim light: the bright copy is queued first; when the normal polygon with the same
    // texture coordinates follows, move the copy so that its shift (normally ~1 native pixel,
    // i.e. N upscaled pixels) becomes the same number of upscaled pixels.
    if (!p.is_rect && p.textured) {
        const u64 key = (static_cast<u64>(p.texpage) << 48) ^ (static_cast<u64>(p.clut) << 32) ^
                        (static_cast<u64>(p.v[0].u) | (static_cast<u64>(p.v[0].v) << 8) |
                         (static_cast<u64>(p.v[1].u) << 16) | (static_cast<u64>(p.v[1].v) << 24)) ^
                        (static_cast<u64>(p.v[2].u) << 40) ^ (static_cast<u64>(p.v[2].v) << 56);
        if (p.rim) {
            // Until its normal polygon arrives (it may come after a queue flush), use the shift of
            // the most recent matched pair: the light direction is the same for the whole model
            const s32 s = static_cast<s32>(hr_scale_);
            for (int i = 0; i < 3; ++i) {
                p.ox[i] = -static_cast<s32>(std::lround(rim_dx_ * static_cast<float>(s - 1)));
                p.oy[i] = -static_cast<s32>(std::lround(rim_dy_ * static_cast<float>(s - 1)));
            }
            rim_pending_[key] = hr_batch_.size();
        } else if (!rim_pending_.empty()) {
            const auto it = rim_pending_.find(key);
            if (it != rim_pending_.end()) {
                HrPrim& r = hr_batch_[it->second];
                // The shift is not exactly the same at every vertex (rounding), so it is applied per vertex
                bool same = r.texpage == p.texpage && r.clut == p.clut;
                for (int i = 0; i < 3 && same; ++i) {
                    same = r.v[i].u == p.v[i].u && r.v[i].v == p.v[i].v &&
                           std::abs(r.v[i].x - p.v[i].x) <= 4 && std::abs(r.v[i].y - p.v[i].y) <= 4;
                }
                if (same) {
                    const s32 s = static_cast<s32>(hr_scale_);
                    float sx = 0.0f, sy = 0.0f;
                    for (int i = 0; i < 3; ++i) {
                        r.ox[i] = -(r.v[i].x - p.v[i].x) * (s - 1);
                        r.oy[i] = -(r.v[i].y - p.v[i].y) * (s - 1);
                        // Exact model position: the copy follows it (plus the game's shift)
                        if (p.v[i].exact) {
                            r.v[i].exact = true;
                            r.v[i].fx = p.v[i].fx + static_cast<float>(r.v[i].x - p.v[i].x);
                            r.v[i].fy = p.v[i].fy + static_cast<float>(r.v[i].y - p.v[i].y);
                            r.v[i].fz = p.v[i].fz;
                        }
                        sx += static_cast<float>(r.v[i].x - p.v[i].x);
                        sy += static_cast<float>(r.v[i].y - p.v[i].y);
                    }
                    rim_dx_ = sx / 3.0f;
                    rim_dy_ = sy / 3.0f;
                }
                rim_pending_.erase(it);
            }
        }
    }
    p.mset   = mask_set_;
    p.mcheck = mask_check_;
    if (p.textured) {
        const TexRects t = tex_rects(p.texpage, p.clut);
        const bool own_tex = bx1 <= bx2 && by1 <= by2 &&
            (rects_overlap(bx1, by1, bx2, by2, t.tx1, t.ty1, t.tx2, t.ty2) ||
             (t.has_clut && rects_overlap(bx1, by1, bx2, by2, t.cx1, t.cy1, t.cx2, t.cy2)));
        // A 15bpp sprite reading its own draw area (Vagrant Story's cutscene trail effect) sees
        // its own fresh pixels as it goes; the upscaled copy reproduces that from the upscaled picture.
        p.self_tex = own_tex && p.is_rect && ((p.texpage >> 7) & 3u) == 2u && p.texwin == 0;
        if (p.self_tex && !hr_batch_.empty()) {
            // It must be drawn row by row on one thread: draw everything queued before it in
            // parallel first, so that only this sprite is drawn serially
            flush_hr();
            batch_x1_ = p.cx1; batch_y1_ = p.cy1; batch_x2_ = p.cx2; batch_y2_ = p.cy2;
            batch_has_tex_ = false;
            tex_cells_.fill(0);
            rim_pending_.clear();
        }
        hr_batch_.push_back(p);
        hr_batch_offs_.push_back({draw_off_x_, draw_off_y_});
        hr_mark_tex_cells(t.tx1, t.ty1, t.tx2, t.ty2);
        if (t.has_clut) hr_mark_tex_cells(t.cx1, t.cy1, t.cx2, t.cy2);
        if (texrep_ && !p.self_tex && p.texwin == 0 && texrep_->active()) hr_match_replacement(hr_batch_.back());
        // A primitive that draws over its own texture must be drawn before its native draw
        if (own_tex) {
            flush_hr();
            return;
        }
    } else {
        hr_batch_.push_back(p);
        hr_batch_offs_.push_back({draw_off_x_, draw_off_y_});
    }
    if (hr_batch_.size() >= 16384u) flush_hr();
}

void PsxGpu::rebuild_upload_cells() {
    upload_cells_.fill(0);
    for (const TexUpload& u : uploads_) {
        const u16 cols = cell_cols(u.x, u.x + u.w - 1);
        for (s32 r = u.y / 16; r <= (u.y + u.h - 1) / 16; ++r) upload_cells_[static_cast<std::size_t>(r)] |= cols;
    }
}

void PsxGpu::invalidate_uploads(s32 x1, s32 y1, s32 x2, s32 y2) {
    if (uploads_.empty()) return;
    // Quick test on the cells first: drawing mostly happens far from the textures
    const u16 cols = cell_cols(x1, x2);
    bool hit = false;
    for (s32 r = std::clamp(y1, 0, 511) / 16; r <= std::clamp(y2, 0, 511) / 16 && !hit; ++r) {
        hit = (upload_cells_[static_cast<std::size_t>(r)] & cols) != 0;
    }
    if (!hit) return;
    // An upload written over completely is gone; partly written over, the rest is still usable
    const std::size_t before = uploads_.size();
    std::erase_if(uploads_, [&](TexUpload& u) {
        const s32 ux2 = u.x + u.w - 1, uy2 = u.y + u.h - 1;
        if (!rects_overlap(x1, y1, x2, y2, u.x, u.y, ux2, uy2)) return false;
        if (x1 <= u.x && y1 <= u.y && x2 >= ux2 && y2 >= uy2) return true;
        const u16 d[4] = {static_cast<u16>(std::max<s32>(x1, u.x)), static_cast<u16>(std::max<s32>(y1, u.y)),
                          static_cast<u16>(std::min(x2, ux2)), static_cast<u16>(std::min(y2, uy2))};
        for (u32 i = 0; i < u.ndamage; ++i) {
            if (d[0] >= u.dmg[i][0] && d[1] >= u.dmg[i][1] && d[2] <= u.dmg[i][2] && d[3] <= u.dmg[i][3]) return false; // Known
        }
        if (u.ndamage < TexUpload::MAX_DAMAGE) {
            std::copy(d, d + 4, u.dmg[u.ndamage++]);
        } else {
            u16* m = u.dmg[TexUpload::MAX_DAMAGE - 1];
            m[0] = std::min(m[0], d[0]); m[1] = std::min(m[1], d[1]); m[2] = std::max(m[2], d[2]); m[3] = std::max(m[3], d[3]);
        }
        return false;
    });
    if (uploads_.size() != before) rebuild_upload_cells();
}

void PsxGpu::track_upload(u32 x, u32 y, u32 w, u32 h) {
    // Writes wrapping around the VRAM edge are never treated as textures
    const s32 x2 = static_cast<s32>(std::min(x + w, VRAM_WIDTH)) - 1, y2 = static_cast<s32>(std::min(y + h, VRAM_HEIGHT)) - 1;
    invalidate_uploads(static_cast<s32>(x), static_cast<s32>(y), x2, y2);
    if (x + w > VRAM_WIDTH || y + h > VRAM_HEIGHT) return;
    if (uploads_.size() >= 4096u) uploads_.erase(uploads_.begin());
    TexUpload u;
    u.x = static_cast<u16>(x); u.y = static_cast<u16>(y); u.w = static_cast<u16>(w); u.h = static_cast<u16>(h);
    u.hash = hash_vram_rect(vram_.data(), x, y, w, h);
    uploads_.push_back(u);
    rebuild_upload_cells();
}

void PsxGpu::hr_match_replacement(HrPrim& p) {
    ++repl_considered_;
    s32 umin, umax, vmin, vmax;
    if (p.is_rect) {
        umin = p.bu; umax = p.bu + p.rw - 1;
        vmin = p.bv; vmax = p.bv + p.rh - 1;
        if (umax > 255 || vmax > 255 || p.rw <= 0 || p.rh <= 0) return; // Wraps inside the page
    } else {
        umin = std::min({p.v[0].u, p.v[1].u, p.v[2].u}); umax = std::max({p.v[0].u, p.v[1].u, p.v[2].u});
        vmin = std::min({p.v[0].v, p.v[1].v, p.v[2].v}); vmax = std::max({p.v[0].v, p.v[1].v, p.v[2].v});
    }
    const u32 depth = std::min(2u, (p.texpage >> 7) & 3u);
    const s32 per_word = depth == 0 ? 4 : depth == 1 ? 2 : 1;
    const s32 tpx = static_cast<s32>((p.texpage & 0x0Fu) * 64u), tpy = static_cast<s32>(((p.texpage >> 4) & 1u) * 256u);
    const s32 x1 = tpx + umin / per_word, x2 = tpx + umax / per_word, y1 = tpy + vmin, y2 = tpy + vmax;
    if (x2 > 1023 || y2 > 511) return;
    const TexUpload* up = nullptr;
    for (auto it = uploads_.rbegin(); it != uploads_.rend(); ++it) {
        if (x1 >= it->x && x2 < it->x + it->w && y1 >= it->y && y2 < it->y + it->h) {
            bool damaged = false;
            for (u32 i = 0; i < it->ndamage && !damaged; ++i) {
                damaged = rects_overlap(x1, y1, x2, y2, it->dmg[i][0], it->dmg[i][1], it->dmg[i][2], it->dmg[i][3]);
            }
            if (!damaged) up = &*it;
            break; // The newest upload covering these texels decides
        }
    }
    if (!up) return;
    ++repl_in_upload_;
    u64 pal = 0;
    if (depth < 2u) {
        const u32 cx = (p.clut & 0x3Fu) * 16u, cy = (p.clut >> 6) & 0x1FFu, n = depth == 0 ? 16u : 256u;
        if (cx + n > VRAM_WIDTH) return;
        pal = hash_vram_rect(vram_.data(), cx, cy, n, 1);
    }
    const TexImage* img = texrep_->lookup(*up, depth, pal, vram_.data(), p.clut);
    if (!img) return;
    p.repl   = img;
    p.rep_u0 = static_cast<float>((up->x - tpx) * per_word);
    p.rep_v0 = static_cast<float>(up->y - tpy);
    p.rep_su = static_cast<float>(up->w * per_word);
    p.rep_sv = static_cast<float>(up->h);
}

void PsxGpu::flush_hr() {
    if (hr_batch_.empty()) return;
    if (hr_scale_ > 1u && hr_) {
        HrOptions o;
        o.texture_filter = tex_filter_;
        o.sharp_filter   = tex_filter_sharp_;
        o.depth_buffer   = depth_buffer_;
        o.perspective    = persp_tex_;
        o.soft_rim       = rim_mode_ == RimLight::Soft;
        hr_->set_options(o);
        hr_->draw(hr_batch_, batch_y1_, batch_y2_, vram_.data());
        if (interp_on_) {
            // Frame interpolation: keep the frame's primitives (as drawn, incl. rim-light shifts)
            if (rec_cur_.ops.empty() || rec_cur_.ops.back().fill) {
                rec_cur_.ops.emplace_back();
                rec_cur_.offs.emplace_back();
            }
            HrOp& op = rec_cur_.ops.back();
            op.prims.insert(op.prims.end(), hr_batch_.begin(), hr_batch_.end());
            rec_cur_.offs.back().insert(rec_cur_.offs.back().end(), hr_batch_offs_.begin(), hr_batch_offs_.end());
            for (const HrPrim& p : hr_batch_) rec_cur_.safe = rec_cur_.safe && !p.self_tex;
        }
    }
    hr_batch_.clear();
    hr_batch_offs_.clear();
}

void PsxGpu::set_internal_scale(u32 scale) {
    scale = std::clamp<u32>(scale, 1u, MAX_INTERNAL_SCALE);
    if (scale == hr_scale_ && (scale == 1u || hr_)) return;
    flush_hr();
    hr_scale_ = scale;
    if (scale == 1u) {
        if (hr_) hr_->set_scale(1u, vram_.data());
        return;
    }
    if (!hr_) hr_ = make_software_hr_backend();
    hr_->set_scale(scale, vram_.data()); // Starts from the current native picture
}

void PsxGpu::set_hr_backend(std::unique_ptr<HrBackend> backend) {
    flush_hr();
    hr_ = std::move(backend);
    if (hr_ && hr_scale_ > 1u) hr_->set_scale(hr_scale_, vram_.data());
}

const std::vector<u32>& PsxGpu::hr_vram() const {
    static const std::vector<u32> none;
    return (hr_ && hr_scale_ > 1u) ? hr_->picture() : none;
}

void PsxGpu::hr_sync_rect(u32 x, u32 y, u32 w, u32 h) {
    if (hr_scale_ == 1u || !hr_) return;
    flush_hr(); // Keep drawing order with queued primitives
    hr_->sync(x, y, w, h, vram_.data());
}

void PsxGpu::hr_copy_rect(u32 sx, u32 sy, u32 dx, u32 dy, u32 w, u32 h) {
    if (hr_scale_ == 1u || !hr_) return;
    flush_hr();
    hr_->copy(sx, sy, dx, dy, w, h);
}

void PsxGpu::rasterize_triangle(const Vertex& v0, const Vertex& v1_in, const Vertex& v2_in,
                                bool shaded, bool textured, u16 clut_word, u16 texpage_word, bool semi_trans) {
    if (hr_scale_ > 1u) {
        HrPrim p;
        p.shaded   = shaded;
        p.textured = textured;
        p.semi     = semi_trans;
        p.v[0] = v0;
        p.v[1] = v1_in;
        p.v[2] = v2_in;
        p.clut    = clut_word;
        p.texpage = texpage_word;
        p.rim     = hr_rim_;
        hr_queue(p, std::min({v0.x, v1_in.x, v2_in.x}), std::min({v0.y, v1_in.y, v2_in.y}),
                 std::max({v0.x, v1_in.x, v2_in.x}), std::max({v0.y, v1_in.y, v2_in.y}));
    }
    Vertex v1 = v1_in;
    Vertex v2 = v2_in;
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
    invalidate_uploads(min_x, min_y, max_x, max_y);

    const auto edge = [](s32 ax, s32 ay, s32 bx, s32 by, s32 px, s32 py) -> s64 {
        return static_cast<s64>(px - ax) * (by - ay) - static_cast<s64>(py - ay) * (bx - ax);
    };

    s64 area = edge(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);
    if (area == 0) {
        return;
    }
    // Normalize winding so every edge function is >= 0 inside the triangle
    if (area < 0) {
        std::swap(v1, v2);
        area = -area;
    }

    // PS1 fill rule: pixels exactly on a right or bottom edge are not drawn (top-left rule).
    // With this winding (screen y down), an edge a->b is "top or left" when it goes down
    // (dy > 0) or is horizontal going left (dy == 0 && dx < 0); other edges need w > 0.
    const auto bias = [](const Vertex& a, const Vertex& b) -> s64 {
        const s32 dx = b.x - a.x, dy = b.y - a.y;
        return (dy > 0 || (dy == 0 && dx < 0)) ? 0 : -1;
    };
    const s64 bias0 = bias(v1, v2);
    const s64 bias1 = bias(v2, v0);
    const s64 bias2 = bias(v0, v1);

    for (s32 py = min_y; py <= max_y; ++py) {
        for (s32 px = min_x; px <= max_x; ++px) {
            const s64 w0 = edge(v1.x, v1.y, v2.x, v2.y, px, py);
            const s64 w1 = edge(v2.x, v2.y, v0.x, v0.y, px, py);
            const s64 w2 = edge(v0.x, v0.y, v1.x, v1.y, px, py);

            if (w0 + bias0 >= 0 && w1 + bias1 >= 0 && w2 + bias2 >= 0) {
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
                    // Bit 15 of the texel is written along (it is the pixel's mask bit)
                    plot_vram_pixel(static_cast<u32>(px), static_cast<u32>(py), static_cast<u16>(out_px | (texel & 0x8000u)));
                } else {
                    const u16 out_px = semi_trans
                        ? blend_psx_abr(read_vram_pixel(static_cast<u32>(px), static_cast<u32>(py)), r, g, b, (texpage_word >> 5) & 3u)
                        : rgb888_to_555(r, g, b);
                    plot_vram_pixel(static_cast<u32>(px), static_cast<u32>(py), out_px);
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
            if (pgxp_ && hr_scale_ > 1u) {
                PgxpCache::Point pt;
                if (pgxp_->lookup(xy, pt)) {
                    verts[i].exact = true;
                    verts[i].fx    = pt.x + static_cast<float>(draw_off_x_);
                    verts[i].fy    = pt.y + static_cast<float>(draw_off_y_);
                    verts[i].fz    = pt.z;
                }
            }

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

        // Vagrant Story's rim light (cutscenes, menus): the model is drawn once with an over-bright
        // white colour (FFFFFF), shifted about one pixel toward the light, then normally on top.
        bool rim = textured && !semi && !raw_tex;
        for (int i = 0; i < num_verts && rim; ++i) {
            rim = verts[i].r == 0xFFu && verts[i].g == 0xFFu && verts[i].b == 0xFFu;
        }
        if (rim && rim_mode_ == RimLight::Off) {
            ++primitives_drawn_;
            return;
        }
        hr_rim_ = rim; // Soft or sharp: the upscaled copy is reshaped when drawn
        rasterize_triangle(verts[0], verts[1], verts[2], shaded, textured, clut_word, texpage_word, semi);
        if (quad) {
            rasterize_triangle(verts[1], verts[2], verts[3], shaded, textured, clut_word, texpage_word, semi);
        }
        hr_rim_ = false;
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

        if (hr_scale_ > 1u) {
            HrPrim p;
            p.is_rect  = true;
            p.textured = textured;
            p.semi     = semi;
            p.rx = rx; p.ry = ry; p.rw = rw; p.rh = rh;
            p.cr = cr; p.cg = cg; p.cb = cb;
            p.bu = base_u; p.bv = base_v;
            p.clut    = clut_word;
            p.texpage = texpage_;
            hr_queue(p, rx, ry, rx + rw - 1, ry + rh - 1);
        }
        {
            const s32 ix1 = std::max(rx, clip_x1_), iy1 = std::max(ry, clip_y1_);
            const s32 ix2 = std::min(rx + rw - 1, clip_x2_), iy2 = std::min(ry + rh - 1, clip_y2_);
            if (ix1 <= ix2 && iy1 <= iy2) invalidate_uploads(ix1, iy1, ix2, iy2);
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
                    plot_vram_pixel(static_cast<u32>(px), static_cast<u32>(py), static_cast<u16>(out_px | (texel & 0x8000u)));
                } else {
                    const u16 out_px = semi
                        ? blend_psx_abr(read_vram_pixel(static_cast<u32>(px), static_cast<u32>(py)), cr, cg, cb, (texpage_ >> 5) & 3u)
                        : rgb888_to_555(cr, cg, cb);
                    plot_vram_pixel(static_cast<u32>(px), static_cast<u32>(py), out_px);
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
            flush_hr(); // Queued primitives may still read textures from this area
            for (u32 dy = 0; dy < fh; ++dy) {
                for (u32 dx = 0; dx < fw; ++dx) {
                    write_vram_pixel(fx + dx, fy + dy, fill_col);
                }
            }
            hr_sync_rect(fx, fy, fw, fh);
            if (interp_on_ && hr_scale_ > 1u) {
                HrOp fop;
                fop.fill = true;
                fop.x = static_cast<s32>(fx); fop.y = static_cast<s32>(fy); fop.w = static_cast<s32>(fw); fop.h = static_cast<s32>(fh);
                fop.color = fill_col;
                rec_cur_.ops.push_back(std::move(fop));
                rec_cur_.offs.emplace_back();
            }
            if (fw > 0 && fh > 0) invalidate_uploads(static_cast<s32>(fx), static_cast<s32>(fy), static_cast<s32>(fx + fw - 1), static_cast<s32>(fy + fh - 1));
            ++primitives_drawn_;
            break;
        }
        case 0x80: { // VRAM -> VRAM Copy
            const u32 sx = fifo_[1] & 0x3FFu, sy = (fifo_[1] >> 16) & 0x1FFu;
            const u32 dx = fifo_[2] & 0x3FFu, dy = (fifo_[2] >> 16) & 0x1FFu;
            u32 cw = fifo_[3] & 0x3FFu, ch = (fifo_[3] >> 16) & 0x1FFu;
            if (cw == 0) cw = 0x400u;
            if (ch == 0) ch = 0x200u;
            flush_hr(); // Queued primitives may still read textures from the destination
            std::vector<u16> tmp(cw * ch);
            for (u32 y = 0; y < ch; ++y) {
                for (u32 x = 0; x < cw; ++x) {
                    tmp[y * cw + x] = read_vram_pixel(sx + x, sy + y);
                }
            }
            for (u32 y = 0; y < ch; ++y) {
                for (u32 x = 0; x < cw; ++x) {
                    plot_vram_pixel(dx + x, dy + y, tmp[y * cw + x]);
                }
            }
            hr_copy_rect(sx, sy, dx, dy, cw, ch); // Keep the copied picture at full detail
            invalidate_uploads(static_cast<s32>(dx), static_cast<s32>(dy), static_cast<s32>(dx + cw - 1), static_cast<s32>(dy + ch - 1));
            if (interp_on_) interp_write(static_cast<s32>(dx), static_cast<s32>(dy), static_cast<s32>(dx + cw - 1), static_cast<s32>(dy + ch - 1));
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
            flush_hr(); // Draw queued primitives before the upload replaces textures they use
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
            if (depth_buffer_ && hr_scale_ > 1u && hr_) {
                // The game sets the drawing area when it starts a new picture: depth starts over
                flush_hr();
                hr_->clear_depth();
            }
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
        case 0xE2: { // Texture Window (mask/offset in 8-texel steps)
            texwin_ = w0 & 0x000FFFFFu;
            break;
        }
        case 0xE6: { // Mask bit setting
            mask_set_   = (w0 & 1u) != 0;
            mask_check_ = (w0 & 2u) != 0;
            gpustat_ = (gpustat_ & ~(3u << 11)) | ((w0 & 3u) << 11);
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

    if (!full_vram && !display_enabled()) {
        // Video output switched off by the game: black picture
        const u32 s = (!is_24bit_display() && hr_scale_ > 1u) ? hr_scale_ : 1u;
        out << "P6\n" << (w * s) << " " << (h * s) << "\n255\n";
        const std::vector<char> black(static_cast<std::size_t>(w * s) * 3u, 0);
        for (u32 y = 0; y < h * s; ++y) out.write(black.data(), static_cast<std::streamsize>(black.size()));
        return out.good();
    }

    if (!full_vram && !is_24bit_display() && hr_scale_ > 1u) {
        // Upscaled display picture at internal resolution
        const u32 s = hr_scale_, hw = VRAM_WIDTH * s, hh = VRAM_HEIGHT * s;
        const std::vector<u32>& pic = hr_vram();
        if (pic.size() != static_cast<std::size_t>(hw) * hh) return false;
        out << "P6\n" << (w * s) << " " << (h * s) << "\n255\n";
        std::vector<u8> hrow(w * s * 3u);
        for (u32 y = 0; y < h * s; ++y) {
            for (u32 x = 0; x < w * s; ++x) {
                const u32 px = pic[static_cast<std::size_t>((sy * s + y) % hh) * hw + (sx * s + x) % hw];
                hrow[x * 3u + 0] = static_cast<u8>(px & 0xFFu);
                hrow[x * 3u + 1] = static_cast<u8>((px >> 8) & 0xFFu);
                hrow[x * 3u + 2] = static_cast<u8>((px >> 16) & 0xFFu);
            }
            out.write(reinterpret_cast<const char*>(hrow.data()), static_cast<std::streamsize>(hrow.size()));
        }
        return out.good();
    }

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

// --- Frame interpolation -------------------------------------------------------------------

namespace {
// Identity of a primitive across frames: what it draws, not where (texture, CLUT, texture
// coordinates, colours for untextured ones). Positions are matched separately.
u64 interp_key(const HrPrim& p) noexcept {
    u64 h = 0xCBF29CE484222325ull;
    const auto mix = [&](u64 v) { h = (h ^ v) * 0x100000001B3ull; };
    mix((p.is_rect ? 1u : 0u) | (p.textured ? 2u : 0u) | (p.semi ? 4u : 0u) | (p.rim ? 8u : 0u) | (p.shaded ? 16u : 0u));
    if (p.textured) {
        mix(p.texpage);
        mix(p.clut);
        if (p.is_rect) {
            mix(static_cast<u64>(p.bu) | (static_cast<u64>(p.bv) << 8));
            mix(static_cast<u64>(p.rw) | (static_cast<u64>(p.rh) << 16));
        } else {
            for (int i = 0; i < 3; ++i) mix(static_cast<u64>(p.v[i].u) | (static_cast<u64>(p.v[i].v) << 8));
        }
    } else if (p.is_rect) {
        mix(static_cast<u64>(p.cr) | (static_cast<u64>(p.cg) << 8) | (static_cast<u64>(p.cb) << 16));
        mix(static_cast<u64>(p.rw) | (static_cast<u64>(p.rh) << 16));
    } else {
        for (int i = 0; i < 3; ++i) mix(static_cast<u64>(p.v[i].r) | (static_cast<u64>(p.v[i].g) << 8) | (static_cast<u64>(p.v[i].b) << 16));
    }
    return h;
}
float vpos_x(const GpuVertex& v) noexcept { return v.exact ? v.fx : static_cast<float>(v.x); }
float vpos_y(const GpuVertex& v) noexcept { return v.exact ? v.fy : static_cast<float>(v.y); }
void prim_centre(const HrPrim& p, float& x, float& y) noexcept {
    if (p.is_rect) {
        x = static_cast<float>(p.rx) + static_cast<float>(p.rw) * 0.5f;
        y = static_cast<float>(p.ry) + static_cast<float>(p.rh) * 0.5f;
    } else {
        x = (vpos_x(p.v[0]) + vpos_x(p.v[1]) + vpos_x(p.v[2])) / 3.0f;
        y = (vpos_y(p.v[0]) + vpos_y(p.v[1]) + vpos_y(p.v[2])) / 3.0f;
    }
}
} // namespace

void PsxGpu::picture_snapshot(bool restore) {
    if (restore) { rim_dx_ = snap_rim_dx_; rim_dy_ = snap_rim_dy_; }
    else { snap_rim_dx_ = rim_dx_; snap_rim_dy_ = rim_dy_; }
    if (!hr_ || hr_scale_ <= 1u) return;
    if (!restore) flush_hr();
    hr_->snapshot(restore);
}

void PsxGpu::interp_clear() {
    rec_cur_   = RecFrame{};
    rec_prev_  = RecFrame{};
    alt_shown_ = false;
}

void PsxGpu::set_frame_interpolation(bool on) {
    if (on == interp_on_) return;
    flush_hr();
    interp_on_ = on;
    interp_clear();
}

void PsxGpu::on_vblank() noexcept {
    ++vb_count_;
    if (alt_shown_ && static_cast<s32>(vb_count_ - alt_end_vb_) >= 0) alt_shown_ = false;
}

void* PsxGpu::display_picture_view() const {
    if (!hr_ || hr_scale_ <= 1u) return nullptr;
    if (alt_shown_) {
        if (void* v = hr_->gpu_alt_picture_view()) return v;
    }
    return hr_->gpu_picture_view();
}

void PsxGpu::interp_write(s32 x1, s32 y1, s32 x2, s32 y2) {
    // An image put straight into the picture (a movie frame, a background) cannot be redrawn from
    // the recorded primitives. Checked at the swap against the area then shown; a few pixels (the
    // game writes a 2x1 marker every frame) do not matter.
    if ((x2 - x1 + 1) * (y2 - y1 + 1) > 16) rec_cur_.writes.push_back({x1, y1, x2, y2});
}
void PsxGpu::interp_on_swap() {
    const u32 interval = vb_count_ - last_swap_vb_;
    last_swap_vb_ = vb_count_;
    alt_shown_ = false;
    if (!interp_on_ || hr_scale_ <= 1u || !hr_) {
        interp_clear();
        return;
    }
    flush_hr(); // The new picture is complete (and its last primitives recorded)
    {
        const s32 dx1 = static_cast<s32>(disp_vram_x_), dy1 = static_cast<s32>(disp_vram_y_);
        const s32 dx2 = dx1 + static_cast<s32>(disp_width_) - 1, dy2 = dy1 + static_cast<s32>(disp_height_) - 1;
        for (const auto& w : rec_cur_.writes) {
            if (rects_overlap(w[0], w[1], w[2], w[3], dx1, dy1, dx2, dy2)) rec_cur_.safe = false;
        }
    }
    // Only the steady 2-vblank rhythm is interpolated (the in-between picture is exactly halfway)
    const bool usable = interval == 2u && rec_prev_.safe && rec_cur_.safe && !rec_prev_.ops.empty() && !rec_cur_.ops.empty();
    if (usable) {
        struct Ref {
            const HrPrim* p;
            s32 ox, oy;
            float cx, cy; // Centre relative to the drawing offset
            bool used;
        };
        std::vector<Ref> prev;
        std::unordered_map<u64, std::vector<u32>> index;
        for (std::size_t o = 0; o < rec_prev_.ops.size(); ++o) {
            const HrOp& op = rec_prev_.ops[o];
            for (std::size_t i = 0; i < op.prims.size() && !op.fill; ++i) {
                Ref r{&op.prims[i], rec_prev_.offs[o][i][0], rec_prev_.offs[o][i][1], 0.0f, 0.0f, false};
                prim_centre(op.prims[i], r.cx, r.cy);
                r.cx -= static_cast<float>(r.ox);
                r.cy -= static_cast<float>(r.oy);
                index[interp_key(op.prims[i])].push_back(static_cast<u32>(prev.size()));
                prev.push_back(r);
            }
        }
        std::vector<HrOp> mid = rec_cur_.ops;
        u32 total = 0, matched = 0;
        for (std::size_t o = 0; o < mid.size(); ++o) {
            HrOp& op = mid[o];
            for (std::size_t i = 0; i < op.prims.size() && !op.fill; ++i) {
                HrPrim& q = op.prims[i];
                ++total;
                const auto it = index.find(interp_key(q));
                if (it == index.end()) continue;
                const s32 qox = rec_cur_.offs[o][i][0], qoy = rec_cur_.offs[o][i][1];
                float qcx, qcy;
                prim_centre(q, qcx, qcy);
                qcx -= static_cast<float>(qox);
                qcy -= static_cast<float>(qoy);
                // Same primitive in the previous frame: the nearest unused one with the same identity
                Ref* best = nullptr;
                float best_d = 0.0f;
                for (u32 k : it->second) {
                    Ref& r = prev[k];
                    if (r.used) continue;
                    const float d = std::abs(r.cx - qcx) + std::abs(r.cy - qcy);
                    if (!best || d < best_d) { best = &r; best_d = d; }
                }
                if (!best) continue;
                best->used = true;
                ++matched;
                if (q.is_rect) continue; // Sprites (text, gauges) stay where they are
                const HrPrim& a = *best->p;
                const float dx = static_cast<float>(qox - best->ox), dy = static_cast<float>(qoy - best->oy); // Into this frame's coordinates
                float ax[3], ay[3], maxd = 0.0f;
                for (int v = 0; v < 3; ++v) {
                    ax[v] = vpos_x(a.v[v]) + dx;
                    ay[v] = vpos_y(a.v[v]) + dy;
                    maxd = std::max({maxd, std::abs(ax[v] - vpos_x(q.v[v])), std::abs(ay[v] - vpos_y(q.v[v]))});
                }
                if (maxd > 48.0f) continue; // Too far for the same polygon: probably another one
                for (int v = 0; v < 3; ++v) {
                    GpuVertex& b = q.v[v];
                    const float mx = (ax[v] + vpos_x(b)) * 0.5f, my = (ay[v] + vpos_y(b)) * 0.5f;
                    b.fz = (a.v[v].exact && b.exact) ? (a.v[v].fz + b.fz) * 0.5f : (b.exact ? b.fz : 0.0f);
                    b.exact = true;
                    b.fx = mx;
                    b.fy = my;
                    b.x = static_cast<s32>(std::floor(mx));
                    b.y = static_cast<s32>(std::floor(my));
                }
            }
        }
        // Too little found again: a scene change, shown as it is
        if (total > 0 && matched * 2 >= total && hr_->render_alternate(mid, vram_.data())) {
            alt_shown_  = true;
            alt_end_vb_ = vb_count_ + 2; // Shown at the next presented vblank, then the new picture
            ++interp_count_;
        } else {
            ++interp_skipped_;
        }
    } else if (!rec_cur_.ops.empty()) {
        ++interp_skipped_;
    }
    rec_prev_ = std::move(rec_cur_);
    rec_cur_  = RecFrame{};
}

bool PsxGpu::dump_interpolated_ppm(const std::filesystem::path& path) {
    if (!hr_ || hr_scale_ <= 1u) return false;
    const std::vector<u32>* pic = hr_->alt_picture();
    const u32 s = hr_scale_, hw = VRAM_WIDTH * s, hh = VRAM_HEIGHT * s;
    if (!pic || pic->size() != static_cast<std::size_t>(hw) * hh) return false;
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const u32 w = disp_width_, h = disp_height_, sx = disp_vram_x_, sy = disp_vram_y_;
    out << "P6\n" << (w * s) << " " << (h * s) << "\n255\n";
    std::vector<u8> row(w * s * 3u);
    for (u32 y = 0; y < h * s; ++y) {
        for (u32 x = 0; x < w * s; ++x) {
            const u32 px = (*pic)[static_cast<std::size_t>((sy * s + y) % hh) * hw + (sx * s + x) % hw];
            row[x * 3u + 0] = static_cast<u8>(px & 0xFFu);
            row[x * 3u + 1] = static_cast<u8>((px >> 8) & 0xFFu);
            row[x * 3u + 2] = static_cast<u8>((px >> 16) & 0xFFu);
        }
        out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }
    return out.good();
}

} // namespace vsport
