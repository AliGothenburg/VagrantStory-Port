#include "vsport/cdrom_iso.hpp"
#include "vsport/d3d11_renderer.hpp"
#include "vsport/psx_exe.hpp"
#include "vsport/runtime.hpp"
#include "vsport/win32_audio.hpp"

#ifdef VSPORT_HAS_AOT_MODULES
#include "vs_aot_registry.hpp"
#endif

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace vsport;

namespace {

// Renders an interactive 3D Rood Inverse / Crystal octahedron frame using REAL PS1 GTE (RTPS + NCLIP + AVSZ3)
// and DMA6 OTC + DMA2 Linked-List GPU packets inside PSX RAM!
void step_hardware_self_demo(PsxRuntime& rt, float angle_rad) {
    auto& gte = rt.gte();

    // Configure GPU display 320x240 & drawing clip area
    rt.write32(0x1F801814u, 0x08000001u); // GP1 Display Mode 320x240
    rt.write32(0x1F801814u, 0x05000000u); // GP1 Display Start (0,0)
    rt.write32(0x1F801810u, 0xE3000000u); // GP0 Clip Top-Left (0,0)
    rt.write32(0x1F801810u, 0xE403BD3Fu); // GP0 Clip Bottom-Right (319, 239)
    rt.write32(0x1F801810u, 0xE5000000u); // GP0 Offset (0,0)

    // Fast-fill background dark gothic burgundy/slate (Vagrant Story atmosphere)
    rt.write32(0x1F801810u, 0x021A1014u);
    rt.write32(0x1F801810u, 0x00000000u);
    rt.write32(0x1F801810u, 0x00F00140u); // 320x240

    // Clear 256-entry Ordering Table in PSX RAM at 0x80100000..0x801003FC via DMA6 (OTC)!
    constexpr u32 OT_BASE   = 0x00100000u;
    constexpr u32 OT_LENGTH = 256;
    rt.write32(0x1F8010E0u, OT_BASE + (OT_LENGTH - 1) * 4u); // D6_MADR = end of OT
    rt.write32(0x1F8010E4u, OT_LENGTH);                      // D6_BCR  = 256 entries
    rt.write32(0x1F8010E8u, 0x11000002u);                    // D6_CHCR = start OTC

    // Configure GTE Screen Offset (160.0, 120.0 in 16.16 fixed point) and Projection H = 220
    gte.write_ctrl(24, 160u << 16); // OFX
    gte.write_ctrl(25, 120u << 16); // OFY
    gte.write_ctrl(26, 220u);       // H
    gte.write_ctrl(29, 1365u);      // ZSF3 = 4096 / 3
    gte.write_ctrl(5, 0u);          // TRX = 0
    gte.write_ctrl(6, 0u);          // TRY = 0
    gte.write_ctrl(7, 320u);        // TRZ = 320 units away

    // Build Y*X rotation matrix in 4.12 fixed-point (4096 = 1.0)
    const float cy = std::cos(angle_rad), sy = std::sin(angle_rad);
    const float cx = std::cos(angle_rad * 0.6f), sx = std::sin(angle_rad * 0.6f);
    const auto fp12 = [](float v) -> u16 {
        return static_cast<u16>(static_cast<s16>(std::lround(v * 4096.0f)));
    };

    const u16 r11 = fp12(cy),       r12 = fp12(sy * sx), r13 = fp12(sy * cx);
    const u16 r21 = fp12(0.0f),     r22 = fp12(cx),      r23 = fp12(-sx);
    const u16 r31 = fp12(-sy),      r32 = fp12(cy * sx), r33 = fp12(cy * cx);

    gte.write_ctrl(0, static_cast<u32>(r11) | (static_cast<u32>(r12) << 16));
    gte.write_ctrl(1, static_cast<u32>(r13) | (static_cast<u32>(r21) << 16));
    gte.write_ctrl(2, static_cast<u32>(r22) | (static_cast<u32>(r23) << 16));
    gte.write_ctrl(3, static_cast<u32>(r31) | (static_cast<u32>(r32) << 16));
    gte.write_ctrl(4, r33);

    struct Vec3s { s16 x, y, z; };
    constexpr Vec3s VERTS[6] = {
        {   0, -95,   0}, // Top spire
        {   0,  95,   0}, // Bottom spire
        { -65,   0, -65},
        {  65,   0, -65},
        {  65,   0,  65},
        { -65,   0,  65}
    };
    struct Tri { int a, b, c; u32 col0, col1, col2; };
    constexpr Tri FACES[8] = {
        {0, 3, 2, 0x305090E0u, 0x00284080u, 0x003860A8u},
        {0, 4, 3, 0x3060B0F5u, 0x00305898u, 0x00284080u},
        {0, 5, 4, 0x305090E0u, 0x00203068u, 0x00305898u},
        {0, 2, 5, 0x304075C0u, 0x003860A8u, 0x00203068u},
        {1, 2, 3, 0x30253568u, 0x003860A8u, 0x00284080u},
        {1, 3, 4, 0x30304885u, 0x00284080u, 0x00305898u},
        {1, 4, 5, 0x30253568u, 0x00305898u, 0x00203068u},
        {1, 5, 2, 0x301C2850u, 0x00203068u, 0x003860A8u}
    };

    u32 pkt_cursor = 0x00110000u; // Packet buffer in PSX RAM
    for (const auto& f : FACES) {
        const auto pack_xy = [](const Vec3s& v) -> u32 {
            return static_cast<u32>(static_cast<u16>(v.x)) |
                   (static_cast<u32>(static_cast<u16>(v.y)) << 16);
        };
        gte.write_data(0, pack_xy(VERTS[f.a])); gte.write_data(1, static_cast<u16>(VERTS[f.a].z));
        gte.write_data(2, pack_xy(VERTS[f.b])); gte.write_data(3, static_cast<u16>(VERTS[f.b].z));
        gte.write_data(4, pack_xy(VERTS[f.c])); gte.write_data(5, static_cast<u16>(VERTS[f.c].z));

        // Execute GTE RTPT (0x4A280030: sf=1, RTPT=0x30) + NCLIP (0x4A000006) + AVSZ3 (0x4A00002D)
        gte.execute(0x4A280030u);
        gte.execute(0x4A000006u);
        const s32 mac0 = static_cast<s32>(gte.read_data(24));
        if (mac0 <= 0) continue; // Backface culled by GTE NCLIP!

        gte.execute(0x4A00002Du);
        const u32 otz = std::min<u32>(OT_LENGTH - 1u, gte.read_data(7) >> 2);

        // Build 6-word GP0 Gouraud Triangle (0x30) packet and link into Ordering Table
        const u32 ot_entry_addr = OT_BASE + otz * 4u;
        const u32 prev_link     = rt.read32(ot_entry_addr) & 0x00FFFFFFu;

        rt.write32(pkt_cursor + 0,  (6u << 24) | prev_link);
        rt.write32(pkt_cursor + 4,  f.col0);
        rt.write32(pkt_cursor + 8,  gte.read_data(12)); // SXY0
        rt.write32(pkt_cursor + 12, f.col1);
        rt.write32(pkt_cursor + 16, gte.read_data(13)); // SXY1
        rt.write32(pkt_cursor + 20, f.col2);
        rt.write32(pkt_cursor + 24, gte.read_data(14)); // SXY2

        rt.write32(ot_entry_addr, pkt_cursor & 0x00FFFFFFu);
        pkt_cursor += 28u;
    }

    // Kick DMA2 GPU Linked-List transfer starting from back of OT (OT_BASE + (OT_LENGTH - 1)*4)!
    rt.write32(0x1F8010A0u, OT_BASE + (OT_LENGTH - 1) * 4u);
    rt.write32(0x1F8010A4u, 0);
    rt.write32(0x1F8010A8u, 0x01000401u); // SyncMode=2 (Linked-List), FromRAM=1, Start=1
}

#ifdef _WIN32
struct Win32HostState {
    PsxRuntime* rt          = nullptr;
    CpuContext* ctx         = nullptr;
    D3D11Renderer* d3d      = nullptr;
    Win32AudioOut* audio    = nullptr;
    bool self_demo          = false;
    bool show_full_vram     = false;
    bool widescreen         = false;
    float demo_angle        = 0.0f;

