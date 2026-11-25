// Graphics card (Direct3D 11) upscaled picture vs the software one, on Microsoft's WARP software
// rasterizer so the test runs on any Windows machine (no graphics card needed). Both PsxGpu
// instances get the same GP0 command stream (random primitives of every kind); the pictures must
// match apart from a handful of pixels.
#include "vsport/d3d11_hr_backend.hpp"
#include "vsport/hr_backend.hpp"
#include "vsport/psx_gpu.hpp"

#ifdef _WIN32
#include <d3d11.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

using namespace vsport;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                                   \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " -> " << msg << "\n"; \
            ++g_failures;                                                                  \
        }                                                                                  \
    } while (0)

constexpr u32 S = 3; // Internal scale used by the tests

struct Rng {
    u32 s;
    u32 next() { s = s * 1664525u + 1013904223u; return s >> 8; }
    u32 range(u32 n) { return next() % n; }
};

u32 xy(s32 x, s32 y) { return (static_cast<u32>(static_cast<u16>(y)) << 16) | static_cast<u16>(x); }

// Texture pages used by the random primitives
constexpr u16 TP_15 = 0x010Au;  // x=640, 15bpp
constexpr u16 TP_4  = 0x000Bu;  // x=704, 4bpp
constexpr u16 TP_8  = 0x008Cu;  // x=768, 8bpp
constexpr u16 CLUT_4 = 480u << 6; // (0,480)
constexpr u16 CLUT_8 = 481u << 6; // (0,481)

void upload(std::vector<u32>& cmd, u32 x, u32 y, u32 w, u32 h, Rng& rng, u32 transparent_pct) {
    cmd.push_back(0xA0000000u);
    cmd.push_back(xy(static_cast<s32>(x), static_cast<s32>(y)));
    cmd.push_back((h << 16) | w);
    const u32 n = w * h;
    for (u32 i = 0; i < n; i += 2) {
        u32 pair = 0;
        for (u32 k = 0; k < 2; ++k) {
            u32 c = rng.next() & 0xFFFFu;
            if (rng.range(100) < transparent_pct) c = 0;
            pair |= c << (16 * k);
        }
        cmd.push_back(pair);
    }
}

// Common set-up: drawing area, textures and CLUTs
void setup(std::vector<u32>& cmd, Rng& rng) {
    cmd.push_back(0xE3000000u);                          // Draw area (0,0)-(319,239)
    cmd.push_back(0xE4000000u | (239u << 10) | 319u);
    cmd.push_back(0xE5000000u);                          // Offset 0
    cmd.push_back(0x02203040u);                          // Background fill
    cmd.push_back(0u);
    cmd.push_back((240u << 16) | 320u);
    upload(cmd, 640, 0, 64, 64, rng, 15);                // 15bpp texels (some transparent, random STP)
    upload(cmd, 704, 0, 16, 64, rng, 0);                 // 4bpp indices
    upload(cmd, 768, 0, 32, 64, rng, 0);                 // 8bpp indices
    upload(cmd, 0, 480, 16, 1, rng, 20);                 // 4bpp CLUT
    upload(cmd, 0, 481, 256, 1, rng, 10);                // 8bpp CLUT
}

struct Opts {
    bool semi = false, mask = false, window = false, sprites = true;
    u32 count = 300;
};

