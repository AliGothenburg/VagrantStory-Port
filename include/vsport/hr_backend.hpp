#pragma once

#include "vsport/types.hpp"

#include <memory>
#include <vector>

namespace vsport {

struct TexImage; // texture_replace.hpp

// A polygon vertex as the GPU received it
struct GpuVertex {
    s32 x = 0, y = 0;
    u8 r = 0, g = 0, b = 0;
    u8 u = 0, v = 0;
    bool exact = false;        // fx/fy hold the exact (sub-pixel) GTE position (upscaled picture)
    float fx = 0.0f, fy = 0.0f;
    float fz = 0.0f;           // GTE depth (SZ) of the exact position, for perspective texturing
};

// One primitive for the upscaled picture, with the GPU state it needs (queued by PsxGpu)
struct HrPrim {
    bool is_rect  = false;
    bool shaded   = false;
    bool textured = false;
    bool semi     = false;
    GpuVertex v[3]{};
    u16 clut = 0, texpage = 0;
    u32 texwin = 0;
    s32 cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0; // Drawing area (native, inclusive)
    s32 rx = 0, ry = 0, rw = 0, rh = 0;     // Rectangle
    u8 cr = 0, cg = 0, cb = 0, bu = 0, bv = 0;
    bool rim = false;                       // Rim-light copy (soft / sharp mode)
    float alpha = 1.0f;                     // Opacity of the result (soft rim light)
    bool mset = false, mcheck = false;      // GP0(E6h) mask state when sent
    bool self_tex = false;                  // 15bpp sprite whose texture is its own draw area
    s32 ox[3]{}, oy[3]{};                   // Extra per-vertex offset in upscaled pixels (rim light)
    // Texture replacement (texture_replace.hpp): image sampled instead of the VRAM texels, at
    // ((u - rep_u0) / rep_su, (v - rep_v0) / rep_sv) in 0..1 (u, v in texels of the texture page)
    const TexImage* repl = nullptr;
    float rep_u0 = 0.0f, rep_v0 = 0.0f, rep_su = 1.0f, rep_sv = 1.0f;
};

// One drawing step of a recorded frame (frame interpolation): a batch of primitives or a fill
struct HrOp {
    bool fill = false;
    s32 x = 0, y = 0, w = 0, h = 0; // Fill: native rectangle
    u16 color = 0;                  // Fill: RGB555
    std::vector<HrPrim> prims;
};

// Options that affect how the upscaled picture is drawn
struct HrOptions {
    bool texture_filter = false; // Bilinear filtering of 3D polygon textures
    bool depth_buffer   = false; // Per-pixel depth test between polygons with exact depth (experimental)
    bool sharp_filter   = false; // ...with texel edges kept crisp: the blend happens only in the middle third between texels
    bool perspective    = false; // Perspective-correct textures (needs exact vertices with depth)
    bool soft_rim       = false; // Rim-light copies fade out instead of a thin line
};

// The display-only upscaled copy of VRAM (1024*N x 512*N). The native VRAM stays authoritative
// in PsxGpu; a backend only draws what PsxGpu hands it. Backends: software (CPU, threads) and
// later Direct3D 11 (graphics card).
class HrBackend {
public:
    static constexpr u32 VRAM_WIDTH  = 1024;
    static constexpr u32 VRAM_HEIGHT = 512;

    virtual ~HrBackend() = default;

    // (Re)allocate at N x and start from the current native picture
    virtual void set_scale(u32 scale, const u16* native_vram) = 0;
    virtual void set_options(const HrOptions& o) = 0;
    // Draw a batch in order. Textures are read from native_vram. native_y1..y2: union of the
    // drawing areas of the batch (native rows)
    virtual void draw(const std::vector<HrPrim>& batch, s32 native_y1, s32 native_y2, const u16* native_vram) = 0;
    // Native -> upscaled (pixel replication) for a rectangle changed outside polygon drawing
    virtual void sync(u32 x, u32 y, u32 w, u32 h, const u16* native_vram) = 0;
    // Upscaled -> upscaled VRAM copy
    virtual void copy(u32 sx, u32 sy, u32 dx, u32 dy, u32 w, u32 h) = 0;
    virtual void clear() = 0;
    // Whole upscaled picture in 24-bit colour (0x00BBGGRR, bit 24 = mask bit), row-major. A
    // graphics card backend reads it back for this (slow): only for dumps and tests
    [[nodiscard]] virtual const std::vector<u32>& picture() = 0;

    // Graphics card backends: the device they draw with and a shader view of the upscaled picture
    // (RGBA8, alpha = mask bit), so a renderer on the same device can present it without copying.
    // Opaque pointers (ID3D11Device* / ID3D11ShaderResourceView*); null for the software backend.
    // Depth buffer (HrOptions::depth_buffer): forget all depth (a new picture starts)
    virtual void clear_depth() {}
    [[nodiscard]] virtual void* gpu_device() { return nullptr; }
    [[nodiscard]] virtual void* gpu_picture_view() { return nullptr; }
    // Frame interpolation: draws ops over a copy of the picture into a second picture of the same
    // layout, which the renderer can show instead of the main one. False if not supported.
    virtual bool render_alternate(const std::vector<HrOp>& ops, const u16* native_vram) { (void)ops; (void)native_vram; return false; }
    [[nodiscard]] virtual void* gpu_alt_picture_view() { return nullptr; }
    // Runahead: keep a copy of the picture (restore = false) or put it back (restore = true)
    virtual void snapshot(bool restore) = 0;
    // The second picture read back (as picture()); null if there is none. Slow: dumps and tests only
    [[nodiscard]] virtual const std::vector<u32>* alt_picture() { return nullptr; }
};

[[nodiscard]] std::unique_ptr<HrBackend> make_software_hr_backend();

} // namespace vsport