    void update_window_title(HWND hwnd) const {
        char title[256]{};
        const char* backend = (d3d && d3d->is_initialized()) ? d3d->shader_mode_name() : "GDI Yazilim";
        const char* sound   = (audio && audio->is_open()) ? (audio->is_muted() ? "Sessiz" : "44.1kHz ADPCM") : "Kapali";
        std::snprintf(title, sizeof(title),
                      "Vagrant Story Native Port [%s | Ses: %s] — F2: VRAM | F3: 16:9 | F4: Shader | M: Ses | F12: Kaydet",
                      backend, sound);
        SetWindowTextA(hwnd, title);
    }
};

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* st = reinterpret_cast<Win32HostState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_SIZE:
            if (st && st->d3d && st->d3d->is_initialized() && wparam != SIZE_MINIMIZED) {
                st->d3d->resize(LOWORD(lparam), HIWORD(lparam));
            }
            break;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                PostQuitMessage(0);
                return 0;
            }
            if (st && wparam == VK_F2) {
                st->show_full_vram = !st->show_full_vram;
            } else if (st && wparam == VK_F3) {
                st->widescreen = !st->widescreen;
                st->rt->gte().set_widescreen_scale(st->widescreen ? 0.75f : 1.0f);
            } else if (st && wparam == VK_F4) {
                if (st->d3d && st->d3d->is_initialized()) {
                    st->d3d->cycle_shader_mode();
                    st->update_window_title(hwnd);
                }
            } else if (st && wparam == 'M') {
                if (st->audio) {
                    st->audio->toggle_mute();
                    st->update_window_title(hwnd);
                }
            } else if (st && wparam == VK_F12) {
                st->rt->gpu().dump_ppm("vram_snapshot.ppm", true);
                std::cout << "[vs_host] VRAM goruntusu kaydedildi: vram_snapshot.ppm\n";
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}
#endif

} // namespace

