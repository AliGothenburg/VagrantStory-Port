#pragma once

#include "vsport/hr_backend.hpp"
#include "vsport/pgxp.hpp"
#include "vsport/texture_replace.hpp"
#include "vsport/types.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace vsport {

class StateIO;

// PlayStation 1 GPU (1024x512 16-bit RGB555 VRAM + GP0/GP1 command FIFO & software rasterizer)
class PsxGpu {
public:
    static constexpr u32 VRAM_WIDTH  = 1024;
    static constexpr u32 VRAM_HEIGHT = 512;

    PsxGpu();
    ~PsxGpu();
    PsxGpu(const PsxGpu&)            = delete;
    PsxGpu& operator=(const PsxGpu&) = delete;
    void reset();

    // MMIO Registers: 0x1F801810 (GP0 / GPUREAD) and 0x1F801814 (GP1 / GPUSTAT)
    void write_gp0(u32 word);
    void write_gp1(u32 word);
    [[nodiscard]] u32 read_gpuread();
    [[nodiscard]] u32 read_gpustat() const noexcept;

    // Direct VRAM access for diagnostics and host display
    [[nodiscard]] const std::vector<u16>& vram() const noexcept { return vram_; }
    [[nodiscard]] u16 read_vram_pixel(u32 x, u32 y) const noexcept {
        return vram_[((y & 511u) * VRAM_WIDTH) + (x & 1023u)];
    }
    void write_vram_pixel(u32 x, u32 y, u16 rgb555) noexcept {
        vram_[((y & 511u) * VRAM_WIDTH) + (x & 1023u)] = rgb555;
    }
    // Pixel write by drawing / transfers, following GP0(E6h): "check mask" skips pixels whose bit
    // 15 is set, "set mask" forces bit 15 on. (Fills ignore the mask, they use write_vram_pixel.)
    void plot_vram_pixel(u32 x, u32 y, u16 c) noexcept {
        u16& d = vram_[((y & 511u) * VRAM_WIDTH) + (x & 1023u)];
        if (mask_check_ && (d & 0x8000u) != 0) return;
        d = static_cast<u16>(c | (mask_set_ ? 0x8000u : 0u));
    }

    [[nodiscard]] u64 primitives_drawn() const noexcept { return primitives_drawn_; }
    [[nodiscard]] u32 display_vram_x() const noexcept { return disp_vram_x_; }
    [[nodiscard]] u32 display_vram_y() const noexcept { return disp_vram_y_; }
    [[nodiscard]] u32 display_width() const noexcept { return disp_width_; }
    [[nodiscard]] u32 display_height() const noexcept { return disp_height_; }
    [[nodiscard]] bool is_24bit_display() const noexcept { return (gpustat_ & (1u << 21)) != 0; }
    // GP1(03h): the game can switch the video output off (the TV then shows black), e.g. while it
    // prepares VRAM for a movie
    [[nodiscard]] bool display_enabled() const noexcept { return (gpustat_ & (1u << 23)) == 0; }
    // GP1(08h) bit 3 -> GPUSTAT bit 20: 0 = NTSC (60 Hz), 1 = PAL (50 Hz)
    [[nodiscard]] bool is_pal() const noexcept { return (gpustat_ & (1u << 20)) != 0; }

