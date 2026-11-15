#pragma once

// Texture replacement packs: a texture the game uploads to VRAM (GP0 A0h) is recognised by a hash
// of the uploaded words plus a hash of the palette it is drawn with, and can be swapped for a
// larger PNG drawn in the upscaled picture. Dump mode writes every texture the game draws as a
// PNG under the name a replacement must have:
//
//     <upload hash, 16 hex>-<palette hash, 16 hex>-<colour depth 0/1/2>.png
//
// The image covers the whole upload (4bpp: 4 texels per VRAM word, 8bpp: 2, 15bpp: 1), drawn with
// that palette; transparent texels have alpha 0. A replacement may be any size (usually an integer
// multiple) and is sampled with bilinear filtering; alpha below 50% is transparent. The emulated
// machine never sees replacements: they only change the upscaled picture.

#include "vsport/types.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vsport {

// Straight-alpha image, 0xAABBGGRR per pixel, row-major
struct TexImage {
    u32 w = 0, h = 0;
    std::vector<u32> rgba;
};

// One CPU -> VRAM upload, at least partly intact in VRAM (tracked by PsxGpu). Later writes into
// part of it are kept as damaged rectangles: texels outside them are still the uploaded ones.
struct TexUpload {
    static constexpr u32 MAX_DAMAGE = 8;
    u16 x = 0, y = 0, w = 0, h = 0;
    u64 hash = 0;
    u16 dmg[MAX_DAMAGE][4]{}; // x1, y1, x2, y2 (inclusive); more damage is merged into the last one
    u32 ndamage = 0;
};

// FNV-1a over a VRAM rectangle (no wrap-around: the caller keeps it inside VRAM)
[[nodiscard]] u64 hash_vram_rect(const u16* vram, u32 x, u32 y, u32 w, u32 h) noexcept;

// PNG reading / writing (Windows Imaging Component; false elsewhere)
bool load_png(const std::filesystem::path& path, TexImage& out);
bool save_png(const std::filesystem::path& path, const TexImage& img);

class TextureReplacer {
public:
    // Indexes the replacement PNGs in dir and its subfolders (files whose name starts with a
    // valid key; the dump folder is skipped). Images are loaded when first drawn. Returns the count.
    std::size_t scan(const std::filesystem::path& dir);
    // Dump every newly seen texture into dir (empty = off). Already dumped files are kept.
    void set_dump_dir(const std::filesystem::path& dir);
    [[nodiscard]] bool dumping() const noexcept { return !dump_dir_.empty(); }
    [[nodiscard]] bool has_replacements() const noexcept { return !index_.empty(); }
    [[nodiscard]] bool active() const noexcept { return dumping() || has_replacements(); }

    // A textured primitive reads only texels of upload `up` (depth 0/1/2, palette hash 0 for
    // 15bpp). Returns the replacement image or null; dumps the texture if needed.
    const TexImage* lookup(const TexUpload& up, u32 depth, u64 pal_hash, const u16* vram, u16 clut);

    [[nodiscard]] static std::string key_name(u64 up_hash, u64 pal_hash, u32 depth);

    struct Stats {
        u64 lookups = 0, hits = 0, dumped = 0, load_failed = 0;
    };
    Stats stats;

private:
    static u64 key(u64 up_hash, u64 pal_hash, u32 depth) noexcept {
        return up_hash ^ (pal_hash * 0x9E3779B97F4A7C15ull) ^ (static_cast<u64>(depth) << 62);
    }
    void dump(const TexUpload& up, u32 depth, u64 pal_hash, const u16* vram, u16 clut);

    std::filesystem::path dump_dir_;
    std::unordered_map<u64, std::filesystem::path> index_;      // key -> replacement file
    std::unordered_map<u64, std::unique_ptr<TexImage>> loaded_; // key -> image (null: failed to load)
    std::unordered_set<u64> seen_;                              // Keys already dumped / present on disk
};

} // namespace vsport
