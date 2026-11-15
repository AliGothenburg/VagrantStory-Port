// Software backend for the upscaled picture: the CPU rasterizes every queued primitive again at
// N x resolution, in parallel horizontal bands (one per thread). Moved here unchanged from
// psx_gpu.cpp; the result is identical to the previous built-in implementation.
#include "vsport/hr_backend.hpp"
#include "vsport/texture_replace.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace vsport {

namespace {

// --- Upscaled picture colours: 24-bit "true colour" 0x00BBGGRR ---------------------------------
// The native VRAM keeps the PS1's 15-bit colours. The upscaled display copy keeps full 8-bit
// channels, so dark scenes (textures multiplied by small light values) do not collapse into a
// few hard colour bands.
inline u8 c5to8(u32 c5) noexcept { return static_cast<u8>((c5 * 527u + 23u) >> 6); }
inline u32 pack888(u32 r, u32 g, u32 b) noexcept { return r | (g << 8) | (b << 16); }
// Bit 24 of an upscaled pixel mirrors bit 15 of a native pixel (the GP0(E6h) mask bit)
constexpr u32 HR_MASK = 0x01000000u;
inline u32 c555_to_888(u16 c) noexcept {
    return pack888(c5to8(c & 0x1Fu), c5to8((c >> 5) & 0x1Fu), c5to8((c >> 10) & 0x1Fu)) |
           ((c & 0x8000u) != 0 ? HR_MASK : 0u);
}

// Texel channel x light value in true colour. Where the PS1's own 15-bit result is 0 (very dark
// texels / light values) the result stays pure black, as on hardware: otherwise faint texture
// detail would appear in areas the game shows as black (e.g. the void around rooms).
inline u32 modulate_ch(u32 t5, u32 t8, u32 c) noexcept {
    return ((t5 * c) >> 7) == 0 ? 0u : std::min<u32>(255u, (t8 * c) >> 7);
}
// Untextured colour channel: black where the 15-bit colour would be black
inline u32 flat_ch(u32 c) noexcept { return c < 8u ? 0u : c; }

u32 blend_888(u32 bg, u32 fr, u32 fg, u32 fb, u32 abr) noexcept {
    const s32 br = static_cast<s32>(bg & 0xFFu), bgc = static_cast<s32>((bg >> 8) & 0xFFu), bb = static_cast<s32>((bg >> 16) & 0xFFu);
    const s32 r = static_cast<s32>(fr), g = static_cast<s32>(fg), b = static_cast<s32>(fb);
    switch (abr & 3u) {
        case 0:  return pack888(static_cast<u32>((br + r) >> 1), static_cast<u32>((bgc + g) >> 1), static_cast<u32>((bb + b) >> 1));
        case 1:  return pack888(static_cast<u32>(std::min(255, br + r)), static_cast<u32>(std::min(255, bgc + g)), static_cast<u32>(std::min(255, bb + b)));
        case 2:  return pack888(static_cast<u32>(std::max(0, br - r)), static_cast<u32>(std::max(0, bgc - g)), static_cast<u32>(std::max(0, bb - b)));
        default: return pack888(static_cast<u32>(std::min(255, br + (r >> 2))), static_cast<u32>(std::min(255, bgc + (g >> 2))), static_cast<u32>(std::min(255, bb + (b >> 2))));
    }
}

// Small persistent thread pool for the upscaled rasterizer: run(job) calls job(band) for every
// band in parallel (band 0 on the calling thread) and returns when all have finished.
struct HrPool {
    std::vector<std::thread> threads;
    std::mutex m;
    std::condition_variable cv_start, cv_done;
    std::function<void(u32)> job;
    u32 generation = 0;
    u32 pending    = 0;
    bool quit      = false;

    explicit HrPool(u32 workers) {
        for (u32 i = 0; i < workers; ++i) {
            threads.emplace_back([this, i] { worker(i + 1u); });
        }
    }
    ~HrPool() {
        {
            std::lock_guard<std::mutex> lk(m);
            quit = true;
        }
        cv_start.notify_all();
        for (auto& t : threads) t.join();
    }
    [[nodiscard]] u32 bands() const noexcept { return static_cast<u32>(threads.size()) + 1u; }

    void worker(u32 band) {
        u32 seen = 0;
        for (;;) {
            std::function<void(u32)> j;
            {
                std::unique_lock<std::mutex> lk(m);
                cv_start.wait(lk, [&] { return quit || generation != seen; });
                if (quit) return;
                seen = generation;
                j    = job;
            }
            j(band);
            std::lock_guard<std::mutex> lk(m);
            if (--pending == 0) cv_done.notify_one();
        }
    }
    void run(const std::function<void(u32)>& f) {
        {
            std::lock_guard<std::mutex> lk(m);
            job     = f;
            pending = static_cast<u32>(threads.size());
            ++generation;
        }
        cv_start.notify_all();
        f(0);
        std::unique_lock<std::mutex> lk(m);
        cv_done.wait(lk, [&] { return pending == 0; });
    }
};


// Texture lookup state prepared once per primitive (texture window, page, depth, CLUT)
struct TexFetch {
    const u16* vram;
    u32 mask_x, mask_y, off_x, off_y, tp_x, tp_y, depth, clut_x, clut_y;