namespace vsport { void dump_halt_trace(); }
using vsport::dump_halt_trace;

int main(int argc, char** argv) {
    std::filesystem::path disc_path;
    std::filesystem::path dump_ppm_path;
    std::filesystem::path dump_disp_path;
    std::filesystem::path dump_wav_path;
    bool headless   = false;
    bool self_demo  = false;
    bool use_aot    = true;
    bool auto_start = false;
    bool sw_present = false;
    bool test_d3d11 = false;
    u64 max_cycles  = 2'000'000ull;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--disc" || arg == "-d") && i + 1 < argc) {
            disc_path = argv[++i];
        } else if (arg == "--headless") {
            headless = true;
        } else if (arg == "--self-demo") {
            self_demo = true;
        } else if (arg == "--no-aot") {
            use_aot = false;
        } else if (arg == "--auto-start") {
            auto_start = true;
        } else if (arg == "--software-present") {
            sw_present = true;
        } else if (arg == "--test-d3d11") {
            test_d3d11 = true;
        } else if (arg == "--cycles" && i + 1 < argc) {
            max_cycles = std::strtoull(argv[++i], nullptr, 0);
        } else if (arg == "--dump-vram" && i + 1 < argc) {
            dump_ppm_path = argv[++i];
        } else if (arg == "--dump-display" && i + 1 < argc) {
            dump_disp_path = argv[++i];
        } else if (arg == "--dump-wav" && i + 1 < argc) {
            dump_wav_path = argv[++i];
        }
    }