    static constexpr u32 MAX_INTERNAL_SCALE = 8;
    // Internal resolution upscaling (1 = native, 2..6 = N x). The upscaled copy is display-only:
    // emulation keeps using the native VRAM.
    void set_internal_scale(u32 scale);
    // Draws all queued upscaled primitives (called before the picture is shown or dumped)
    void flush_hr();
    [[nodiscard]] u32 internal_scale() const noexcept { return hr_scale_; }
    // Bilinear texture filtering for 3D polygons in the upscaled picture (sprites/text stay sharp)
    void set_texture_filtering(bool on) noexcept { tex_filter_ = on; }
    // Sharp variant of the filter: texels keep crisp edges, only their borders are blended
    void set_texture_filter_sharp(bool on) noexcept { tex_filter_sharp_ = on; }
    // Depth buffer between polygons with exact depth (needs set_pgxp; experimental)
    void set_depth_buffer(bool on) noexcept { depth_buffer_ = on; }
    [[nodiscard]] bool texture_filtering() const noexcept { return tex_filter_; }
    // Character rim light (Vagrant Story cutscenes / menus): the game draws a bright copy of the
    // model shifted ~1 native pixel, which becomes a hard broad band at high internal resolution.
    // Soft: the copy fades out (drawn semi-transparent at the full and half shift). Sharp: a thin
    // line, the copy shifted ~1 upscaled pixel. Off: not drawn. (The native 1x picture is unchanged.)
    enum class RimLight : u8 { Soft = 0, Sharp = 1, Off = 2 };
    void set_rim_light(RimLight mode) noexcept { rim_mode_ = mode; }
    // Exact vertex positions from the GTE (sub-pixel precision in the upscaled picture); null = off
    void set_pgxp(const PgxpCache* cache) noexcept { pgxp_ = cache; }
    // Perspective-correct texture mapping in the upscaled picture (needs exact vertices with depth)
    void set_perspective_textures(bool on) noexcept { persp_tex_ = on; }
    [[nodiscard]] RimLight rim_light() const noexcept { return rim_mode_; }
    // Upscaled picture in 24-bit colour (0x00BBGGRR per pixel)
    [[nodiscard]] const std::vector<u32>& hr_vram() const;
    // Replaces the backend that draws the upscaled picture (default: software)
    void set_hr_backend(std::unique_ptr<HrBackend> backend);
    [[nodiscard]] HrBackend* hr_backend() const noexcept { return hr_scale_ > 1u ? hr_.get() : nullptr; }
    // Frame interpolation (needs a graphics card backend): the game shows a new picture every 2
    // vblanks; on the first one an in-between picture is shown (each polygon halfway between its
    // place in the previous and the new picture), on the second the new picture.
    void set_frame_interpolation(bool on);
    [[nodiscard]] bool frame_interpolation() const noexcept { return interp_on_; }
    void on_vblank() noexcept; // Called by the runtime at every vblank
    [[nodiscard]] bool showing_interpolated() const noexcept { return alt_shown_; }
    // Graphics card view of the picture to present now (main or in-between); null without one
    [[nodiscard]] void* display_picture_view() const;
    // The in-between picture of the display area as a PPM (diagnostics)
    bool dump_interpolated_ppm(const std::filesystem::path& path);
    [[nodiscard]] u64 interp_shown() const noexcept { return interp_count_; }
    [[nodiscard]] u64 interp_skipped() const noexcept { return interp_skipped_; }
    // Texture replacement packs / texture dumps in the upscaled picture (null = off)
    void set_texture_replacer(TextureReplacer* r) noexcept { texrep_ = r; }
    // Diagnostics: textured primitives looked at for replacement / found inside one intact upload
    [[nodiscard]] u64 repl_considered() const noexcept { return repl_considered_; }
    [[nodiscard]] u64 repl_in_upload() const noexcept { return repl_in_upload_; }
    [[nodiscard]] std::size_t upload_count() const noexcept { return uploads_.size(); }

    // Converts either the active display region or full 1024x512 VRAM into BGRA8888 (for Win32 GDI)
    void export_bgra8888(std::vector<u32>& out_pixels, u32& out_w, u32& out_h, bool full_vram = false) const;

    // Saves VRAM or active display to a PPM image file
    bool dump_ppm(const std::filesystem::path& path, bool full_vram = true) const;

    // Save state: writes or reads the whole state (see state_io.hpp)
    void serialize(StateIO& io);
    // Runahead: loading a state leaves the upscaled picture alone (it is restored from a snapshot)
    void set_keep_picture_on_load(bool on) noexcept { keep_picture_on_load_ = on; }
    // Runahead: copy of the upscaled picture taken with a save state, put back with its load
    void picture_snapshot(bool restore);

private:
    using Vertex = GpuVertex;

    void dispatch_gp0_packet();
    void rasterize_triangle(const Vertex& v0, const Vertex& v1, const Vertex& v2,
                            bool shaded, bool textured, u16 clut_word, u16 texpage_word, bool semi_trans);
    u16 sample_texture(u8 u, u8 v, u16 clut_word, u16 texpage_word) const noexcept;
    // Upscaled drawing is queued per primitive (with the GPU state it needs, see HrPrim) and drawn
    // by the HrBackend in flush_hr()
    // bx1..by2: native bounding box the primitive draws into
    void hr_queue(const HrPrim& p, s32 bx1, s32 by1, s32 bx2, s32 by2);
    void hr_guard_write(s32 x1, s32 y1, s32 x2, s32 y2); // Flush if queued prims read this area
    void hr_mark_tex_cells(s32 x1, s32 y1, s32 x2, s32 y2);
    void hr_sync_rect(u32 x, u32 y, u32 w, u32 h);                    // native -> upscaled (replicate)
    void hr_copy_rect(u32 sx, u32 sy, u32 dx, u32 dy, u32 w, u32 h);  // upscaled -> upscaled
    // Texture replacement: uploads still intact in VRAM, and the lookup for a queued primitive
    void track_upload(u32 x, u32 y, u32 w, u32 h);
    void invalidate_uploads(s32 x1, s32 y1, s32 x2, s32 y2); // Something else was written there
    void rebuild_upload_cells();
    void hr_match_replacement(HrPrim& p);
    // Frame interpolation (see set_frame_interpolation)
    struct RecFrame {
        std::vector<HrOp> ops;
        std::vector<std::vector<std::array<s32, 2>>> offs; // Drawing offset of each primitive (per op)
        bool safe = true; // Nothing written into the frame that the recording cannot redraw
        std::vector<std::array<s32, 4>> writes; // Uploads / copies (x1, y1, x2, y2), checked against the shown area
    };
    void interp_on_swap();
    void interp_write(s32 x1, s32 y1, s32 x2, s32 y2); // Upload / copy: frame unsafe if it lands in the shown area
    void interp_clear();

