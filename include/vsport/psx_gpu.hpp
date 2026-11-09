#pragma once

#include "vsport/types.hpp"

#include <filesystem>
#include <vector>

namespace vsport {

// PlayStation 1 GPU (1024x512 16-bit RGB555 VRAM + GP0/GP1 command FIFO & software rasterizer)
class PsxGpu {
public:
    static constexpr u32 VRAM_WIDTH  = 1024;
    static constexpr u32 VRAM_HEIGHT = 512;

    PsxGpu();
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

    [[nodiscard]] u64 primitives_drawn() const noexcept { return primitives_drawn_; }
    [[nodiscard]] u32 display_vram_x() const noexcept { return disp_vram_x_; }
    [[nodiscard]] u32 display_vram_y() const noexcept { return disp_vram_y_; }
    [[nodiscard]] u32 display_width() const noexcept { return disp_width_; }
    [[nodiscard]] u32 display_height() const noexcept { return disp_height_; }
    [[nodiscard]] bool is_24bit_display() const noexcept { return (gpustat_ & (1u << 21)) != 0; }

    // Converts either the active display region or full 1024x512 VRAM into BGRA8888 (for Win32 GDI)
    void export_bgra8888(std::vector<u32>& out_pixels, u32& out_w, u32& out_h, bool full_vram = false) const;

    // Saves VRAM or active display to a PPM image file
    bool dump_ppm(const std::filesystem::path& path, bool full_vram = true) const;

private:
    struct Vertex {
        s32 x = 0, y = 0;
        u8 r = 0, g = 0, b = 0;
        u8 u = 0, v = 0;
    };

    void dispatch_gp0_packet();
    void rasterize_triangle(const Vertex& v0, const Vertex& v1, const Vertex& v2,
                            bool shaded, bool textured, u16 clut_word, u16 texpage_word, bool semi_trans);
    u16 sample_texture(u8 u, u8 v, u16 clut_word, u16 texpage_word) const noexcept;

    static u16 rgb888_to_555(u8 r, u8 g, u8 b) noexcept {
        return static_cast<u16>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
    }

    enum class Gp0Mode {
        Command,
        CpuToVramCopy,
        VramToCpuRead
    };

    std::vector<u16> vram_;
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
    s32 clip_x1_     = 0, clip_y1_ = 0;
    s32 clip_x2_     = 1023, clip_y2_ = 511;
    s32 draw_off_x_  = 0, draw_off_y_ = 0;
    u32 disp_vram_x_ = 0, disp_vram_y_ = 0;
    u32 disp_width_  = 320, disp_height_ = 240;

    u64 primitives_drawn_ = 0;
};

} // namespace vsport
