#pragma once

#include "vsport/types.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace vsport {

// Exact (sub-pixel) screen positions of GTE-projected points, for the upscaled picture
// ("PGXP"-style precision). The GTE rounds every projected point to whole pixels; that rounding
// makes PS1 geometry wobble, and at high internal resolution it is plainly visible.
//
// Vagrant Story copies the GTE results through its own code before building GPU packets, so a
// point cannot be followed by memory address. Instead the GTE stores each exact point under its
// rounded value (SXY word), and the GPU looks a vertex word up when it receives a polygon.
// Measured on the game: ~96% of polygon vertices match. A vertex shared by several polygons
// always resolves to the same position, so no cracks open between them. Geometry is sent a few
// frames after it was computed, so entries stay valid for several frames.
class PgxpCache {
public:
    struct Point {
        float x = 0.0f, y = 0.0f; // Exact screen position (before the GPU drawing offset)
        float z = 0.0f;           // Depth (SZ), for perspective-correct texturing later
        u32 frame = 0;
        bool ambiguous = false;   // Two different points rounded to this pixel in the same frame
    };

    static constexpr u32 MAX_AGE_FRAMES = 8;

    void set_enabled(bool on) noexcept {
        enabled_ = on;
        if (!on) map_.clear();
    }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    // Called once per vblank
    void clear() { map_.clear(); }
    void begin_frame() {
        ++frame_;
        if (map_.size() > 200000u) prune();
    }
    // Drops entries too old to be used (runahead copies the cache every frame: keep it small)
    void prune() {
        for (auto it = map_.begin(); it != map_.end();) {
            it = (frame_ - it->second.frame > MAX_AGE_FRAMES) ? map_.erase(it) : std::next(it);
        }
    }

    // GTE: exact point that was rounded to the SXY word `sxy`
    // Small models put many vertices on the same pixel. If two different points round to one
    // value in the same frame, the GPU cannot tell which one a vertex is: such values are not
    // used (the vertex keeps the PS1's whole-pixel position) instead of jumping between them.
    void store(u32 sxy, float x, float y, float z) {
        if (!enabled_) return;
        auto [it, fresh] = map_.try_emplace(sxy);
        Point& p = it->second;
        if (!fresh && p.frame == frame_) {
            if (std::fabs(p.x - x) > 1.0f / 64.0f || std::fabs(p.y - y) > 1.0f / 64.0f) p.ambiguous = true;
            return;
        }
        p.x = x; p.y = y; p.z = z; p.frame = frame_; p.ambiguous = false;
    }

    // GPU: exact position for a vertex word, if the GTE recently produced it. The exact point
    // must round down to the same pixel (so a wrong pick can never move a vertex a whole pixel).
    [[nodiscard]] bool lookup(u32 sxy, Point& out) const {
        if (!enabled_ || map_.empty()) return false;
        const auto it = map_.find(sxy);
        if (it == map_.end() || it->second.ambiguous || frame_ - it->second.frame > MAX_AGE_FRAMES) return false;
        const s32 ix = static_cast<s16>(sxy & 0xFFFFu), iy = static_cast<s16>(sxy >> 16);
        if (static_cast<s32>(std::floor(it->second.x)) != ix || static_cast<s32>(std::floor(it->second.y)) != iy) return false;
        out = it->second;
        return true;
    }

    // GPU: true if the GTE produced this SXY word within the last max_age frames (also when the
    // word was ambiguous). Used to tell 3D primitives from 2D ones (HUD, menus, text).
    [[nodiscard]] bool seen(u32 sxy, u32 max_age) const {
        if (!enabled_) return false;
        const auto it = map_.find(sxy);
        return it != map_.end() && frame_ - it->second.frame <= max_age;
    }

private:
    std::unordered_map<u32, Point> map_;
    u32 frame_ = 0;
    bool enabled_ = false;
};

} // namespace vsport