    TexFetch(const u16* v, u32 texwin, u16 clut_word, u16 texpage_word) noexcept
        : vram(v),
          mask_x((texwin & 0x1Fu) * 8u), mask_y(((texwin >> 5) & 0x1Fu) * 8u),
          off_x(((texwin >> 10) & 0x1Fu) * 8u), off_y(((texwin >> 15) & 0x1Fu) * 8u),
          tp_x((texpage_word & 0x0Fu) * 64u), tp_y(((texpage_word >> 4) & 0x01u) * 256u),
          depth((texpage_word >> 7) & 0x03u),
          clut_x((clut_word & 0x3Fu) * 16u), clut_y((clut_word >> 6) & 0x1FFu) {}

    u16 px(u32 x, u32 y) const noexcept { return vram[((y & 511u) << 10) | (x & 1023u)]; }

    u16 fetch(u32 u, u32 v) const noexcept {
        u &= 0xFFu;
        v &= 0xFFu;
        u = (u & ~mask_x) | (off_x & mask_x);
        v = (v & ~mask_y) | (off_y & mask_y);
        if (depth == 0) {
            const u16 w = px(tp_x + (u >> 2), tp_y + v);
            return px(clut_x + ((w >> ((u & 3u) * 4u)) & 0x0Fu), clut_y);
        }
        if (depth == 1) {
            const u16 w = px(tp_x + (u >> 1), tp_y + v);
            return px(clut_x + ((w >> ((u & 1u) * 8u)) & 0xFFu), clut_y);
        }
        return px(tp_x + u, tp_y + v);
    }
};

// Replacement texture (texture_replace.hpp) at texture position (su, sv): bilinear, weighted by
// alpha so transparent pixels do not darken the edges; false = transparent (alpha below 50%)
bool repl_sample(const HrPrim& p, double su, double sv, u32& r, u32& g, u32& b) noexcept {
    const TexImage& im = *p.repl;
    const float x = static_cast<float>((su - p.rep_u0) / p.rep_su) * static_cast<float>(im.w) - 0.5f;
    const float y = static_cast<float>((sv - p.rep_v0) / p.rep_sv) * static_cast<float>(im.h) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float ax = x - fx, ay = y - fy;
    const s32 mx = static_cast<s32>(im.w) - 1, my = static_cast<s32>(im.h) - 1;
    const s32 x0 = std::clamp(static_cast<s32>(fx), 0, mx), x1 = std::clamp(static_cast<s32>(fx) + 1, 0, mx);
    const s32 y0 = std::clamp(static_cast<s32>(fy), 0, my), y1 = std::clamp(static_cast<s32>(fy) + 1, 0, my);
    const u32 c[4] = {im.rgba[static_cast<std::size_t>(y0) * im.w + x0], im.rgba[static_cast<std::size_t>(y0) * im.w + x1],
                      im.rgba[static_cast<std::size_t>(y1) * im.w + x0], im.rgba[static_cast<std::size_t>(y1) * im.w + x1]};
    const float w[4] = {(1.0f - ax) * (1.0f - ay), ax * (1.0f - ay), (1.0f - ax) * ay, ax * ay};
    float wa = 0.0f, sr = 0.0f, sg = 0.0f, sb = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const float k = w[i] * static_cast<float>(c[i] >> 24);
        wa += k;
        sr += k * static_cast<float>(c[i] & 0xFFu);
        sg += k * static_cast<float>((c[i] >> 8) & 0xFFu);
        sb += k * static_cast<float>((c[i] >> 16) & 0xFFu);
    }
    if (wa < 127.5f) return false;
    r = static_cast<u32>(sr / wa + 0.5f);
    g = static_cast<u32>(sg / wa + 0.5f);
    b = static_cast<u32>(sb / wa + 0.5f);
    return true;
}
inline u16 rgb_to_555(u32 r, u32 g, u32 b) noexcept { return static_cast<u16>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)); }

s64 floor_div(s64 a, s64 b) noexcept { // b > 0
    return (a >= 0) ? a / b : -((-a + b - 1) / b);
}

} // namespace