#ifdef _WIN32
    if (test_d3d11) {
        WNDCLASSA twc{};
        twc.lpfnWndProc   = DefWindowProcA;
        twc.hInstance     = GetModuleHandleA(nullptr);
        twc.lpszClassName = "VsPortD3D11SmokeWnd";
        RegisterClassA(&twc);
        HWND thwnd = CreateWindowExA(0, twc.lpszClassName, "D3D11Test", WS_OVERLAPPEDWINDOW,
                                     0, 0, 640, 480, nullptr, nullptr, twc.hInstance, nullptr);
        PsxRuntime trt;
        step_hardware_self_demo(trt, 0.4f);
        D3D11Renderer td3d;
        std::string terr;
        if (!td3d.initialize(thwnd, 640, 480, &terr)) {
            std::cerr << "[FAIL] D3D11 smoke test basarisiz: " << terr << "\n";
            DestroyWindow(thwnd);
            return 1;
        }
        for (int m = 0; m < 3; ++m) {
            td3d.present_vram(trt.gpu(), false, m == 1, false);
            td3d.cycle_shader_mode();
        }
        std::printf("[PASS] D3D11 + HLSL Shader Model 4.0 dogrulandi! GPU: %s\n", td3d.adapter_name().c_str());
        td3d.shutdown();
        DestroyWindow(thwnd);
        return 0;
    }
#endif

    if (disc_path.empty() && !self_demo) {
        std::cout << "Kullanim:\n"
                  << "  vs_host --disc <VagrantStory.cue|.bin|.iso> [--headless] [--no-aot] [--cycles N] [--dump-vram vram.ppm]\n"
                  << "  vs_host --self-demo [--headless] [--dump-vram demo.ppm]\n";
        return 1;
    }

    PsxRuntime rt(PSX_RAM_SIZE_2MB);
    CpuContext ctx{};
    DiscImage disc;

#ifdef VSPORT_HAS_AOT_MODULES
    if (use_aot && !self_demo) {
        const std::size_t aot_cnt = register_all_generated_modules(rt);
        std::printf("[vs_host] Native C++20 AOT aktif: %zu derlenmis fonksiyon kaydedildi.\n", aot_cnt);
        std::fflush(stdout);
    }
#else
    (void)use_aot;