    static u16 rgb888_to_555(u8 r, u8 g, u8 b) noexcept {
        return static_cast<u16>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
    }

    enum class Gp0Mode {
        Command,
        CpuToVramCopy,
        VramToCpuRead
    };

    std::vector<u16> vram_;
    std::unique_ptr<HrBackend> hr_; // Draws the (1024*N) x (512*N) upscaled display copy
    u32 hr_scale_ = 1;
    bool tex_filter_ = false;
    bool tex_filter_sharp_ = false;
    bool depth_buffer_ = false;
    bool mask_set_   = false; // GP0(E6h) bit 0: drawn pixels get bit 15 set
    bool mask_check_ = false; // GP0(E6h) bit 1: pixels with bit 15 set are not drawn over
    RimLight rim_mode_ = RimLight::Soft;
    const PgxpCache* pgxp_ = nullptr;
    bool persp_tex_ = false;
    bool hr_rim_ = false; // The polygon being drawn is a rim-light copy (soft / sharp mode)
    std::unordered_map<u64, std::size_t> rim_pending_; // Queued rim copies waiting for their normal polygon
    float rim_dx_ = 1.0f, rim_dy_ = -1.0f;             // Shift of the last matched rim pair (native px)
    std::vector<HrPrim> hr_batch_;
    std::vector<std::array<s32, 2>> hr_batch_offs_; // Drawing offset of each queued primitive
    bool interp_on_ = false;
    bool keep_picture_on_load_ = false;
    float snap_rim_dx_ = 1.0f, snap_rim_dy_ = -1.0f; // Runahead copy of rim_dx_ / rim_dy_
    RecFrame rec_cur_, rec_prev_;
    u32 vb_count_ = 0, last_swap_vb_ = 0, alt_end_vb_ = 0;
    bool alt_shown_ = false;
    u64 interp_count_ = 0, interp_skipped_ = 0;
    s32 batch_x1_ = 0, batch_y1_ = 0, batch_x2_ = -1, batch_y2_ = -1; // Union of queued drawing areas
    // VRAM cells (64x16 px; row = y/16, bit = x/64) that queued primitives read as textures / CLUTs
    bool batch_has_tex_ = false;
    std::array<u16, 32> tex_cells_{};
    std::vector<TexUpload> uploads_;     // CPU -> VRAM uploads not overwritten since (oldest first)
    std::array<u16, 32> upload_cells_{}; // VRAM cells (as tex_cells_) covered by uploads_
    TextureReplacer* texrep_ = nullptr;
    u64 repl_considered_ = 0, repl_in_upload_ = 0;
    std::vector<u32> fifo_;
    u32 expected_words_ = 0;
    Gp0Mode mode_       = Gp0Mode::Command;
    bool in_polyline_   = false;

    // CPU <-> VRAM transfer cursors
    u32 xfer_x_ = 0, xfer_y_ = 0;
    u32 xfer_w_ = 0, xfer_h_ = 0;
    u32 xfer_cx_ = 0, xfer_cy_ = 0;
    u32 gpuread_latch_ = 0;

    // Drawing environment registers
    u32 gpustat_     = 0x14802000u; // Ready for cmd, DMA, VRAM, interlaced field bit
    u16 texpage_     = 0;
    u32 texwin_      = 0; // GP0(E2h) texture window: mask X/Y, offset X/Y (5 bits each)
    s32 clip_x1_     = 0, clip_y1_ = 0;
    s32 clip_x2_     = 1023, clip_y2_ = 511;
    s32 draw_off_x_  = 0, draw_off_y_ = 0;
    u32 disp_vram_x_ = 0, disp_vram_y_ = 0;
    u32 disp_width_  = 320, disp_height_ = 240;

    u64 primitives_drawn_ = 0;
};

} // namespace vsport