void random_prims(std::vector<u32>& cmd, Rng& rng, const Opts& o, std::vector<std::pair<s32, s32>>* verts) {
    for (u32 n = 0; n < o.count; ++n) {
        if (o.mask && rng.range(8) == 0) cmd.push_back(0xE6000000u | rng.range(4));
        if (o.window && rng.range(8) == 0) {
            cmd.push_back(0xE2000000u | rng.range(4) | (rng.range(4) << 5) | (rng.range(8) << 10) | (rng.range(8) << 15));
        }
        const u32 kind = rng.range(o.sprites ? 10 : 8);
        const u32 tsel = rng.range(3);
        const u16 tp_base = tsel == 0 ? TP_15 : (tsel == 1 ? TP_4 : TP_8);
        const u16 clut = tsel == 2 ? CLUT_8 : CLUT_4;
        const bool semi = o.semi && rng.range(2) == 0;
        const u16 tp = static_cast<u16>(tp_base | (rng.range(4) << 5)); // Random ABR mode
        const u32 col = rng.next() & 0xFFFFFFu;
        if (kind < 8) {
            const bool quad = (kind & 1) != 0, gouraud = (kind & 2) != 0, tex = (kind & 4) != 0;
            const bool raw = tex && rng.range(3) == 0;
            u32 op = 0x20u | (gouraud ? 0x10u : 0u) | (quad ? 0x08u : 0u) | (tex ? 0x04u : 0u) | (semi ? 0x02u : 0u) | (raw ? 0x01u : 0u);
            const s32 cx = static_cast<s32>(rng.range(340)) - 10, cy = static_cast<s32>(rng.range(260)) - 10;
            const s32 sz = 4 + static_cast<s32>(rng.range(rng.range(4) == 0 ? 150 : 50));
            cmd.push_back((op << 24) | col);
            const int nv = quad ? 4 : 3;
            for (int i = 0; i < nv; ++i) {
                if (gouraud && i > 0) cmd.push_back(rng.next() & 0xFFFFFFu);
                const s32 x = cx + static_cast<s32>(rng.range(static_cast<u32>(sz))) - sz / 2;
                const s32 y = cy + static_cast<s32>(rng.range(static_cast<u32>(sz))) - sz / 2;
                cmd.push_back(xy(x, y));
                if (verts) verts->push_back({x, y});
                if (tex) {
                    u32 uv = rng.range(64) | (rng.range(64) << 8);
                    if (i == 0) uv |= static_cast<u32>(clut) << 16;
                    if (i == 1) uv |= static_cast<u32>(tp) << 16;
                    cmd.push_back(uv);
                }
            }
        } else {
            // Sprite (variable size), textured or flat
            const bool tex = kind == 9;
            const bool raw = tex && rng.range(3) == 0;
            cmd.push_back(0xE1000000u | tp);
            const u32 op = 0x60u | (tex ? 0x04u : 0u) | (semi ? 0x02u : 0u) | (raw ? 0x01u : 0u);
            cmd.push_back((op << 24) | col);
            cmd.push_back(xy(static_cast<s32>(rng.range(330)) - 5, static_cast<s32>(rng.range(250)) - 5));
            if (tex) cmd.push_back(rng.range(32) | (rng.range(32) << 8) | (static_cast<u32>(clut) << 16));
            cmd.push_back(((1u + rng.range(40)) << 16) | (1u + rng.range(40)));
        }
    }
    cmd.push_back(0xE6000000u);
    cmd.push_back(0xE2000000u);
}

#ifdef _WIN32
struct Device {
    ID3D11Device* dev = nullptr;
    ~Device() { if (dev) dev->Release(); }
};
#endif

// Draws the command stream with both backends and returns the number of differing pixels
// (any colour channel off by more than `tol`, or the mask bit different)
u32 compare(const char* name, void* device, const std::vector<u32>& cmd, u32 tol,
            const std::function<void(PsxGpu&)>& configure = {}) {
    PsxGpu sw, gpu;
    sw.set_internal_scale(S);
    gpu.set_internal_scale(S);
    std::string err;
    auto b = make_d3d11_hr_backend(device, &err);
    if (!b) {
        std::cerr << "[FAIL] " << name << ": D3D11 backend: " << err << "\n";
        ++g_failures;
        return ~0u;
    }
    gpu.set_hr_backend(std::move(b));
    if (configure) { configure(sw); configure(gpu); }
    for (u32 w : cmd) { sw.write_gp0(w); gpu.write_gp0(w); }
    sw.flush_hr();
    gpu.flush_hr();
    const auto& a = sw.hr_vram();
    const auto& c = gpu.hr_vram();
    if (a.size() != c.size() || a.empty()) {
        std::cerr << "[FAIL] " << name << ": picture sizes " << a.size() << " / " << c.size() << "\n";
        ++g_failures;
        return ~0u;
    }
    u32 diff = 0, first = ~0u;
    const u32 w = PsxGpu::VRAM_WIDTH * S;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const u32 p = a[i], q = c[i];
        bool d = ((p ^ q) & 0x01000000u) != 0;
        for (u32 sh = 0; sh < 24 && !d; sh += 8) {
            const int dp = static_cast<int>((p >> sh) & 0xFFu) - static_cast<int>((q >> sh) & 0xFFu);
            d = std::abs(dp) > static_cast<int>(tol);
        }
        if (d) {
            if (first == ~0u) first = static_cast<u32>(i);
            if (std::getenv("VS_GPU_TEST_LIST") && diff < 40) {
                std::printf("    fark %u,%u (dogal %u,%u) sw %08X gpu %08X\n", static_cast<u32>(i % w), static_cast<u32>(i / w),
                            static_cast<u32>(i % w) / S, static_cast<u32>(i / w) / S, p, q);
            }
            ++diff;
        }
    }
    if (const char* dir = std::getenv("VS_GPU_TEST_DUMP")) {
        // Diagnostics: both pictures (display area) as PPM files
        for (int k = 0; k < 2; ++k) {
            const std::string path = std::string(dir) + "/" + name + (k ? "_gpu.ppm" : "_sw.ppm");
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                const u32 ow = 320 * S, oh = 240 * S;
                std::fprintf(f, "P6\n%u %u\n255\n", ow, oh);
                const auto& pic = k ? c : a;
                for (u32 y = 0; y < oh; ++y) {
                    for (u32 x = 0; x < ow; ++x) {
                        const u32 v = pic[static_cast<std::size_t>(y) * w + x];
                        const unsigned char rgb[3] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v >> 16)};
                        std::fwrite(rgb, 1, 3, f);
                    }
                }
                std::fclose(f);
            }
        }
    }
    std::printf("  %-34s %6u farkli piksel", name, diff);
    if (diff) std::printf("  (ilk: %u,%u sw %08X gpu %08X)", first % w, first / w, a[first], c[first]);
    std::printf("\n");
    return diff;
}

} // namespace