class SoftwareHrBackend final : public HrBackend {
public:
    void set_scale(u32 scale, const u16* native_vram) override;
    void clear_depth() override { std::fill(depth_.begin(), depth_.end(), 0.0f); }
    void set_options(const HrOptions& o) override { opt_ = o; tex_filter_ = o.texture_filter; persp_tex_ = o.perspective; }
    void draw(const std::vector<HrPrim>& batch, s32 native_y1, s32 native_y2, const u16* native_vram) override;
    void sync(u32 x, u32 y, u32 w, u32 h, const u16* native_vram) override;
    void copy(u32 sx, u32 sy, u32 dx, u32 dy, u32 w, u32 h) override;
    void clear() override { std::fill(hr_vram_.begin(), hr_vram_.end(), 0u); }
    const std::vector<u32>& picture() override { return hr_vram_; }
    void snapshot(bool restore) override {
        if (!restore) snap_ = hr_vram_;
        else if (snap_.size() == hr_vram_.size()) std::copy(snap_.begin(), snap_.end(), hr_vram_.begin());
    }

private:
    using Vertex = GpuVertex;
    void hr_draw_tri(const HrPrim& p, s32 band_y0, s32 band_y1);
    void hr_draw_rect(const HrPrim& p, s32 band_y0, s32 band_y1, s32 band_x0 = 0, s32 band_x1 = 0x7FFFFFFF);

    std::vector<u32> hr_vram_;   // (1024*N) x (512*N), 0x00BBGGRR + bit 24 mask
    std::vector<u32> snap_;      // Runahead copy of hr_vram_
    std::vector<float> depth_;   // Depth buffer (1/z per pixel, 0 = nothing), only with opt_.depth_buffer
    u32 hr_scale_ = 1;
    const u16* vram_ = nullptr;  // Native VRAM of the batch being drawn (textures)
    HrOptions opt_;
    bool tex_filter_ = false;
    bool persp_tex_  = false;
    std::unique_ptr<HrPool> hr_pool_;
};

void SoftwareHrBackend::set_scale(u32 scale, const u16* native_vram) {
    hr_scale_ = scale;
    if (scale > 1u && !hr_pool_) {
        // Leave a couple of cores for the emulation thread, audio and the desktop
        const u32 hw = std::max(2u, std::thread::hardware_concurrency());
        hr_pool_ = std::make_unique<HrPool>(std::clamp(hw - 2u, 1u, 15u));
    }
    if (scale <= 1u) {
        hr_vram_.clear();
        hr_vram_.shrink_to_fit();
        return;
    }
    // Start from the current native picture
    hr_vram_.assign(static_cast<std::size_t>(VRAM_WIDTH * scale) * (VRAM_HEIGHT * scale), 0);
    sync(0, 0, VRAM_WIDTH, VRAM_HEIGHT, native_vram);
}

void SoftwareHrBackend::sync(u32 x, u32 y, u32 w, u32 h, const u16* native_vram) {
    if (hr_scale_ <= 1u || hr_vram_.empty()) return;
    const u32 s = hr_scale_, hw = VRAM_WIDTH * s;
    for (u32 dy = 0; dy < h; ++dy) {
        const u32 ny = (y + dy) & 511u;
        for (u32 dx = 0; dx < w; ++dx) {
            const u32 nx = (x + dx) & 1023u;
            const u32 px = c555_to_888(native_vram[ny * VRAM_WIDTH + nx]);
            for (u32 sy = 0; sy < s; ++sy) {
                u32* row = &hr_vram_[static_cast<std::size_t>(ny * s + sy) * hw + nx * s];
                for (u32 sx = 0; sx < s; ++sx) row[sx] = px;
                if (!depth_.empty()) {
                    // Filled / uploaded area: nothing 3D is there any more
                    float* drow = &depth_[static_cast<std::size_t>(ny * s + sy) * hw + nx * s];
                    for (u32 sx = 0; sx < s; ++sx) drow[sx] = 0.0f;
                }
            }
        }
    }
}

void SoftwareHrBackend::copy(u32 sx, u32 sy, u32 dx, u32 dy, u32 w, u32 h) {
    if (hr_scale_ <= 1u || hr_vram_.empty()) return;
    const u32 s = hr_scale_, hw = VRAM_WIDTH * s, hh = VRAM_HEIGHT * s;
    std::vector<u32> tmp(static_cast<std::size_t>(w * s) * (h * s));
    for (u32 y = 0; y < h * s; ++y) {
        for (u32 x = 0; x < w * s; ++x) {
            tmp[static_cast<std::size_t>(y) * (w * s) + x] =
                hr_vram_[static_cast<std::size_t>((sy * s + y) % hh) * hw + (sx * s + x) % hw];
        }
    }
    for (u32 y = 0; y < h * s; ++y) {
        for (u32 x = 0; x < w * s; ++x) {
            hr_vram_[static_cast<std::size_t>((dy * s + y) % hh) * hw + (dx * s + x) % hw] =
                tmp[static_cast<std::size_t>(y) * (w * s) + x];
        }
    }
}