#endif

    if (!disc_path.empty()) {
        std::string err;
        if (!disc.open(disc_path, &err)) {
            std::cerr << "[HATA] Disk acilamadi: " << err << "\n";
            return 1;
        }
        rt.attach_disc(&disc);

        const auto cnf = disc.parse_system_cnf();
        if (!cnf.has_value()) {
            std::cerr << "[HATA] SYSTEM.CNF bulunamadi.\n";
            return 1;
        }

        const auto exe_bytes = disc.extract_file(cnf->boot_path);
        PsxExeImage exe{};
        if (!parse_psx_exe(exe_bytes, exe, &err)) {
            std::cerr << "[HATA] PS-X EXE ayristirilamadi: " << err << "\n";
            return 1;
        }

        rt.load_exe(exe, ctx);
        std::printf("[vs_host] Yuklendi: %s (Entry PC=0x%08X, GP=0x%08X, SP=0x%08X)\n",
                    cnf->boot_path.c_str(), ctx.pc, ctx.get_reg(reg::GP), ctx.get_reg(reg::SP));
        std::fflush(stdout);
    }

    if (headless) {
        if (self_demo) {
            step_hardware_self_demo(rt, 0.55f);
        } else {
            if (auto_start) {
                rt.set_vblank_callback([&rt]() {
                    const u32 vb = rt.vblank_count();
                    const bool pulse = (vb >= 350u) && ((vb % 60u) >= 10u && (vb % 60u) <= 18u);
                    rt.set_pad_buttons(pulse ? static_cast<u16>(~(1u << 3)) : 0xFFFFu);
                });
            }
            const u64 executed = rt.run(ctx, max_cycles);
            std::printf("[vs_host] Cevrim: %llu | Son PC: 0x%08X | VBlank: %u | CD Sektor: %u (Son LBA: 0x%X) | GPU Primitif: %llu (%s %ux%u) | MDEC MB: %llu | SPU RAM: %llu B (KON: %llu, XA: %llu, Ses: %llu kare) | AOT Cagri: %llu\n",
                        static_cast<unsigned long long>(executed),
                        ctx.pc,
                        rt.vblank_count(),
                        rt.cdrom().sectors_read(),
                        rt.cdrom().last_read_lba(),
                        static_cast<unsigned long long>(rt.gpu().primitives_drawn()),
                        rt.gpu().is_24bit_display() ? "24-bit" : "15-bit",
                        rt.gpu().display_width(),
                        rt.gpu().display_height(),
                        static_cast<unsigned long long>(rt.mdec().macroblocks_decoded()),
                        static_cast<unsigned long long>(rt.spu().bytes_uploaded()),
                        static_cast<unsigned long long>(rt.spu().key_on_count()),
                        static_cast<unsigned long long>(rt.spu().xa_sectors_decoded()),
                        static_cast<unsigned long long>(rt.spu().frames_rendered()),
                        static_cast<unsigned long long>(rt.aot_dispatch_count()));
            if (ctx.halted) {
                std::printf("[vs_host] CPU durdu! PC=0x%08X\n", ctx.pc);
                for (u32 r = 1; r < 32; ++r) std::printf("r%u=%08X%s", r, ctx.get_reg(r), (r % 8 == 0) ? "\n" : " ");
                std::printf("\n");
                dump_halt_trace();
                for (u32 a = 0x801FFE60u; a < 0x801FFFF0u; a += 16u)
                    std::printf("[stack] %08X: %08X %08X %08X %08X\n", a, rt.read32(a), rt.read32(a + 4), rt.read32(a + 8), rt.read32(a + 12));
            }
            if (!rt.tty_output().empty()) {
                std::printf("--- BIOS TTY Ciktisi ---\n%s\n", rt.tty_output().c_str());
            }
            if (!rt.cdrom().debug_log().empty()) {
                std::printf("--- CD-ROM Komut Gunlugu ---\n%s", rt.cdrom().debug_log().c_str());
            }
        }

        if (!dump_ppm_path.empty()) {
            rt.gpu().dump_ppm(dump_ppm_path, !self_demo);
            std::printf("[vs_host] VRAM goruntusu disari aktarildi: %s\n", dump_ppm_path.string().c_str());
        }
        if (!dump_disp_path.empty()) {
            rt.gpu().dump_ppm(dump_disp_path, false);
            std::printf("[vs_host] Aktif ekran goruntusu disari aktarildi: %s\n", dump_disp_path.string().c_str());
        }
        if (!dump_wav_path.empty()) {
            rt.spu().dump_wav(dump_wav_path);
            std::printf("[vs_host] SPU 44.1 kHz Stereo WAV disari aktarildi: %s\n", dump_wav_path.string().c_str());
        }
        return 0;
    }