int main() {
#ifdef _WIN32
    Device warp;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &warp.dev,
                                 nullptr, nullptr))) {
        std::cerr << "[FAIL] Could not create the WARP device\n";
        return 1;
    }
    std::printf("Ekran karti (WARP) / islemci yuksek cozunurluk karsilastirmasi, %ux:\n", S);
    // Allowed differences: pixels exactly on a texel or edge boundary can round either way
    constexpr u32 LIMIT = 15, TOL = 4; // Stacked semi-transparent layers round by 1 each
    {
        // Edges: pixels exactly on an edge follow the same fill rule, also for triangles reaching
        // outside the picture (which Direct3D must not clip) and in both vertex orders
        const s32 tris[6][6] = {{-10, -25, 90, 80, 20, 80}, {20, 20, 390, 20, 90, 80}, {90, 20, 20, 80, 90, 80},
                                {20, 20, 97, 23, 21, 81}, {5, 3, 160, 140, 2, 150}, {300, -40, 340, 100, 250, 60}};
        u32 total = 0;
        for (int t = 0; t < 6; ++t) {
            for (int order = 0; order < 2; ++order) {
                std::vector<u32> cmd;
                Rng rng{1u};
                setup(cmd, rng);
                cmd.push_back(0x20FFFFFFu);
                for (int i = 0; i < 3; ++i) {
                    const int k = order ? 2 - i : i;
                    cmd.push_back(xy(tris[t][k * 2], tris[t][k * 2 + 1]));
                }
                char nm[32];
                std::snprintf(nm, sizeof nm, "kenar kurali %02d", t * 2 + order + 1);
                total += compare(nm, warp.dev, cmd, 0);
            }
        }
        CHECK(total == 0u, "edge fill rule: " << total << " pixels differ");
    }
    const auto run = [&](const char* name, u32 seed, const Opts& o, const std::function<void(PsxGpu&)>& cfg = {}) {
        Rng rng{seed};
        std::vector<u32> cmd;
        setup(cmd, rng);
        random_prims(cmd, rng, o, nullptr);
        const u32 d = compare(name, warp.dev, cmd, TOL, cfg);
        CHECK(d <= LIMIT, name << ": " << d << " pixels differ");
    };
    run("opak ucgen+dortgen+sprite", 1u, Opts{});
    run("opak (baska tohum)", 7u, Opts{});
    Opts semi; semi.semi = true;
    run("yari saydam (4 ABR, doku STP)", 2u, semi);
    Opts mask; mask.mask = true; mask.semi = true;
    run("maske biti (E6 set+check)", 3u, mask);
    Opts win; win.window = true;
    run("doku penceresi (E2)", 4u, win);
    run("doku yumusatma", 5u, Opts{}, [](PsxGpu& g) { g.set_texture_filtering(true); });
    run("keskin doku yumusatma", 5u, Opts{}, [](PsxGpu& g) { g.set_texture_filtering(true); g.set_texture_filter_sharp(true); });
    {
        Opts rim; rim.sprites = false;
        run("kenar isigi yumusak", 9u, rim, [](PsxGpu& g) { g.set_rim_light(PsxGpu::RimLight::Soft); });
    }

    // Perspective: every vertex gets an exact sub-pixel position and a depth
    {
        Rng rng{6u};
        std::vector<u32> cmd;
        std::vector<std::pair<s32, s32>> verts;
        setup(cmd, rng);
        Opts o; o.sprites = false; o.count = 200;
        random_prims(cmd, rng, o, &verts);
        PgxpCache cache;
        cache.set_enabled(true);
        Rng zr{60u};
        for (const auto& v : verts) {
            const float fx = static_cast<float>(v.first) + static_cast<float>(zr.range(1000)) / 1000.0f;
            const float fy = static_cast<float>(v.second) + static_cast<float>(zr.range(1000)) / 1000.0f;
            cache.store(xy(v.first, v.second), fx, fy, 100.0f + static_cast<float>(zr.range(4000)));
        }
        const u32 d = compare("perspektifli doku + kesin kose", warp.dev, cmd, TOL, [&](PsxGpu& g) {
            g.set_pgxp(&cache);
            g.set_perspective_textures(true);
        });
        CHECK(d <= LIMIT, "perspective: " << d << " pixels differ");
    }

    // Sprites texturing from their own draw area (trail effect: 2 rows up; in place) and VRAM copies
    {
        Rng rng{8u};
        std::vector<u32> cmd;
        setup(cmd, rng);
        Opts o; o.count = 120;
        random_prims(cmd, rng, o, nullptr);
        for (u32 k = 0; k < 6; ++k) {
            const u32 x = rng.range(200), y = 2 + rng.range(150);
            const u32 tpx = (x / 64) * 64;
            cmd.push_back(0xE1000000u | (2u << 7) | (x / 64) | ((k & 1) ? 0x20u : 0u)); // 15bpp page over the picture
            const u32 op = 0x64u | ((k & 2) ? 0x02u : 0u) | ((k & 4) ? 0x01u : 0u);
            cmd.push_back((op << 24) | 0x707070u);
            cmd.push_back(xy(static_cast<s32>(x), static_cast<s32>(y)));
            const u32 dv = (k % 3 == 0) ? 0u : 2u; // In place or 2 rows up
            cmd.push_back((x - tpx) | ((y - dv) << 8));
            cmd.push_back(((20u + rng.range(60)) << 16) | (20u + rng.range(100)));
        }
        for (u32 k = 0; k < 4; ++k) {
            cmd.push_back(0x80000000u);
            cmd.push_back(xy(static_cast<s32>(rng.range(280)), static_cast<s32>(rng.range(200))));
            cmd.push_back(xy(static_cast<s32>(rng.range(280)), static_cast<s32>(rng.range(200))));
            cmd.push_back(((1u + rng.range(40)) << 16) | (1u + rng.range(40)));
        }
        const u32 d = compare("kendi alanindan dokulanan sprite + kopya", warp.dev, cmd, TOL);
        CHECK(d <= LIMIT, "self-texturing: " << d << " pixels differ");
    }

    // Texture replacement: dump the textures of a random scene, make 4x replacements with a
    // pattern and transparent holes, then both backends must draw them the same way
    {
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() / "vsport_texrep_test";
        std::error_code ec;
        fs::remove_all(root, ec);
        Rng rng{11u};
        std::vector<u32> cmd;
        setup(cmd, rng);
        Opts o; o.semi = true; o.count = 250;
        random_prims(cmd, rng, o, nullptr);
        TextureReplacer dumper;
        dumper.set_dump_dir(root / "_dokum");
        {
            PsxGpu g;
            g.set_internal_scale(S);
            g.set_texture_replacer(&dumper);
            for (u32 w : cmd) g.write_gp0(w);
            g.flush_hr();
        }
        u32 made = 0;
        for (const auto& e : fs::directory_iterator(root / "_dokum", ec)) {
            TexImage src, big;
            if (!load_png(e.path(), src)) continue;
            big.w = src.w * 4; big.h = src.h * 4;
            big.rgba.resize(static_cast<std::size_t>(big.w) * big.h);
            for (u32 y = 0; y < big.h; ++y) {
                for (u32 x = 0; x < big.w; ++x) {
                    u32 c = src.rgba[static_cast<std::size_t>(y / 4) * src.w + x / 4];
                    if (((x ^ y) & 4u) != 0) c = (c & 0xFF000000u) | (~c & 0x00FFFFFFu); // Detail finer than a texel
                    if (((x * 7 + y * 3) % 23) == 0) c &= 0x00FFFFFFu;                   // Transparent specks
                    big.rgba[static_cast<std::size_t>(y) * big.w + x] = c;
                }
            }
            if (save_png(root / e.path().filename(), big)) ++made;
        }
        CHECK(dumper.stats.dumped > 0 && made == dumper.stats.dumped, "texture dump: " << dumper.stats.dumped << " dumped, " << made << " replaced");
        TextureReplacer pack;
        const std::size_t indexed = pack.scan(root);
        CHECK(indexed == made, "texture pack index: " << indexed << " of " << made);
        const u32 d = compare("doku degistirme paketi", warp.dev, cmd, TOL, [&](PsxGpu& g) { g.set_texture_replacer(&pack); });
        CHECK(d <= LIMIT, "texture replacement: " << d << " pixels differ");
        CHECK(pack.stats.hits > 0, "texture replacement: no hits");
        std::printf("    (%llu doku dokuldu, %llu isabet)\n", static_cast<unsigned long long>(dumper.stats.dumped),
                    static_cast<unsigned long long>(pack.stats.hits));
        fs::remove_all(root, ec);
    }

    // Frame interpolation: a triangle drawn at x = 40 in one frame and at x = 80 in the
    // next (double buffered, the second buffer 320 pixels to the right) must appear at x = 60 in
    // the in-between picture, shown for one vblank before the new frame
    {
        PsxGpu g;
        g.set_internal_scale(S);
        std::string err;
        auto b = make_d3d11_hr_backend(warp.dev, &err);
        CHECK(b != nullptr, "interpolation: D3D11 backend: " << err);
        if (b) {
            g.set_hr_backend(std::move(b));
            g.set_frame_interpolation(true);
            Rng rng{12u};
            std::vector<u32> init;
            setup(init, rng);
            for (u32 w : init) g.write_gp0(w);
            g.write_gp1(0x05000000u | (256u << 10)); // Displaying elsewhere before the first frame
            const auto frame = [&](s32 buf_x, s32 tri_x) {
                g.write_gp0(0xE3000000u | static_cast<u32>(buf_x));                       // Draw area of this buffer
                g.write_gp0(0xE4000000u | (239u << 10) | static_cast<u32>(buf_x + 319));
                g.write_gp0(0xE5000000u | static_cast<u32>(buf_x));                       // Offset = buffer
                g.write_gp0(0x02000000u);                                                 // Clear to black
                g.write_gp0(xy(buf_x, 0));
                g.write_gp0((240u << 16) | 320u);
                g.write_gp0(0x20FFFFFFu);                                                 // White triangle
                g.write_gp0(xy(tri_x, 50));
                g.write_gp0(xy(tri_x + 40, 50));
                g.write_gp0(xy(tri_x, 90));
                g.write_gp1(0x05000000u | static_cast<u32>(buf_x));                       // Show this buffer
            };
            frame(0, 40);
            g.on_vblank(); g.on_vblank();
            frame(320, 80); // The game draws in the other buffer
            const bool shown = g.showing_interpolated();
            CHECK(shown, "interpolation: no in-between picture after a 2-vblank frame");
            const std::vector<u32>* alt = g.hr_backend() ? g.hr_backend()->alt_picture() : nullptr;
            CHECK(alt != nullptr, "interpolation: no alternate picture");
            if (alt) {
                // Leftmost lit pixel of row 60 (native) inside the displayed buffer
                const u32 w = PsxGpu::VRAM_WIDTH * S;
                s32 left = -1;
                for (u32 x = 320 * S; x < 640 * S && left < 0; ++x) {
                    if (((*alt)[static_cast<std::size_t>(60 * S) * w + x] & 0xFFFFFFu) != 0) left = static_cast<s32>(x / S) - 320;
                }
                std::printf("  %-34s triangle at x %d (expected 60)\n", "interpolated (between frames)", left);
                CHECK(left >= 59 && left <= 61, "interpolation: triangle at x " << left << ", expected 60");
            }
            g.on_vblank();
            CHECK(g.showing_interpolated(), "interpolation: in-between picture must stay for the next presented vblank");
            g.on_vblank();
            CHECK(!g.showing_interpolated(), "interpolation: the new frame must follow");
        }
    }

    if (g_failures == 0) {
        std::printf("[PASS] Graphics card and CPU renderers give the same result.\n");
        return 0;
    }
    std::printf("[FAIL] %d failures\n", g_failures);
    return 1;
#else
    std::printf("The Direct3D 11 test only runs on Windows.\n");
    return 0;
#endif
}