void SoftwareHrBackend::draw(const std::vector<HrPrim>& hr_batch_, s32 batch_y1_, s32 batch_y2_, const u16* native_vram) {
    if (hr_batch_.empty() || hr_scale_ <= 1u || hr_vram_.empty()) return;
    vram_ = native_vram;
    if (opt_.depth_buffer && depth_.size() != hr_vram_.size()) depth_.assign(hr_vram_.size(), 0.0f);
    if (!opt_.depth_buffer && !depth_.empty()) { depth_.clear(); depth_.shrink_to_fit(); }
    const s32 s  = static_cast<s32>(hr_scale_);
    const s32 y0 = std::max(0, batch_y1_ * s);
    const s32 y1 = std::min(static_cast<s32>(VRAM_HEIGHT) * s - 1, (batch_y2_ + 1) * s - 1);
    const bool soft_rim = opt_.soft_rim;
    const auto draw_band = [&](s32 by0, s32 by1) {
        for (const HrPrim& p : hr_batch_) {
            if (p.is_rect) {
                hr_draw_rect(p, by0, by1);
            } else if (p.rim && soft_rim) {
                // Soft rim light: the bright copy at the game's full shift and at half of it, both
                // semi-transparent, so the light fades outward instead of ending in a hard band.
                // (p.ox / p.oy hold the "sharp" offset that removes all but ~1 upscaled pixel.)
                HrPrim q = p;
                for (int i = 0; i < 3; ++i) { q.ox[i] = 0; q.oy[i] = 0; }
                q.alpha = 0.30f;
                hr_draw_tri(q, by0, by1);
                for (int i = 0; i < 3; ++i) { q.ox[i] = p.ox[i] / 2; q.oy[i] = p.oy[i] / 2; }
                q.alpha = 0.40f;
                hr_draw_tri(q, by0, by1);
            } else {
                hr_draw_tri(p, by0, by1);
            }
        }
    };
    // Self-texturing sprites read rows drawn earlier in the same sprite: draw such batches serially,
    // except a lone sprite that copies from the same columns (only from rows above): its columns
    // are independent and are drawn in parallel vertical strips
    const bool serial = std::any_of(hr_batch_.begin(), hr_batch_.end(), [](const HrPrim& p) { return p.self_tex; });
    if (serial && hr_pool_ && hr_batch_.size() == 1u) {
        const HrPrim& p = hr_batch_[0];
        const s32 tp_x = static_cast<s32>((p.texpage & 0x0Fu) * 64u);
        const bool same_columns = ((tp_x + p.bu - p.rx) & 1023) == 0 && p.bu + p.rw <= 256;
        if (same_columns) {
            const s32 cx0 = std::max(p.rx, p.cx1) * s, cx1 = std::min(p.rx + p.rw, p.cx2 + 1) * s - 1;
            if (cx1 - cx0 >= 32) {
                const s32 n = static_cast<s32>(hr_pool_->bands()), cols = cx1 - cx0 + 1;
                hr_pool_->run([&](u32 band) {
                    const s32 b  = static_cast<s32>(band);
                    const s32 c0 = cx0 + cols * b / n, c1 = cx0 + cols * (b + 1) / n - 1;
                    if (c0 <= c1) hr_draw_rect(p, y0, y1, c0, c1);
                });
                return;
            }
        }
    }
    if (hr_pool_ && !serial && hr_batch_.size() >= 4u && y1 - y0 >= 64) {
        // Split the touched rows into equal bands, one per thread
        const s32 n = static_cast<s32>(hr_pool_->bands());
        const s32 rows = y1 - y0 + 1;
        hr_pool_->run([&](u32 band) {
            const s32 b  = static_cast<s32>(band);
            const s32 b0 = y0 + rows * b / n;
            const s32 b1 = y0 + rows * (b + 1) / n - 1;
            if (b0 <= b1) draw_band(b0, b1);
        });
    } else {
        draw_band(y0, y1);
    }
}