#ifdef _WIN32
    D3D11Renderer d3d;
    Win32AudioOut audio;
    Win32HostState host_state{&rt, &ctx, &d3d, &audio, self_demo, false, false, 0.0f};

    WNDCLASSA wc{};
    wc.lpfnWndProc   = HostWndProc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "VagrantStoryNativePortWnd";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);

    RECT wr{0, 0, 960, 720};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExA(
        0, wc.lpszClassName,
        "Vagrant Story Native Port",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, wc.hInstance, nullptr);

    SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&host_state));

    if (!sw_present) {
        std::string d3d_err;
        if (d3d.initialize(hwnd, 960, 720, &d3d_err)) {
            std::printf("[vs_host] Direct3D 11 aktif (%s) — Shader: %s\n",
                        d3d.adapter_name().c_str(), d3d.shader_mode_name());
        } else {
            std::printf("[vs_host] D3D11 baslatilamadi (%s), GDI yedegine geciliyor.\n", d3d_err.c_str());
        }
    }
    if (audio.initialize(PsxSpu::SAMPLE_RATE)) {
        std::printf("[vs_host] WinMM 44.1 kHz Stereo SPU ADPCM ses cikisi aktif.\n");
    }
    std::fflush(stdout);
    host_state.update_window_title(hwnd);

    std::vector<u32> frame_bgra;
    MSG msg{};
    bool running = true;

    auto present_and_pump = [&]() {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running    = false;
                ctx.halted = true;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!running) return;

        // Map PC keyboard to PS1 Digital Pad (active-low bits)
        u16 pad = 0xFFFFu;
        if (GetAsyncKeyState(VK_RETURN) & 0x8000) pad &= ~(1u << 3);  // START
        if (GetAsyncKeyState(VK_BACK)   & 0x8000) pad &= ~(1u << 0);  // SELECT
        if (GetAsyncKeyState(VK_UP)     & 0x8000) pad &= ~(1u << 4);  // UP
        if (GetAsyncKeyState(VK_RIGHT)  & 0x8000) pad &= ~(1u << 5);  // RIGHT
        if (GetAsyncKeyState(VK_DOWN)   & 0x8000) pad &= ~(1u << 6);  // DOWN
        if (GetAsyncKeyState(VK_LEFT)   & 0x8000) pad &= ~(1u << 7);  // LEFT
        if (GetAsyncKeyState('Z')       & 0x8000) pad &= ~(1u << 14); // CROSS (Confirm)
        if (GetAsyncKeyState('X')       & 0x8000) pad &= ~(1u << 13); // CIRCLE (Cancel)
        if (GetAsyncKeyState('A')       & 0x8000) pad &= ~(1u << 15); // SQUARE
        if (GetAsyncKeyState('S')       & 0x8000) pad &= ~(1u << 12); // TRIANGLE
        if (GetAsyncKeyState('Q')       & 0x8000) pad &= ~(1u << 10); // L1
        if (GetAsyncKeyState('W')       & 0x8000) pad &= ~(1u << 11); // R1
        rt.set_pad_buttons(pad);

        // Stream synthesized PS1 SPU 44.1 kHz Stereo PCM to Windows audio ring buffer
        audio.pump_from_spu(rt.spu());

        if (d3d.is_initialized()) {
            d3d.present_vram(rt.gpu(), host_state.show_full_vram, host_state.widescreen, true);
        } else {
            u32 fw = 0, fh = 0;
            rt.gpu().export_bgra8888(frame_bgra, fw, fh, host_state.show_full_vram);

            RECT cr{};
            GetClientRect(hwnd, &cr);
            const int win_w = cr.right - cr.left;
            const int win_h = cr.bottom - cr.top;

            if (win_w > 0 && win_h > 0 && fw > 0 && fh > 0) {
                BITMAPINFO bmi{};
                bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth       = static_cast<LONG>(fw);
                bmi.bmiHeader.biHeight      = -static_cast<LONG>(fh); // Top-down scanlines
                bmi.bmiHeader.biPlanes      = 1;
                bmi.bmiHeader.biBitCount    = 32;
                bmi.bmiHeader.biCompression = BI_RGB;

                HDC hdc = GetDC(hwnd);
                StretchDIBits(hdc, 0, 0, win_w, win_h,
                              0, 0, static_cast<int>(fw), static_cast<int>(fh),
                              frame_bgra.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
                ReleaseDC(hwnd, hdc);
            }
            Sleep(14);
        }
    };

    if (host_state.self_demo) {
        while (running) {
            host_state.demo_angle += 0.025f;
            step_hardware_self_demo(rt, host_state.demo_angle);
            present_and_pump();
        }
    } else {
        rt.set_vblank_callback(present_and_pump);
        while (running && !ctx.halted) {
            rt.run(ctx, 0x00FF'FFFF'FFFF'FFFFull);
        }
        while (running) {
            present_and_pump();
        }
    }
#endif
    return 0;
}