void SoftwareHrBackend::hr_draw_tri(const HrPrim& p, s32 band_y0, s32 band_y1) {
    const bool shaded = p.shaded, textured = p.textured, semi_trans = p.semi;
    const u16 clut_word = p.clut, texpage_word = p.texpage;
    const s32 s  = static_cast<s32>(hr_scale_);
    const u32 hw = VRAM_WIDTH * hr_scale_;
    Vertex v[3] = {p.v[0], p.v[1], p.v[2]};

    // Same rejection as the native rasterizer
    {
        const s32 mnx = std::min({v[0].x, v[1].x, v[2].x}), mxx = std::max({v[0].x, v[1].x, v[2].x});
        const s32 mny = std::min({v[0].y, v[1].y, v[2].y}), mxy = std::max({v[0].y, v[1].y, v[2].y});
        if ((mxx - mnx) >= 1024 || (mxy - mny) >= 512) return;
    }
    s32 X[3], Y[3];
    for (int i = 0; i < 3; ++i) {
        // Exact GTE position when known (no wobble), otherwise the PS1's whole-pixel position.
        // floor() keeps the vertex inside the same native pixel as on the PS1.
        X[i] = (v[i].exact ? static_cast<s32>(std::floor(v[i].fx * static_cast<float>(s))) : v[i].x * s) + p.ox[i];
        Y[i] = (v[i].exact ? static_cast<s32>(std::floor(v[i].fy * static_cast<float>(s))) : v[i].y * s) + p.oy[i];
    }

    s64 area = static_cast<s64>(X[2] - X[0]) * (Y[1] - Y[0]) - static_cast<s64>(Y[2] - Y[0]) * (X[1] - X[0]);
    if (area == 0) return;
    if (area < 0) {
        std::swap(v[1], v[2]);
        std::swap(X[1], X[2]);
        std::swap(Y[1], Y[2]);
        area = -area;
    }

    const s32 min_x = std::max(std::min({X[0], X[1], X[2]}), p.cx1 * s);
    const s32 max_x = std::min(std::max({X[0], X[1], X[2]}), (p.cx2 + 1) * s - 1);
    const s32 min_y = std::max({std::min({Y[0], Y[1], Y[2]}), p.cy1 * s, band_y0});
    const s32 max_y = std::min({std::max({Y[0], Y[1], Y[2]}), (p.cy2 + 1) * s - 1, band_y1});
    if (min_x > max_x || min_y > max_y) return;

    // Edge i is opposite vertex i: E_i(x, y) = A*x + B*y + C; inside means E_i + bias_i >= 0
    // (bias -1 on right/bottom edges = top-left fill rule)
    struct Edge { s64 a, b, c, bias; };
    const auto make_edge = [](s32 ax, s32 ay, s32 bx, s32 by) {
        Edge e{};
        e.a = by - ay;
        e.b = -(static_cast<s64>(bx) - ax);
        e.c = -static_cast<s64>(ax) * e.a - static_cast<s64>(ay) * e.b;
        const s32 dx = bx - ax, dy = by - ay;
        e.bias = (dy > 0 || (dy == 0 && dx < 0)) ? 0 : -1;
        return e;
    };
    const Edge e[3] = {make_edge(X[1], Y[1], X[2], Y[2]), make_edge(X[2], Y[2], X[0], Y[0]), make_edge(X[0], Y[0], X[1], Y[1])};

    // Attribute = A0*b0 + A1*b1 + A2*b2 with b2 = 1 - b0 - b1: per-pixel deltas along x
    const double db0 = static_cast<double>(e[0].a) / static_cast<double>(area), db1 = static_cast<double>(e[1].a) / static_cast<double>(area);
    const auto d_x = [&](float a0, float a1, float a2) { return (static_cast<double>(a0) - a2) * db0 + (static_cast<double>(a1) - a2) * db1; };
    const double dr = d_x(v[0].r, v[1].r, v[2].r), dg = d_x(v[0].g, v[1].g, v[2].g), dbl = d_x(v[0].b, v[1].b, v[2].b);

    const TexFetch tex(vram_, p.texwin, clut_word, texpage_word);
    const u32 abr = (texpage_word >> 5) & 3u;
    const bool filter = textured && tex_filter_;
    const u32 set_mask = p.mset ? HR_MASK : 0u;
    // Exact per-pixel texture coordinate steps (see the row setup below)
    const double du_d = (static_cast<double>(v[0].u - v[2].u) * static_cast<double>(e[0].a) +
                         static_cast<double>(v[1].u - v[2].u) * static_cast<double>(e[1].a)) / static_cast<double>(area);
    const double dv_d = (static_cast<double>(v[0].v - v[2].v) * static_cast<double>(e[0].a) +
                         static_cast<double>(v[1].v - v[2].v) * static_cast<double>(e[1].a)) / static_cast<double>(area);
    constexpr double TEX_EPS = 1.0 / 4096.0; // Keeps exact integer coordinates from rounding down

    // Perspective-correct texturing: with exact vertices and their depth, u/z, v/z and 1/z vary
    // linearly across the screen; dividing per pixel gives the true texture position (no bending
    // or sliding on large polygons). Otherwise the PS1's affine mapping is used.
    bool persp = textured && persp_tex_;
    for (int i = 0; i < 3 && persp; ++i) persp = v[i].exact && v[i].fz > 0.0f;
    double q[3]{}, uq[3]{}, vq[3]{};
    double dq = 0, duq = 0, dvq = 0;
    double umin = 0, umax = 0, vmin = 0, vmax = 0;
    if (persp) {
        for (int i = 0; i < 3; ++i) {
            q[i]  = 1.0 / static_cast<double>(v[i].fz);
            uq[i] = static_cast<double>(v[i].u) * q[i];
            vq[i] = static_cast<double>(v[i].v) * q[i];
        }
        const auto step = [&](const double* a) {
            return ((a[0] - a[2]) * static_cast<double>(e[0].a) + (a[1] - a[2]) * static_cast<double>(e[1].a)) / static_cast<double>(area);
        };
        dq = step(q); duq = step(uq); dvq = step(vq);
        umin = std::min({v[0].u, v[1].u, v[2].u}); umax = std::max({v[0].u, v[1].u, v[2].u}) + 0.999;
        vmin = std::min({v[0].v, v[1].v, v[2].v}); vmax = std::max({v[0].v, v[1].v, v[2].v}) + 0.999;
        // A nearly constant coordinate stays exact (see TEX_EPS)
        if (umax - umin < 1.0) umax = umin + TEX_EPS * 2;
        if (vmax - vmin < 1.0) vmax = vmin + TEX_EPS * 2;
    }

    // Depth buffer (optional): polygons whose three vertices have an exact depth are tested per
    // pixel against what is already drawn (1/z, larger = nearer), so they hide each other
    // correctly even when the game sorted them wrongly. Other polygons are drawn as always.
    bool depth = opt_.depth_buffer && !depth_.empty();
    for (int i = 0; i < 3 && depth; ++i) depth = v[i].exact && v[i].fz > 0.0f;
    double zq[3]{}, dzq = 0;
    if (depth) {
        for (int i = 0; i < 3; ++i) zq[i] = 1.0 / static_cast<double>(v[i].fz);
        dzq = ((zq[0] - zq[2]) * static_cast<double>(e[0].a) + (zq[1] - zq[2]) * static_cast<double>(e[1].a)) / static_cast<double>(area);
    }

    for (s32 py = min_y; py <= max_y; ++py) {
        // Span of this row inside all three edges
        s64 x0 = min_x, x1 = max_x;
        bool empty = false;
        for (const Edge& ed : e) {
            const s64 k = ed.b * py + ed.c + ed.bias; // need a*x + k >= 0
            if (ed.a > 0) {
                x0 = std::max<s64>(x0, -floor_div(k, ed.a));            // x >= ceil(-k / a)
            } else if (ed.a < 0) {
                x1 = std::min<s64>(x1, floor_div(k, -ed.a));            // x <= floor(k / -a)
            } else if (k < 0) {
                empty = true;
            }
        }
        if (empty || x0 > x1) continue;

        // Texture coordinates and colours in double precision, relative to vertex 2 so that equal
        // values stay exact. A float sum like 248*b0 + 248*b1 + 248*b2 can come out as 247.99997
        // and pick the neighbouring texel (when that one is transparent, a covering polygon gets
        // holes that show up as streaks), or turn a colour of 128 into 127.
        const double bd0 = static_cast<double>(e[0].a * x0 + e[0].b * py + e[0].c) / static_cast<double>(area);
        const double bd1 = static_cast<double>(e[1].a * x0 + e[1].b * py + e[1].c) / static_cast<double>(area);
        const auto lin_c = [&](float a0, float a1, float a2) {
            return static_cast<double>(a2) + bd0 * (static_cast<double>(a0) - a2) + bd1 * (static_cast<double>(a1) - a2);
        };
        double r = lin_c(v[0].r, v[1].r, v[2].r), g = lin_c(v[0].g, v[1].g, v[2].g), bb = lin_c(v[0].b, v[1].b, v[2].b);
        double fu = v[2].u + bd0 * (v[0].u - v[2].u) + bd1 * (v[1].u - v[2].u) + TEX_EPS;
        double fv = v[2].v + bd0 * (v[0].v - v[2].v) + bd1 * (v[1].v - v[2].v) + TEX_EPS;
        const auto lin = [&](const double* a) { return a[2] + bd0 * (a[0] - a[2]) + bd1 * (a[1] - a[2]); };
        double cq = persp ? lin(q) : 0.0, cuq = persp ? lin(uq) : 0.0, cvq = persp ? lin(vq) : 0.0;

        double czq = depth ? lin(zq) : 0.0;
        float* drow = depth ? &depth_[static_cast<std::size_t>(py) * hw] : nullptr;

        u32* row = &hr_vram_[static_cast<std::size_t>(py) * hw];
        for (s64 px = x0; px <= x1; ++px, r += dr, g += dg, bb += dbl, fu += du_d, fv += dv_d, cq += dq, cuq += duq, cvq += dvq, czq += dzq) {
            u32 cr = v[0].r, cg = v[0].g, cb = v[0].b;
            if (shaded) {
                cr = static_cast<u32>(std::clamp(r, 0.0, 255.0));
                cg = static_cast<u32>(std::clamp(g, 0.0, 255.0));
                cb = static_cast<u32>(std::clamp(bb, 0.0, 255.0));
            }
            u32& dst = row[px];
            if (p.mcheck && (dst & HR_MASK) != 0) continue; // GP0(E6h) check mask
            if (depth && static_cast<float>(czq) < drow[px] * 0.999f) continue; // Behind what is drawn
            if (!textured) {
                cr = flat_ch(cr); cg = flat_ch(cg); cb = flat_ch(cb);
                dst = (semi_trans ? blend_888(dst, cr, cg, cb, abr) : pack888(cr, cg, cb)) | set_mask;
                if (depth && !semi_trans) drow[px] = static_cast<float>(czq);
                continue;
            }
            // Texture position: perspective-correct when possible, else affine
            double su = fu, sv = fv;
            if (persp && cq > 0.0) {
                su = std::clamp(cuq / cq + TEX_EPS, umin, umax);
                sv = std::clamp(cvq / cq + TEX_EPS, vmin, vmax);
            }
            u16 texel;
            u32 tr, tg, tb;
            if (p.repl) {
                // Replacement texture; the semi-transparency bit still comes from the game's texel
                const u16 nt = tex.fetch(static_cast<u32>(static_cast<s32>(su)), static_cast<u32>(static_cast<s32>(sv)));
                if (!repl_sample(p, su, sv, tr, tg, tb)) continue;
                texel = static_cast<u16>(rgb_to_555(tr, tg, tb) | (nt & 0x8000u));
            } else if (!filter) {
                texel = tex.fetch(static_cast<u32>(static_cast<s32>(su)), static_cast<u32>(static_cast<s32>(sv)));
                if (texel == 0) continue; // Transparent texel
                tr = c5to8(texel & 0x1Fu); tg = c5to8((texel >> 5) & 0x1Fu); tb = c5to8((texel >> 10) & 0x1Fu);
            } else {
                // Bilinear filtering between the 4 surrounding texels (centres at +0.5). Transparency
                // follows the nearest of them, as on hardware; transparent neighbours are left out of
                // the blend so cut-out edges do not bleed black.
                const float cu = static_cast<float>(su) - 0.5f, cv = static_cast<float>(sv) - 0.5f;
                const float flu = std::floor(cu), flv = std::floor(cv);
                float ax = cu - flu, ay = cv - flv;
                if (opt_.sharp_filter) {
                    // Crisp texels: blend only in the middle third between two texel centres
                    ax = std::clamp((ax - 0.5f) * 3.0f + 0.5f, 0.0f, 1.0f);
                    ay = std::clamp((ay - 0.5f) * 3.0f + 0.5f, 0.0f, 1.0f);
                }
                const u32 iu = static_cast<u32>(static_cast<s32>(flu)), iv = static_cast<u32>(static_cast<s32>(flv));
                const u16 t00 = tex.fetch(iu, iv), t10 = tex.fetch(iu + 1u, iv);
                const u16 t01 = tex.fetch(iu, iv + 1u), t11 = tex.fetch(iu + 1u, iv + 1u);
                texel = (ay < 0.5f) ? ((ax < 0.5f) ? t00 : t10) : ((ax < 0.5f) ? t01 : t11);
                if (texel == 0) continue;
                tr = c5to8(texel & 0x1Fu); tg = c5to8((texel >> 5) & 0x1Fu); tb = c5to8((texel >> 10) & 0x1Fu);
                const float w00 = t00 ? (1.0f - ax) * (1.0f - ay) : 0.0f, w10 = t10 ? ax * (1.0f - ay) : 0.0f;
                const float w01 = t01 ? (1.0f - ax) * ay : 0.0f,          w11 = t11 ? ax * ay : 0.0f;
                const float wsum = w00 + w10 + w01 + w11;
                if (wsum > 0.0f) {
                    const auto ch = [&](u32 shift) {
                        return (w00 * static_cast<float>((t00 >> shift) & 0x1Fu) + w10 * static_cast<float>((t10 >> shift) & 0x1Fu) +
                                w01 * static_cast<float>((t01 >> shift) & 0x1Fu) + w11 * static_cast<float>((t11 >> shift) & 0x1Fu)) *
                               ((255.0f / 31.0f) / wsum);
                    };
                    tr = static_cast<u32>(ch(0) + 0.5f);
                    tg = static_cast<u32>(ch(5) + 0.5f);
                    tb = static_cast<u32>(ch(10) + 0.5f);
                }
            }
            const u32 mr = modulate_ch(texel & 0x1Fu, tr, cr);
            const u32 mg = modulate_ch((texel >> 5) & 0x1Fu, tg, cg);
            const u32 mb = modulate_ch((texel >> 10) & 0x1Fu, tb, cb);
            u32 out = (semi_trans && (texel & 0x8000u) != 0) ? blend_888(dst, mr, mg, mb, abr) : pack888(mr, mg, mb);
            if (p.alpha < 1.0f) {
                // Partly transparent (soft rim light): mix with what is already there
                const auto mix = [&](u32 shift) {
                    const float d = static_cast<float>((dst >> shift) & 0xFFu), o = static_cast<float>((out >> shift) & 0xFFu);
                    return static_cast<u32>(d + (o - d) * p.alpha + 0.5f) << shift;
                };
                out = mix(0) | mix(8) | mix(16);
            }
            dst = out | set_mask | ((texel & 0x8000u) != 0 ? HR_MASK : 0u);
            if (depth && !(semi_trans && (texel & 0x8000u) != 0) && p.alpha >= 1.0f) drow[px] = static_cast<float>(czq);
        }
    }
}

void SoftwareHrBackend::hr_draw_rect(const HrPrim& p, s32 band_y0, s32 band_y1, s32 band_x0, s32 band_x1) {
    const s32 rx = p.rx, ry = p.ry, rw = p.rw, rh = p.rh;
    const bool textured = p.textured, semi = p.semi;
    const u8 cr = p.cr, cg = p.cg, cb = p.cb, base_u = p.bu, base_v = p.bv;
    const s32 s  = static_cast<s32>(hr_scale_);
    const u32 hw = VRAM_WIDTH * hr_scale_;
    const s32 x0 = std::max({rx * s, p.cx1 * s, band_x0}), x1 = std::min({(rx + rw) * s - 1, (p.cx2 + 1) * s - 1, band_x1});
    const s32 y0 = std::max({ry * s, p.cy1 * s, band_y0}), y1 = std::min({(ry + rh) * s - 1, (p.cy2 + 1) * s - 1, band_y1});
    const u32 abr = (p.texpage >> 5) & 3u;
    const u32 set_mask = p.mset ? HR_MASK : 0u;
    const TexFetch tex(vram_, p.texwin, p.clut, p.texpage);
    const u32 hh = VRAM_HEIGHT * hr_scale_;
    const s32 tp_x = static_cast<s32>((p.texpage & 0x0Fu) * 64u), tp_y = static_cast<s32>(((p.texpage >> 4) & 1u) * 256u);
    for (s32 py = y0; py <= y1; ++py) {
        u32* row = &hr_vram_[static_cast<std::size_t>(py) * hw];
        const s32 dy = py / s - ry; // Sprites keep their native texel grid
        for (s32 px = x0; px <= x1; ++px) {
            u32& dst = row[px];
            if (p.mcheck && (dst & HR_MASK) != 0) continue; // GP0(E6h) check mask
            if (textured) {
                const s32 dx = px / s - rx;
                u32 tr, tg, tb;
                bool tsemi;
                u16 t5; // 15-bit texel (for the "result is black" test)
                if (p.self_tex) {
                    // Texture = this sprite's own draw area: read the upscaled picture itself (same
                    // sub-pixel), so pixels written earlier by this sprite are seen, as natively
                    const u32 sx = static_cast<u32>((tp_x + ((base_u + dx) & 0xFF)) * s + (px % s)) % hw;
                    const u32 sy = static_cast<u32>((tp_y + ((base_v + dy) & 0xFF)) * s + (py % s)) % hh;
                    const u32 sv = hr_vram_[static_cast<std::size_t>(sy) * hw + sx];
                    if ((sv & (0xFFFFFFu | HR_MASK)) == 0) continue; // Transparent (black, no mask)
                    tr = sv & 0xFFu; tg = (sv >> 8) & 0xFFu; tb = (sv >> 16) & 0xFFu;
                    tsemi = (sv & HR_MASK) != 0;
                    t5 = static_cast<u16>((tr >> 3) | ((tg >> 3) << 5) | ((tb >> 3) << 10));
                } else if (p.repl) {
                    // Replacement texture, sampled at the upscaled pixel (more detail than the game's texels)
                    const u16 nt = tex.fetch(static_cast<u32>(base_u + dx), static_cast<u32>(base_v + dy));
                    const double su = base_u + (px + 0.5) / s - rx, sv = base_v + (py + 0.5) / s - ry;
                    if (!repl_sample(p, su, sv, tr, tg, tb)) continue;
                    tsemi = (nt & 0x8000u) != 0;
                    t5 = rgb_to_555(tr, tg, tb);
                } else {
                    const u16 texel = tex.fetch(static_cast<u32>(base_u + dx), static_cast<u32>(base_v + dy));
                    if (texel == 0) continue;
                    tr = c5to8(texel & 0x1Fu); tg = c5to8((texel >> 5) & 0x1Fu); tb = c5to8((texel >> 10) & 0x1Fu);
                    tsemi = (texel & 0x8000u) != 0;
                    t5 = texel;
                }
                const u32 mr = modulate_ch(t5 & 0x1Fu, tr, cr);
                const u32 mg = modulate_ch((t5 >> 5) & 0x1Fu, tg, cg);
                const u32 mb = modulate_ch((t5 >> 10) & 0x1Fu, tb, cb);
                dst = ((semi && tsemi) ? blend_888(dst, mr, mg, mb, abr) : pack888(mr, mg, mb)) | set_mask | (tsemi ? HR_MASK : 0u);
            } else {
                dst = (semi ? blend_888(dst, flat_ch(cr), flat_ch(cg), flat_ch(cb), abr) : pack888(flat_ch(cr), flat_ch(cg), flat_ch(cb))) | set_mask;
            }
        }
    }
}

std::unique_ptr<HrBackend> make_software_hr_backend() {
    return std::make_unique<SoftwareHrBackend>();
}

} // namespace vsport
