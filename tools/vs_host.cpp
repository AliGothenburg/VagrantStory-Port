#include "vsport/cdrom_iso.hpp"
#include "vsport/cheats.hpp"
#include "vsport/free_camera.hpp"
#include "vsport/pad_mapping.hpp"
#include "vsport/psx_exe.hpp"
#include "vsport/runtime.hpp"
#include "launcher_win32.hpp"
#ifdef _WIN32
#include "vsport/d3d11_renderer.hpp"
#include "vsport/win32_audio.hpp"
#include "vsport/win32_input.hpp"
#include "vsport/d3d11_hr_backend.hpp"
#endif
#include "achievements.hpp"
#include "rc_consoles.h"
#include "rc_hash.h"

#ifdef VSPORT_HAS_AOT_MODULES
#include "vs_aot_registry.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h> // timeBeginPeriod / timeEndPeriod (winmm)

// Laptops with two graphics cards: ask the NVIDIA / AMD drivers for the dedicated one
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement                = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
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

#ifndef _WIN32
void host_log(const char*, ...) {} // Linux: vs_host is headless only (the game is sdl_host.cpp)
#endif

#ifdef _WIN32
// Start / shutdown steps of the windowed game, written to VagrantStory.log next to the exe, so a
// start or exit that hangs (no console in the GUI build) shows where it stopped
FILE* g_host_log = nullptr;
ULONGLONG g_host_log_t0 = 0;

void host_log(const char* fmt, ...) {
    if (!g_host_log) return;
    std::fprintf(g_host_log, "[%8.2f s] ", static_cast<double>(GetTickCount64() - g_host_log_t0) / 1000.0);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_host_log, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_host_log);
    std::fflush(g_host_log);
}

// Once quitting has been asked for, a process that has not ended within 8 s (a hang somewhere in
// shutdown) ends itself, so no windowless game process stays behind (it would also lock the exe)
void start_exit_watchdog() {
    static bool started = false;
    if (started) return;
    started = true;
    CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        Sleep(8000);
        host_log("Kapanis 8 saniyede bitmedi, surec zorla sonlandiriliyor");
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    }, nullptr, 0, nullptr));
}

struct Win32HostState {
    PsxRuntime* rt          = nullptr;
    CpuContext* ctx         = nullptr;
    D3D11Renderer* d3d      = nullptr;
    Win32AudioOut* audio    = nullptr;
    bool self_demo          = false;
    bool show_full_vram     = false;
    bool widescreen         = false; // Widen the 3D view to the picture aspect (GTE hack)
    int aspect              = 0;     // Index into launcher::ASPECT_VALUES
    float demo_angle        = 0.0f;

    // Picture width / height: the chosen preset, or the window itself in "fill" mode
    float picture_aspect(HWND hwnd) const {
        const float a = launcher::ASPECT_VALUES[std::clamp(aspect, 0, launcher::ASPECT_COUNT - 1)];
        if (a > 0.0f) return a;
        RECT cr{};
        GetClientRect(hwnd, &cr);
        return cr.bottom > 0 ? static_cast<float>(cr.right) / static_cast<float>(cr.bottom) : 4.0f / 3.0f;
    }
    // Windowed mode: make the client area exactly the picture's shape (keeps its height), so no
    // black bars appear around the picture
    void fit_window_to_aspect(HWND hwnd) const {
        const float a = launcher::ASPECT_VALUES[std::clamp(aspect, 0, launcher::ASPECT_COUNT - 1)];
        if (fullscreen || a <= 0.0f || IsZoomed(hwnd) || IsIconic(hwnd)) return;
        RECT cr{};
        GetClientRect(hwnd, &cr);
        RECT wr{0, 0, static_cast<LONG>(static_cast<float>(cr.bottom) * a + 0.5f), cr.bottom};
        AdjustWindowRect(&wr, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE);
        SetWindowPos(hwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    // Hor+ widening: shrink GTE X so a wider picture shows more of the scene instead of stretching it
    void apply_gte_widening(HWND hwnd) const {
        const float k = widescreen ? std::clamp((4.0f / 3.0f) / picture_aspect(hwnd), 0.25f, 1.0f) : 1.0f;
        if (rt && rt->gte().widescreen_scale() != k) rt->gte().set_widescreen_scale(k);
        if (rt && rt->gpu().hud_squash() != k) rt->gpu().set_hud_squash(k); // HUD keeps its shape
    }
    bool fullscreen         = false;
    bool hidden             = false; // --hidden test window: never shown, not even in fullscreen

    // Save states (F5 save, F7 load, F6 next slot, hold F8 rewind): requests set here are carried
    // out by the main loop once run() has returned (see PsxRuntime::request_break)
    enum StateOp { OP_NONE, OP_SAVE, OP_LOAD };
    StateOp state_op        = OP_NONE;
    int state_slot          = 1;     // 1..5
    std::string notice;              // Short message shown in the window title
    ULONGLONG notice_until  = 0;

    // Shown in the window title and over the picture (so it is also seen in fullscreen)
    void show_notice(HWND hwnd, const std::string& text, ULONGLONG duration_ms = 2500) {
        notice       = text;
        notice_until = GetTickCount64() + duration_ms;
        update_window_title(hwnd);
        if (d3d && d3d->is_initialized()) {
            std::wstring w(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0)), L'\0');
            if (!w.empty()) {
                MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, w.data(), static_cast<int>(w.size()));
                w.pop_back(); // Terminating zero
            }
            d3d->set_overlay_text(w);
        }
    }
    WINDOWPLACEMENT windowed_placement{sizeof(WINDOWPLACEMENT)};

    // Borderless fullscreen on the window's current monitor (F11 / Alt+Enter)
    void toggle_fullscreen(HWND hwnd) {
        if (!fullscreen) {
            MONITORINFO mi{sizeof(MONITORINFO)};
            if (!GetWindowPlacement(hwnd, &windowed_placement) ||
                !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
                return;
            }
            SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | (hidden ? 0 : WS_VISIBLE)); // Test window stays hidden
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            if (!hidden) ShowCursor(FALSE);
            fullscreen = true;
        } else {
            SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | (hidden ? 0 : WS_VISIBLE));
            SetWindowPlacement(hwnd, &windowed_placement);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            if (!hidden) ShowCursor(TRUE);
            fullscreen = false;
        }
    }

    void update_window_title(HWND hwnd) const {
        char title[256]{};
        const char* backend = (d3d && d3d->is_initialized()) ? d3d->shader_mode_name() : "GDI Yazilim";
        const char* sound   = (audio && audio->is_open()) ? (audio->is_muted() ? "Sessiz" : "44.1kHz ADPCM") : "Kapali";
        if (!notice.empty() && GetTickCount64() < notice_until) {
            const std::string t = "Vagrant Story - " + notice; // UTF-8 (achievement names, Turkish letters)
            std::wstring w(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, t.c_str(), -1, nullptr, 0)), L'\0');
            if (!w.empty()) MultiByteToWideChar(CP_UTF8, 0, t.c_str(), -1, w.data(), static_cast<int>(w.size()));
            SetWindowTextW(hwnd, w.c_str());
            return;
        }
        std::snprintf(title, sizeof(title),
                      "Vagrant Story Native Port [%s | %s%s | Ses: %s] - F11: Tam ekran | F3: Oran | Shift+F3: 3D genislet | F4: Shader | M: Ses | Tab: Hizli",
                      backend, launcher::ASPECT_NAMES[std::clamp(aspect, 0, launcher::ASPECT_COUNT - 1)],
                      widescreen ? " genis 3D" : "", sound);
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
        case WM_SIZING: {
            // Dragging a window edge keeps the chosen aspect ratio (not in "fill" mode)
            const float a = st ? launcher::ASPECT_VALUES[std::clamp(st->aspect, 0, launcher::ASPECT_COUNT - 1)] : 0.0f;
            if (a <= 0.0f) break;
            auto* r = reinterpret_cast<RECT*>(lparam);
            RECT frame{0, 0, 0, 0};
            AdjustWindowRect(&frame, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE);
            const int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
            const int cw = (r->right - r->left) - fw, ch = (r->bottom - r->top) - fh;
            if (wparam == WMSZ_LEFT || wparam == WMSZ_RIGHT) {
                r->bottom = r->top + static_cast<LONG>(static_cast<float>(cw) / a + 0.5f) + fh; // Width drives
            } else {
                const LONG w = static_cast<LONG>(static_cast<float>(ch) * a + 0.5f) + fw;         // Height drives
                if (wparam == WMSZ_TOPLEFT || wparam == WMSZ_BOTTOMLEFT) r->left = r->right - w;
                else r->right = r->left + w;
            }
            return TRUE;
        }
        case WM_SYSKEYDOWN:
            // Alt+Enter: toggle fullscreen (bit 29 = Alt held)
            if (st && wparam == VK_RETURN && (lparam & (1 << 29)) != 0) {
                st->toggle_fullscreen(hwnd);
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                if (st && st->fullscreen) {
                    st->toggle_fullscreen(hwnd); // Leave fullscreen first so the dialog is visible
                }
                const int answer = MessageBoxW(hwnd,
                    L"Oyundan çıkmak istiyor musunuz?\n\n"
                    L"Oyunda kaydetmediğiniz ilerleme kaybolur.",
                    L"Vagrant Story", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
                if (answer == IDYES) {
                    host_log("Esc: cikis onaylandi");
                    PostQuitMessage(0);
                }
                return 0;
            }
            if (st && wparam == VK_F11) {
                st->toggle_fullscreen(hwnd);
            } else if (st && wparam == VK_F2) {
                st->show_full_vram = !st->show_full_vram;
            } else if (st && wparam == VK_F3) {
                if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
                    st->widescreen = !st->widescreen;            // Shift+F3: widen 3D on/off
                } else {
                    st->aspect = (st->aspect + 1) % launcher::ASPECT_COUNT; // F3: next aspect ratio
                }
                st->fit_window_to_aspect(hwnd);
                st->apply_gte_widening(hwnd);
                st->update_window_title(hwnd);
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
            } else if (st && wparam == VK_F5) {
                st->state_op = Win32HostState::OP_SAVE;
                st->rt->request_break();
            } else if (st && wparam == VK_F7) {
                st->state_op = Win32HostState::OP_LOAD;
                st->rt->request_break();
            } else if (st && wparam == VK_F6) {
                st->state_slot = st->state_slot % 5 + 1;
                st->show_notice(hwnd, "Durum yuvasi " + std::to_string(st->state_slot));
            } else if (st && wparam == VK_F12) {
                st->rt->gpu().dump_ppm("vram_snapshot.ppm", true);
                std::cout << "[vs_host] VRAM goruntusu kaydedildi: vram_snapshot.ppm\n";
            }
            break;
        case WM_CLOSE:
            host_log("Pencere kapatiliyor (WM_CLOSE)");
            break; // DefWindowProc destroys the window
        case WM_DESTROY:
            host_log("Pencere yok edildi, cikis istendi");
            start_exit_watchdog();
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
#if defined(_WIN32) && defined(VSPORT_GUI_SUBSYSTEM)
    // VagrantStory.exe is a GUI-subsystem build (no console window on double-click). When it is
    // started from a terminal, write the log there instead.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
#endif
#ifdef _WIN32
    // Crisp settings window and exact pixel sizes on high-DPI displays (no bitmap stretching)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
    std::filesystem::path disc_path;
    std::filesystem::path sbi_path;
    std::filesystem::path memcard_path;
    std::filesystem::path dump_ppm_path;
    std::filesystem::path dump_disp_path;
    std::filesystem::path dump_wav_path;
    std::filesystem::path dump_ram_path;
    bool headless   = false;
    bool self_demo  = false;
    bool use_aot    = true;
    bool auto_start = false;
    bool sw_present = false;
    bool test_d3d11 = false;
    float widen_aspect = 0.0f; // --widen A (0 = off)
    int rim_mode = -1;         // --rim N (-1 = from settings / default)
    bool perf = false;         // --perf
    int gpu_hr = -1;           // --gpu-hr (1) / --cpu-hr (0); -1 = from settings (window) or CPU (headless)
    std::filesystem::path cheats_path; // --cheats FILE
    std::filesystem::path save_state_path, load_state_path; // --save-state FILE N / --load-state FILE
    u32 save_state_vblank = 0, until_vblank = 0, break_every = 0;
    int overclock = -1;       // --overclock PERCENT (-1 = from settings / 100)
    int rumble_flag = -1;      // --rumble 0/1: DualShock + vibration (-1 = from settings; headless default off)
    std::string adapter_name;  // --adapter NAME: graphics card (empty = settings / automatic)
    bool have_adapter = false;
    bool hidden_window = false; // --hidden
    int aspect_flag = -1;       // --aspect N: picture aspect index (4 = 16:9), -1 = from settings
    int widescreen_flag = -1;   // --widescreen 0/1: widen the 3D view (Shift+F3), -1 = from settings
    bool no_throttle = false;   // --no-throttle
    std::string window_dump_path; // --window-dump FILE N
    u32 window_dump_frame = 0;
    int pgxp_mode = -1;       // --pgxp N (-1 = from settings / default 2)
    std::filesystem::path series_dir; // --dump-series DIR N
    u32 series_every = 0;
    u32 series_from  = 0;
    u64 max_cycles  = 2'000'000ull;
    u32 internal_scale = 0; // --scale N (0 = from settings / native)
    std::filesystem::path texdump_dir, texpack_dir; // --texdump DIR / --texpack DIR
    int interp_flag = -1;                           // --interp 0/1: frame interpolation (-1 = from settings)
    int runahead_flag = -1;                         // --runahead N: vblanks run ahead (-1 = from settings)
    int pal60_flag = -1;                            // --pal60 0/1: PAL disc at 60 Hz (-1 = from settings)
    std::filesystem::path interp_dump_dir;          // --dump-interp DIR FROM: pictures around vblank FROM (headless)
    u32 interp_dump_from = 0;
    int reverb_flag = -1;   // --reverb 0/1 (-1 = from settings / on)
    bool cubic_flag = false; // --cubic
    bool depth_flag = false;
    int texture_filter = -1; // --texfilter (1) / --texfilter-sharp (2); -1 = from settings / off
    // Scripted headless input: --press BUTTON@VBLANK[:FRAMES][,...] (e.g. START@560,CROSS@900:4)
    // RS_LEFT / RS_RIGHT / RS_UP / RS_DOWN push the right stick fully (camera tests)
    struct ScriptedPress { u16 bits; u32 at_vblank; u32 frames; float sx = 0.0f, sy = 0.0f; };
    std::vector<ScriptedPress> presses;
    const auto parse_presses = [&](const std::string& spec) -> bool {
        struct Name { const char* n; u16 bit; };
        constexpr Name NAMES[] = {
            {"SELECT", pad::SELECT}, {"START", pad::START}, {"UP", pad::UP}, {"RIGHT", pad::RIGHT},
            {"DOWN", pad::DOWN}, {"LEFT", pad::LEFT}, {"L2", pad::L2}, {"R2", pad::R2},
            {"L1", pad::L1}, {"R1", pad::R1}, {"TRIANGLE", pad::TRIANGLE}, {"CIRCLE", pad::CIRCLE},
            {"CROSS", pad::CROSS}, {"SQUARE", pad::SQUARE},
        };
        struct Stick { const char* n; float x, y; };
        constexpr Stick STICKS[] = {{"RS_LEFT", -1.0f, 0.0f}, {"RS_RIGHT", 1.0f, 0.0f}, {"RS_UP", 0.0f, -1.0f}, {"RS_DOWN", 0.0f, 1.0f}};
        std::size_t pos = 0;
        while (pos < spec.size()) {
            const std::size_t end  = std::min(spec.find(',', pos), spec.size());
            const std::string item = spec.substr(pos, end - pos);
            const std::size_t at   = item.find('@');
            if (at == std::string::npos) return false;
            const std::string name = item.substr(0, at);
            u16 bit = 0;
            for (const auto& n : NAMES) {
                if (name == n.n) bit = n.bit;
            }
            const Stick* stick = nullptr;
            for (const auto& s : STICKS) {
                if (name == s.n) stick = &s;
            }
            if (bit == 0 && stick == nullptr) return false;
            char* rest = nullptr;
            const u32 vb = static_cast<u32>(std::strtoul(item.c_str() + at + 1, &rest, 10));
            const u32 frames = (rest && *rest == ':') ? static_cast<u32>(std::strtoul(rest + 1, nullptr, 10)) : 8u;
            presses.push_back({bit, vb, frames, stick ? stick->x : 0.0f, stick ? stick->y : 0.0f});
            pos = end + 1;
        }
        return true;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--disc" || arg == "-d") && i + 1 < argc) {
            disc_path = argv[++i];
        } else if (arg == "--sbi" && i + 1 < argc) {
            sbi_path = argv[++i];
        } else if (arg == "--memcard" && i + 1 < argc) {
            memcard_path = argv[++i];
        } else if (arg == "--scale" && i + 1 < argc) {
            internal_scale = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--texfilter") {
            texture_filter = 1;
        } else if (arg == "--ra-hash" && i + 1 < argc) {
            // Diagnostics: the RetroAchievements hash of a disc image (no login needed)
            char hash[33] = {};
            rc_hash_init_default_cdreader();
            const int ok = rc_hash_generate_from_file(hash, RC_CONSOLE_PLAYSTATION, argv[++i]);
            std::printf("[vs_host] RetroAchievements hash: %s\n", ok ? hash : "(hesaplanamadi)");
            return ok ? 0 : 1;
        } else if (arg == "--pal60" && i + 1 < argc) {
            pal60_flag = std::atoi(argv[++i]) != 0 ? 1 : 0;
        } else if (arg == "--runahead" && i + 1 < argc) {
            runahead_flag = std::clamp(std::atoi(argv[++i]), 0, 3);
        } else if (arg == "--interp" && i + 1 < argc) {
            interp_flag = std::atoi(argv[++i]) != 0 ? 1 : 0;
        } else if (arg == "--dump-interp" && i + 1 < argc) {
            interp_dump_dir = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') interp_dump_from = static_cast<u32>(std::atoi(argv[++i]));
            std::error_code ec;
            std::filesystem::create_directories(interp_dump_dir, ec);
        } else if (arg == "--texdump" && i + 1 < argc) {
            texdump_dir = argv[++i];
        } else if (arg == "--texpack" && i + 1 < argc) {
            texpack_dir = argv[++i];
        } else if (arg == "--reverb" && i + 1 < argc) {
            reverb_flag = std::atoi(argv[++i]) != 0 ? 1 : 0;
        } else if (arg == "--cubic") {
            cubic_flag = true;
        } else if (arg == "--depth") {
            depth_flag = true; // Experimental depth buffer (see PsxGpu::set_depth_buffer)
        } else if (arg == "--texfilter-sharp") {
            texture_filter = 2;
        } else if (arg == "--hidden") {
            hidden_window = true; // Tests: invisible window, no real input
        } else if (arg == "--aspect" && i + 1 < argc) {
            aspect_flag = std::atoi(argv[++i]);
        } else if (arg == "--widescreen" && i + 1 < argc) {
            widescreen_flag = std::atoi(argv[++i]) != 0 ? 1 : 0;
        } else if (arg == "--no-throttle") {
            no_throttle = true;   // Tests: run as fast as possible instead of real time
        } else if (arg == "--window-dump" && i + 2 < argc) {
            window_dump_path  = argv[++i]; // Tests: save the window picture at frame N, then quit
            window_dump_frame = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--gpu-hr") {
            gpu_hr = 1; // Draw the upscaled picture on the graphics card (Direct3D 11)
        } else if (arg == "--adapter" && i + 1 < argc) {
            adapter_name = argv[++i]; // Graphics card by name ("" = automatic, the most powerful)
            have_adapter = true;
        } else if (arg == "--settings-dump" && i + 1 < argc) {
            // Diagnostics: save each page of the settings window (opened off-screen) as BMP, then quit
            std::filesystem::path dir = argv[++i];
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            launcher::dump_settings_window(launcher::load_settings(launcher::settings_path()), dir);
            return 0;
        } else if (arg == "--save-state" && i + 2 < argc) {
            save_state_path   = argv[++i]; // Headless: save the machine state at VBlank N
            save_state_vblank = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--cheats" && i + 1 < argc) {
            cheats_path = argv[++i]; // Cheat code file (see vsport/cheats.hpp)
        } else if (arg == "--load-state" && i + 1 < argc) {
            load_state_path = argv[++i];   // Headless: start from a saved state
        } else if (arg == "--until-vblank" && i + 1 < argc) {
            until_vblank = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10)); // Headless: stop at VBlank N
        } else if (arg == "--break-every" && i + 1 < argc) {
            break_every = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));  // Diagnostics
        } else if (arg == "--overclock" && i + 1 < argc) {
            overclock = std::atoi(argv[++i]);
        } else if (arg == "--rumble" && i + 1 < argc) {
            rumble_flag = std::atoi(argv[++i]) != 0 ? 1 : 0;
        } else if (arg == "--cpu-hr") {
            gpu_hr = 0; // Draw the upscaled picture on the processor (software)
        } else if (arg == "--perf") {
            perf = true; // Headless: print speed every 250 frames
        } else if (arg == "--pgxp" && i + 1 < argc) {
            // Vertex precision in the upscaled picture: 0 = off, 1 = no wobble, 2 = + perspective textures
            pgxp_mode = std::clamp(std::atoi(argv[++i]), 0, 2);
        } else if (arg == "--rim" && i + 1 < argc) {
            // Character rim light: 0 = soft, 1 = sharp, 2 = off
            rim_mode = std::clamp(std::atoi(argv[++i]), 0, 2);
        } else if (arg == "--widen" && i + 1 < argc) {
            // Headless diagnostics: widen the 3D view to this aspect ratio (e.g. 1.7778 for 16:9)
            const float a = std::strtof(argv[++i], nullptr);
            if (a > 0.0f) widen_aspect = a;
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
        } else if (arg == "--dump-series" && i + 2 < argc) {
            series_dir   = argv[++i];
            series_every = static_cast<u32>(std::max(1, std::atoi(argv[++i])));
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                series_from = static_cast<u32>(std::atoi(argv[++i])); // Optional first frame
            }
            std::error_code ec;
            std::filesystem::create_directories(series_dir, ec);
        } else if (arg == "--dump-wav" && i + 1 < argc) {
            dump_wav_path = argv[++i];
        } else if (arg == "--dump-ram" && i + 1 < argc) {
            dump_ram_path = argv[++i];
        } else if (arg == "--press" && i + 1 < argc) {
            if (!parse_presses(argv[++i])) {
                std::cerr << "[HATA] --press bicimi: BUTON@VBLANK[:KARE][,...] (or. START@560,CROSS@900:4)\n";
                return 1;
            }
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
            td3d.present_vram(trt.gpu(), false, m == 1 ? 16.0f / 9.0f : (m == 2 ? 0.0f : 4.0f / 3.0f), false);
            td3d.cycle_shader_mode();
        }
        std::printf("[PASS] D3D11 + HLSL Shader Model 4.0 dogrulandi! GPU: %s\n", td3d.adapter_name().c_str());
        td3d.shutdown();
        DestroyWindow(thwnd);
        return 0;
    }
#endif

    // Errors go to the log and, in windowed mode, to a message box (no console on double-click)
    const auto fail = [&](const std::string& msg) {
        std::cerr << "[HATA] " << msg << "\n";
#ifdef _WIN32
        if (!headless) {
            launcher::show_message(msg + "\n\nAyarlar penceresini acmak icin oyunu Shift tusuna basili tutarak baslatin.", true);
        }
#endif
        return 1;
    };

    // Settings from VagrantStory.ini, used when the game is started without --disc (double-click)
    launcher::GameSettings settings;
    bool use_settings = false;

#ifdef _WIN32
    if (!headless) {
        // Windowed game: log start / shutdown steps to VagrantStory.log next to the exe
        g_host_log_t0 = GetTickCount64();
        _wfopen_s(&g_host_log, (launcher::settings_path().parent_path() / L"VagrantStory.log").c_str(), L"w");
        SYSTEMTIME now{};
        GetLocalTime(&now);
        host_log("Baslatildi %04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    }
#endif

    if (disc_path.empty() && !self_demo) {
#ifdef _WIN32
        if (!headless) {
            settings = launcher::load_settings(launcher::settings_path());
            std::error_code ec;
            const bool disc_ok = !settings.disc.empty() && std::filesystem::exists(settings.disc, ec);
            const bool shift   = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            if (settings.show_launcher || shift || !disc_ok) {
                host_log("Ayarlar penceresi aciliyor");
                if (!launcher::run_settings_window(settings)) {
                    host_log("Ayarlar penceresi kapatildi, oyun baslatilmadi");
                    return 0; // Player closed the settings window
                }
                host_log("Ayarlar penceresi: Oyna");
            }
            disc_path    = settings.disc;
            sbi_path     = settings.sbi;
            use_settings = true;
        }
#endif
        if (disc_path.empty()) {
            std::cout << "Kullanim:\n"
                      << "  vs_host --disc <VagrantStory.cue|.bin|.iso> [--sbi altkanal.sbi] [--headless] [--no-aot] [--cycles N] [--dump-vram vram.ppm]\n"
                      << "  vs_host --self-demo [--headless] [--dump-vram demo.ppm]\n"
                      << "  vs_host            (ayarlar penceresi; secimler VagrantStory.ini'ye kaydedilir)\n";
            return 1;
        }
    }

    PsxRuntime rt(PSX_RAM_SIZE_2MB);
    CpuContext ctx{};
    DiscImage disc;
    if (widen_aspect > 0.0f) rt.gte().set_widescreen_scale(std::min(1.0f, (4.0f / 3.0f) / widen_aspect));
    if (internal_scale == 0) {
        internal_scale = use_settings ? static_cast<u32>(settings.internal_scale) : 1u;
    }
    rt.gpu().set_internal_scale(internal_scale);
    rt.set_pal60(pal60_flag >= 0 ? pal60_flag == 1 : (use_settings && settings.pal60));
    rt.set_cpu_overclock(static_cast<u32>(overclock >= 0 ? overclock : (use_settings ? settings.cpu_overclock : 100)));
    // Cheat codes: --cheats FILE, or the file next to the exe when switched on in the settings
    CheatList cheats;
#ifdef _WIN32
    if (cheats_path.empty() && use_settings && settings.cheats) cheats_path = launcher::cheats_path();
#endif
    if (!cheats_path.empty()) {
        std::ifstream in(cheats_path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<std::string> problems;
        cheats.parse(text, &problems);
        std::printf("[vs_host] Hile kodlari: %zu hile acik (%zu tanimli)\n", cheats.enabled_count(), cheats.cheats().size());
        for (const std::string& p : problems) std::printf("[vs_host] Hile dosyasi, %s\n", p.c_str());
#ifdef _WIN32
        host_log("Hile kodlari: %zu acik, %zu tanimli, %zu sorunlu satir", cheats.enabled_count(), cheats.cheats().size(), problems.size());
#endif
    }
    // Controller type: DualShock (vibration) unless switched off
    const bool rumble_on = rumble_flag >= 0 ? rumble_flag == 1 : (use_settings ? settings.rumble : false);
    rt.set_pad_dualshock(rumble_on);
    const bool free_cam_on = use_settings ? settings.free_camera : true;
    FreeCamera free_cam;
#ifdef _WIN32
    host_log("Serbest kamera: %s", free_cam_on ? "acik (sag cubuk, J/L/I/K)" : "kapali");
#endif
    bool cam_log_pushed = false; // Game log of right-stick pushes (see the vblank callback)
    FreeCamera::Status cam_log_status = FreeCamera::Status::Idle;
    int cam_log_lines = 0;
#ifdef _WIN32
    // Graphics card: --adapter, else the settings, else automatic (the most powerful one)
    set_preferred_adapter(have_adapter ? adapter_name : (use_settings ? settings.gpu : std::string()));
#else
    (void)have_adapter; (void)sw_present; (void)hidden_window; (void)no_throttle;
#endif
    // Upscaled picture on the graphics card; on any failure the processor (software) keeps drawing
    const auto use_gpu_hr = [&rt](void* device) {
#ifndef _WIN32
        (void)device;
        std::printf("[vs_host] Ekran karti cizimi bu sistemde yok, islemci kullaniliyor\n");
        (void)rt;
        return;
#else
        std::string err;
        auto b = make_d3d11_hr_backend(device, &err);
        if (b) {
            rt.gpu().set_hr_backend(std::move(b));
            if (rt.gpu().hr_backend() && rt.gpu().hr_backend()->gpu_picture_view()) {
                std::printf("[vs_host] Yuksek cozunurluk ekran kartinda ciziliyor (Direct3D 11)\n");
                return;
            }
            err = "hedef doku olusturulamadi";
            rt.gpu().set_hr_backend(make_software_hr_backend());
        }
        std::printf("[vs_host] Ekran karti cizimi baslatilamadi (%s), islemci kullaniliyor\n", err.c_str());
#endif
    };
    if (gpu_hr == 1 && headless && internal_scale > 1u) {
        use_gpu_hr(nullptr); // Headless: an own Direct3D 11 device
    }
    if (texture_filter < 0) texture_filter = use_settings ? settings.texture_filter : 0;
    rt.gpu().set_texture_filtering(texture_filter != 0);
    rt.gpu().set_texture_filter_sharp(texture_filter == 2);
    rt.gpu().set_depth_buffer(depth_flag);
    rt.spu().set_reverb_enabled(reverb_flag >= 0 ? reverb_flag == 1 : (use_settings ? settings.reverb : true));
    rt.spu().set_cubic_interpolation(cubic_flag || (use_settings && settings.audio_cubic));
    TextureReplacer texrep;
    if (texpack_dir.empty() && use_settings && settings.texture_pack) texpack_dir = launcher::texture_pack_dir();
    if (texdump_dir.empty() && use_settings && settings.texture_dump) texdump_dir = launcher::texture_pack_dir() / "_dokum";
    if (!texpack_dir.empty()) {
        const std::size_t n = texrep.scan(texpack_dir);
        std::printf("[vs_host] Doku paketi: %zu degistirme dokusu (%s)\n", n, texpack_dir.string().c_str());
        if (n) host_log("Doku paketi: %zu degistirme dokusu", n);
    }
    if (!texdump_dir.empty()) texrep.set_dump_dir(texdump_dir);
    if (texrep.active()) rt.gpu().set_texture_replacer(&texrep);
    rt.gpu().set_frame_interpolation(interp_flag >= 0 ? interp_flag == 1 : (use_settings && settings.frame_interp));
    if (pgxp_mode < 0) pgxp_mode = use_settings ? settings.pgxp : 2;
    rt.pgxp().set_enabled(pgxp_mode > 0 && rt.gpu().internal_scale() > 1u);
    rt.gpu().set_perspective_textures(pgxp_mode >= 2);
    if (rim_mode >= 0) {
        rt.gpu().set_rim_light(static_cast<PsxGpu::RimLight>(rim_mode));
    } else if (use_settings) {
        rt.gpu().set_rim_light(static_cast<PsxGpu::RimLight>(std::clamp(settings.rim_light, 0, 2)));
    }

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
            return fail("Disk acilamadi: " + err);
        }
        if (!sbi_path.empty() && !disc.load_sbi(sbi_path, &err)) {
            return fail(err);
        }
        if (disc.subq_override_count() > 0) {
            std::printf("[vs_host] Alt kanal verisi yuklendi: %s (%zu sektor)\n",
                        disc.sbi_path().string().c_str(), disc.subq_override_count());
        } else if (disc.volume_id() == "SLES-02754") {
            std::printf("[vs_host] UYARI: Bu PAL disk LibCrypt korumali; diskin .sbi alt kanal dosyasi olmadan\n"
                        "          oyun New Game'den sonra 'NOW LOADING'de kalir. .sbi dosyasini .cue ile ayni\n"
                        "          klasore ayni adla koyun ya da --sbi <dosya> ile belirtin.\n");
        }
        std::fflush(stdout);
        rt.attach_disc(&disc);

        // Memory card 1: --memcard, else the settings / default saves\VagrantStory_kart1.mcr
        if (memcard_path.empty()) {
            memcard_path = (use_settings && !settings.memcard.empty()) ? settings.memcard
                                                                       : launcher::default_memcard_path();
        }
        if (rt.attach_memory_card(0, memcard_path, &err)) {
            std::printf("[vs_host] Hafiza karti 1: %s (%u blok dolu)\n", memcard_path.string().c_str(),
                        rt.memory_card(0).used_blocks());
        } else {
            std::fprintf(stderr, "[UYARI] Hafiza karti acilamadi, kayit yapilamayacak: %s\n", err.c_str());
#ifdef _WIN32
            if (!headless) launcher::show_message("Hafiza karti acilamadi, oyunda kayit yapilamayacak:\n" + err, true);
#endif
        }

        const auto cnf = disc.parse_system_cnf();
        if (!cnf.has_value()) {
            return fail("Diskte SYSTEM.CNF bulunamadi; bu bir PlayStation oyun diski degil gibi gorunuyor.");
        }

        const auto exe_bytes = disc.extract_file(cnf->boot_path);
        PsxExeImage exe{};
        if (!parse_psx_exe(exe_bytes, exe, &err)) {
            return fail("PS-X EXE ayristirilamadi: " + err);
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
            bool save_pending = false;
            if (!load_state_path.empty()) {
                std::ifstream in(load_state_path, std::ios::binary);
                std::vector<u8> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                const bool ok = rt.load_state(std::move(data), ctx);
                std::printf("[vs_host] Durum yukleme: %s (kare %u)\n", ok ? "tamam" : "HATA", rt.vblank_count());
            }
            // --dump-wav in headless mode records the whole run (the SPU itself keeps only ~60 ms)
            std::vector<s16> wav_all;
            const bool record_wav = !dump_wav_path.empty();
            {
                rt.set_vblank_callback([&rt, &ctx, &presses, auto_start, &series_dir, series_every, series_from, perf, rumble_flag, &cheats,
                                        save_state_vblank, until_vblank, break_every, &save_pending, record_wav, &wav_all,
                                        &interp_dump_dir, interp_dump_from]() {
                    const u32 vb = rt.vblank_count();
                    if (!interp_dump_dir.empty() && vb >= interp_dump_from && vb < interp_dump_from + 40u) {
                        // Diagnostics: what would be presented at each vblank (m = main, i = in-between)
                        char name[32];
                        rt.gpu().flush_hr();
                        if (rt.gpu().showing_interpolated()) {
                            std::snprintf(name, sizeof(name), "v%06u_i.ppm", vb);
                            rt.gpu().dump_interpolated_ppm(interp_dump_dir / name);
                        } else {
                            std::snprintf(name, sizeof(name), "v%06u_m.ppm", vb);
                            rt.gpu().dump_ppm(interp_dump_dir / name, false);
                        }
                    }
                    if (record_wav) {
                        std::vector<s16> chunk;
                        rt.spu().pop_samples(chunk, 1u << 20);
                        wav_all.insert(wav_all.end(), chunk.begin(), chunk.end());
                    }
                    if (save_state_vblank != 0 && vb == save_state_vblank) {
                        save_pending = true;
                        rt.request_break();
                    }
                    if (break_every != 0 && vb % break_every == 0) rt.request_break(); // Diagnostics: unwind often
                    if (until_vblank != 0 && vb >= until_vblank) ctx.halted = true;
                    if (perf && vb % 250u == 0) {
                        // Diagnostics: emulation speed over the last 250 frames (5 s of PAL game time)
                        static auto last = std::chrono::steady_clock::now();
                        const auto now = std::chrono::steady_clock::now();
                        const double sec = std::chrono::duration<double>(now - last).count();
                        last = now;
                        std::printf("[perf] kare %u: %.0f fps (%.2fx gercek zaman) | GPU primitif: %llu\n", vb, 250.0 / sec,
                                    (250.0 / sec) / (rt.video_60hz() ? 60.0 : 50.0), static_cast<unsigned long long>(rt.gpu().primitives_drawn()));
                        std::fflush(stdout);
                    }
                    if (series_every > 0 && vb >= series_from && vb % series_every == 0) {
                        // Diagnostics: displayed picture every N frames (upscaled when --scale > 1)
                        char name[32];
                        std::snprintf(name, sizeof(name), "f%06u.ppm", vb);
                        rt.gpu().dump_ppm(series_dir / name, false);
                    }
                    u16 pressed = 0;
                    if (auto_start && vb >= 350u && (vb % 60u) >= 10u && (vb % 60u) <= 18u) {
                        pressed |= pad::START;
                    }
                    for (const auto& p : presses) {
                        if (vb >= p.at_vblank && vb < p.at_vblank + p.frames) pressed |= p.bits;
                    }
                    rt.set_pad_buttons(static_cast<u16>(~pressed));
                    cheats.apply(rt);
                    if (rumble_flag == 1) { // Diagnostics: print vibration changes
                        static int last = -1;
                        const int now_v = (rt.pad_motor_small() ? 256 : 0) | rt.pad_motor_large();
                        if (now_v != last) {
                            std::printf("[rumble] kare %u: kucuk=%d buyuk=%u\n", vb, rt.pad_motor_small() ? 1 : 0, rt.pad_motor_large());
                            last = now_v;
                        }
                    }
                });
            }
            u64 executed = 0;
            while (!ctx.halted && executed < max_cycles) {
                executed += rt.run(ctx, max_cycles - executed);
                if (save_pending) {
                    save_pending = false;
                    const std::vector<u8> st = rt.save_state(ctx);
                    std::ofstream out(save_state_path, std::ios::binary);
                    out.write(reinterpret_cast<const char*>(st.data()), static_cast<std::streamsize>(st.size()));
                    std::printf("[vs_host] Durum kaydedildi: kare %u, %zu bayt\n", rt.vblank_count(), st.size());
                }
            }
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
            if (rt.gpu().frame_interpolation()) {
                std::printf("[vs_host] Ara kare: %llu gosterildi, %llu atlandi\n", static_cast<unsigned long long>(rt.gpu().interp_shown()),
                            static_cast<unsigned long long>(rt.gpu().interp_skipped()));
            }
            if (texrep.active()) {
                std::printf("[vs_host] Doku degistirme: %llu dokulu primitif, %llu tek yuklemede (%%%.1f), %llu arama, %llu isabet, %llu dokum, %zu kayitli yukleme\n",
                            static_cast<unsigned long long>(rt.gpu().repl_considered()), static_cast<unsigned long long>(rt.gpu().repl_in_upload()),
                            rt.gpu().repl_considered() ? 100.0 * static_cast<double>(rt.gpu().repl_in_upload()) / static_cast<double>(rt.gpu().repl_considered()) : 0.0,
                            static_cast<unsigned long long>(texrep.stats.lookups), static_cast<unsigned long long>(texrep.stats.hits),
                            static_cast<unsigned long long>(texrep.stats.dumped), rt.gpu().upload_count());
            }
            if (record_wav) {
                std::vector<s16> chunk;
                rt.spu().pop_samples(chunk, 1u << 20);
                wav_all.insert(wav_all.end(), chunk.begin(), chunk.end());
                std::ofstream out(dump_wav_path, std::ios::binary);
                const u32 bytes = static_cast<u32>(wav_all.size() * sizeof(s16));
                const u32 hdr[] = {0x46464952u, 36u + bytes, 0x45564157u, 0x20746D66u, 16u, 0x00020001u, 44100u, 44100u * 4u, 0x00100004u,
                                   0x61746164u, bytes};
                out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
                out.write(reinterpret_cast<const char*>(wav_all.data()), static_cast<std::streamsize>(bytes));
                std::printf("[vs_host] Tum oturumun sesi (%.1f s) WAV olarak yazildi: %s\n", wav_all.size() / 2.0 / 44100.0,
                            dump_wav_path.string().c_str());
                dump_wav_path.clear();
            }
        }

        rt.gpu().flush_hr();
        if (!dump_ppm_path.empty()) {
            rt.gpu().dump_ppm(dump_ppm_path, !self_demo);
            std::printf("[vs_host] VRAM goruntusu disari aktarildi: %s\n", dump_ppm_path.string().c_str());
        }
        if (!dump_disp_path.empty()) {
            rt.gpu().dump_ppm(dump_disp_path, false);
            std::printf("[vs_host] Aktif ekran goruntusu disari aktarildi: %s\n", dump_disp_path.string().c_str());
        }
        if (!dump_ram_path.empty()) {
            const auto ram = rt.ram_span();
            std::ofstream out(dump_ram_path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(ram.data()), static_cast<std::streamsize>(ram.size()));
            std::printf("[vs_host] PS1 RAM (%zu B) disari aktarildi: %s\n", ram.size(), dump_ram_path.string().c_str());
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
    Win32HostState host_state{&rt, &ctx, &d3d, &audio, self_demo};

    WNDCLASSA wc{};
    wc.lpfnWndProc   = HostWndProc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "VagrantStoryNativePortWnd";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);

    // Window client size: height from the settings (720 by default), width from the aspect ratio;
    // shrunk until it fits the desktop work area
    if (use_settings) {
        host_state.aspect     = std::clamp(settings.aspect, 0, launcher::ASPECT_COUNT - 1);
        host_state.widescreen = settings.widescreen;
    }
    if (aspect_flag >= 0) host_state.aspect = std::clamp(aspect_flag, 0, launcher::ASPECT_COUNT - 1);
    if (widescreen_flag >= 0) host_state.widescreen = widescreen_flag == 1;
    int client_h = use_settings ? settings.window_height : 720;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT wr{};
    for (;;) {
        wr = RECT{0, 0, launcher::window_width_for(client_h, host_state.aspect), client_h};
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        if (client_h <= 240 || ((wr.right - wr.left) <= (work.right - work.left) &&
                                (wr.bottom - wr.top) <= (work.bottom - work.top))) {
            break;
        }
        client_h -= 60;
    }
    const int win_w = wr.right - wr.left, win_h = wr.bottom - wr.top;

    HWND hwnd = CreateWindowExA(
        0, wc.lpszClassName,
        "Vagrant Story Native Port",
        WS_OVERLAPPEDWINDOW | (hidden_window ? 0u : WS_VISIBLE),
        work.left + ((work.right - work.left) - win_w) / 2, work.top + ((work.bottom - work.top) - win_h) / 2,
        win_w, win_h,
        nullptr, nullptr, wc.hInstance, nullptr);

    SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&host_state));
    host_state.hidden = hidden_window;
    host_log("Oyun penceresi olusturuldu (%s)", hwnd ? "tamam" : "HATA");

    if (!sw_present) {
        std::string d3d_err;
        RECT client{};
        GetClientRect(hwnd, &client);
        if (d3d.initialize(hwnd, static_cast<u32>(client.right), static_cast<u32>(client.bottom), &d3d_err)) {
            if (use_settings) {
                d3d.set_shader_mode(static_cast<UpscaleShaderMode>(settings.filter));
                d3d.set_integer_scaling(settings.integer_scale);
            }
            // Default: graphics card (settings "renderer" 0); --gpu-hr / --cpu-hr override
            const bool want_gpu = gpu_hr == 1 || (gpu_hr < 0 && (!use_settings || settings.renderer == 0));
            if (!want_gpu && rt.gpu().internal_scale() > 6u) {
                rt.gpu().set_internal_scale(6); // 7x / 8x are far too slow on the processor
                host_log("Islemci cizimi: ic cozunurluk 6x ile sinirlandi");
            }
            if (want_gpu && rt.gpu().internal_scale() > 1u) {
                use_gpu_hr(d3d.device_handle()); // Shares the window's device: no CPU copy per frame
            }
            std::printf("[vs_host] Direct3D 11 aktif (%s) - Shader: %s\n",
                        d3d.adapter_name().c_str(), d3d.shader_mode_name());
            host_log("Direct3D 11: %s, yuksek cozunurluk %s", d3d.adapter_name().c_str(),
                     rt.gpu().hr_backend() && rt.gpu().hr_backend()->gpu_picture_view() ? "ekran kartinda" : "islemcide");
        } else {
            std::printf("[vs_host] D3D11 baslatilamadi (%s), GDI yedegine geciliyor.\n", d3d_err.c_str());
            host_log("Direct3D 11 baslatilamadi (%s), GDI", d3d_err.c_str());
        }
    }
    if (audio.initialize(PsxSpu::SAMPLE_RATE)) {
        std::printf("[vs_host] %s 44.1 kHz Stereo SPU ADPCM ses cikisi aktif.\n", audio.backend());
    }
    host_log("Ses: %s", audio.is_open() ? audio.backend() : "acilamadi");
    Win32GamepadInput gamepad;
    gamepad.set_log([](const char* line) { host_log("%s", line); });
    gamepad.initialize(hwnd);
    host_log("Kontrolcu girisi hazir");
    std::printf("[vs_host] Gamepad destegi aktif (XInput + DirectInput); kontrolcu istediginiz an takilabilir.\n");
    std::fflush(stdout);
    if (use_settings) {
        gamepad.set_button_map(settings.pad_map);
        audio.set_muted(settings.muted);
        audio.set_volume(static_cast<u32>(settings.volume));
        if (settings.fullscreen) {
            host_state.toggle_fullscreen(hwnd);
            host_log("Tam ekran");
        }
    }
    if (hidden_window) audio.set_muted(true); // Test window: silent, also with settings
    host_state.update_window_title(hwnd);

    std::vector<u32> frame_bgra;
    MSG msg{};
    bool running = true;

    // Real-time frame pacing. The emulated CPU runs far faster than a PS1, so every emulated
    // VBlank waits until its wall-clock slot: 50 Hz for PAL, 60 Hz for NTSC (matching the runtime's
    // VBlank period, so SPU audio stays at 44.1 kHz). Hold Tab to fast-forward.
    LARGE_INTEGER qpc_freq{};
    LARGE_INTEGER next_frame{};
    QueryPerformanceFrequency(&qpc_freq);
    QueryPerformanceCounter(&next_frame);
    timeBeginPeriod(1);
    auto throttle_frame = [&]() {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if ((GetAsyncKeyState(VK_TAB) & 0x8000) != 0) {
            next_frame = now;
            return;
        }
        const LONGLONG period = qpc_freq.QuadPart / (rt.video_60hz() ? 60 : 50);
        next_frame.QuadPart += period;
        if (now.QuadPart > next_frame.QuadPart + period * 4) {
            next_frame = now; // Far behind (slow frame or window drag): resync instead of racing
            return;
        }
        for (;;) {
            QueryPerformanceCounter(&now);
            const LONGLONG remain = next_frame.QuadPart - now.QuadPart;
            if (remain <= 0) break;
            const LONGLONG ms = remain * 1000 / qpc_freq.QuadPart;
            Sleep(ms > 2 ? static_cast<DWORD>(ms - 1) : 0u);
        }
    };

    // RetroAchievements (softcore): needs a login made in the settings window (token in the ini)
    ra::Achievements achievements;
    if (use_settings && settings.ra_enabled && !settings.ra_token.empty() && !hidden_window) {
        host_log("RetroAchievements: %s olarak baglaniliyor", settings.ra_user.c_str());
        achievements.start(rt, settings.ra_user, settings.ra_token, disc_path,
                           [&](const std::string& m) { host_state.show_notice(hwnd, m, 5000); host_log("%s", m.c_str()); },
                           [&](const std::string& m) { host_log("%s", m.c_str()); });
    }

    // Runahead: after each real vblank (input, sound) the state is saved, N vblanks are run ahead
    // with the same input and the last one is shown, then the saved state is loaded back. The
    // game answers a button N vblanks sooner; it costs N extra emulated vblanks per vblank.
    const int runahead = runahead_flag >= 0 ? runahead_flag : (use_settings ? std::clamp(settings.runahead, 0, 3) : 0);
    int ra_phase = 0;          // 0 = real vblank next, 1..runahead = vblanks ahead
    u32 ra_real_vb = 0;        // Vblank count of the real timeline (what is shown is ra_real_vb + runahead)
    std::vector<u8> ra_state;  // State after the last real vblank
    if (runahead > 0) {
        rt.gpu().set_frame_interpolation(false); // Both re-show frames: not together
        host_log("Gecikme azaltma: %d kare ileri", runahead);
    }

    // Save states: files next to the memory card (slot 0 = automatic "continue" state)
    const bool autosave_on = use_settings && settings.autosave && !hidden_window;
    const bool rewind_on   = use_settings ? settings.rewind : false;
    constexpr u32 REWIND_EVERY_VBLANKS       = 50; // One snapshot per second of PAL game time
    constexpr std::size_t REWIND_MAX_STATES  = 30; // ~30 s, ~110 MB
    std::deque<std::vector<u8>> rewind_states;
    bool snapshot_due = false, rewinding = false, test_save_due = false;
    const std::filesystem::path state_dir =
        (memcard_path.empty() ? launcher::default_memcard_path() : std::filesystem::path(memcard_path)).parent_path();
    const auto state_file = [&](int slot) {
        return state_dir / (slot == 0 ? std::string("VagrantStory_devam.vss") : "VagrantStory_durum" + std::to_string(slot) + ".vss");
    };
    const auto save_state_file = [&](const std::filesystem::path& path, const std::string& what) {
        const std::vector<u8> st = rt.save_state(ctx);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(st.data()), static_cast<std::streamsize>(st.size()));
        const bool ok = static_cast<bool>(out);
        host_log("%s: %s (%zu bayt)", what.c_str(), ok ? "kaydedildi" : "KAYDEDILEMEDI", st.size());
        host_state.show_notice(hwnd, what + (ok ? " kaydedildi" : " KAYDEDILEMEDI"));
        return ok;
    };
    const auto load_state_file = [&](const std::filesystem::path& path, const std::string& what) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            host_state.show_notice(hwnd, what + ": kayit yok");
            return false;
        }
        std::vector<u8> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const bool ok = rt.load_state(std::move(data), ctx);
        host_log("%s: %s", what.c_str(), ok ? "yuklendi" : "YUKLENEMEDI (bozuk ya da eski surum)");
        host_state.show_notice(hwnd, what + (ok ? " yuklendi" : " yuklenemedi"));
        return ok;
    };

    auto present_and_pump = [&]() {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                host_log("Cikis mesaji alindi, emulasyon durduruluyor");
                start_exit_watchdog();
                running = false;
                // Not ctx.halted: run() returns at a clean point, so the state can still be saved
                rt.request_break();
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!running) return;
        achievements.pump(); // Server answers (login, game, unlocks)
        cheats.apply(rt);
        if (runahead > 0) {
            rt.request_break(); // The main loop takes over after every vblank (see runahead above)
            if (ra_phase > 0 && ra_phase < runahead) return; // Silent vblank ahead
        }
        const bool ra_show = runahead > 0 && ra_phase == runahead; // Last vblank ahead: only shown
        if (!ra_show) {
        if (runahead > 0) ra_real_vb = rt.vblank_count();

        // Title notice expired -> normal title
        if (!host_state.notice.empty() && GetTickCount64() >= host_state.notice_until) {
            host_state.notice.clear();
            host_state.update_window_title(hwnd);
            if (d3d.is_initialized()) d3d.set_overlay_text(L"");
        }
        // Save state requests for the main loop
        const u32 vb_now = rt.vblank_count();
        if (save_state_vblank != 0 && vb_now == save_state_vblank) { test_save_due = true; rt.request_break(); }
        rewinding = rewind_on && !hidden_window && GetForegroundWindow() == hwnd && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (rewinding) {
            rt.request_break();
        } else if (rewind_on && vb_now % REWIND_EVERY_VBLANKS == 0) {
            snapshot_due = true;
            rt.request_break();
        }

        // Keyboard (only while the game window has focus) + gamepads, merged as active-high
        // PS1 bits, then inverted to the pad's active-low format
        u16 pressed = hidden_window ? 0u : gamepad.poll(); // Hidden test window: no real input
        float cam_x = 0.0f, cam_y = 0.0f;                  // Right stick (free camera)
        float raw_x = 0.0f, raw_y = 0.0f;
        if (!hidden_window) {
            gamepad.right_stick(raw_x, raw_y);
            cam_x = FreeCamera::shape(raw_x, 0.25f);
            cam_y = FreeCamera::shape(raw_y, 0.25f);
        }
        for (const auto& pr : presses) {                    // Scripted --press, as in headless mode
            if (rt.vblank_count() >= pr.at_vblank && rt.vblank_count() < pr.at_vblank + pr.frames) {
                pressed |= pr.bits;
                if (pr.sx != 0.0f) cam_x = pr.sx;
                if (pr.sy != 0.0f) cam_y = pr.sy;
            }
        }
        if (!hidden_window && GetForegroundWindow() == hwnd) {
            // Keyboard camera: J / L turn, I / K tilt
            if ((GetAsyncKeyState('J') & 0x8000) != 0) cam_x = -1.0f;
            if ((GetAsyncKeyState('L') & 0x8000) != 0) cam_x = 1.0f;
            if ((GetAsyncKeyState('I') & 0x8000) != 0) cam_y = -1.0f;
            if ((GetAsyncKeyState('K') & 0x8000) != 0) cam_y = 1.0f;
            struct KeyBind { int vk; u16 bit; };
            constexpr KeyBind KEYS[] = {
                {VK_RETURN, pad::START}, {VK_BACK, pad::SELECT},
                {VK_UP, pad::UP}, {VK_RIGHT, pad::RIGHT}, {VK_DOWN, pad::DOWN}, {VK_LEFT, pad::LEFT},
                {'Z', pad::CROSS}, {'X', pad::CIRCLE}, {'A', pad::SQUARE}, {'S', pad::TRIANGLE},
                {'Q', pad::L1}, {'W', pad::R1}, {'E', pad::L2}, {'R', pad::R2},
            };
            for (const auto& k : KEYS) {
                if ((GetAsyncKeyState(k.vk) & 0x8000) != 0) pressed |= k.bit;
            }
        }
        rt.set_pad_buttons(static_cast<u16>(~pressed));
        cheats.apply(rt);
        if (free_cam_on) free_cam.apply(rt, cam_x, cam_y, rt.video_60hz() ? 60u : 50u);
        {
            // Game log: each push of the right stick (or J/L/I/K), with the raw stick and what the
            // camera did, so a controller that does not turn it can be told apart from the game
            const bool pushed = cam_x != 0.0f || cam_y != 0.0f || raw_x * raw_x + raw_y * raw_y > 0.09f;
            const FreeCamera::Status st = free_cam_on ? free_cam.status() : FreeCamera::Status::Idle;
            if (pushed && (!cam_log_pushed || st != cam_log_status) && cam_log_lines < 200) {
                ++cam_log_lines;
                host_log("Sag cubuk: ham x=%.2f y=%.2f, kullanilan x=%.2f y=%.2f -> %s", raw_x, raw_y, cam_x, cam_y,
                         free_cam_on ? FreeCamera::status_text(st) : "serbest kamera ayarlardan kapali");
            }
            cam_log_pushed = pushed;
            cam_log_status = st;
        }
        if (rumble_on && !hidden_window) gamepad.set_rumble(rt.pad_motor_small(), rt.pad_motor_large());

        // Stream synthesized PS1 SPU 44.1 kHz Stereo PCM to Windows audio ring buffer (runahead:
        // the main loop takes the sound of the real vblank only)
        if (runahead == 0) audio.pump_from_spu(rt.spu());
        achievements.do_frame(); // Real vblanks only (never the ones run ahead)
        if (runahead > 0) return; // Real vblank: shown after running ahead
        } // !ra_show

        host_state.apply_gte_widening(hwnd); // Follows window resizes in "fill" mode
        if (d3d.is_initialized()) {
            const u32 shown_vb = runahead > 0 ? ra_real_vb : rt.vblank_count(); // Runahead: count of the real timeline
            const bool dump_now = !window_dump_path.empty() && shown_vb >= window_dump_frame;
            if (dump_now) d3d.request_backbuffer_dump(window_dump_path);
            // No vsync: pacing comes from throttle_frame(), not the monitor's refresh rate
            d3d.present_vram(rt.gpu(), host_state.show_full_vram,
                             launcher::ASPECT_VALUES[std::clamp(host_state.aspect, 0, launcher::ASPECT_COUNT - 1)], false);
            if (dump_now) {
                std::printf("[vs_host] Pencere goruntusu kaydedildi: %s (kare %u)\n", window_dump_path.c_str(), rt.vblank_count());
                std::fflush(stdout);
                PostQuitMessage(0);
                window_dump_path.clear();
            }
        } else {
            u32 fw = 0, fh = 0;
            rt.gpu().export_bgra8888(frame_bgra, fw, fh, host_state.show_full_vram);
            if (!host_state.show_full_vram && !rt.gpu().display_enabled()) {
                std::fill(frame_bgra.begin(), frame_bgra.end(), 0u); // Video output off: black
            }

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
        }
        if (!no_throttle) throttle_frame();
    };

    if (host_state.self_demo) {
        while (running) {
            host_state.demo_angle += 0.025f;
            step_hardware_self_demo(rt, host_state.demo_angle);
            present_and_pump();
        }
    } else {
        host_log("Emulasyon basladi");
        rt.set_vblank_callback(present_and_pump);
        if (!load_state_path.empty()) {
            load_state_file(load_state_path, "Test durumu");
        } else if (autosave_on && std::filesystem::exists(state_file(0))) {
            load_state_file(state_file(0), "Kaldiginiz yerden devam");
        }
        while (!ctx.halted) {
            rt.run(ctx, 0x00FF'FFFF'FFFF'FFFFull);
            if (ctx.halted) break;
            if (runahead > 0 && running) {
                if (ra_phase == 0) {
                    // A real vblank has run: take its sound, remember this point, then run ahead
                    audio.pump_from_spu(rt.spu());
                    ra_state = rt.save_state(ctx);
                    rt.runahead_snapshot(false);
                    ra_phase = 1;
                    continue;
                }
                if (ra_phase < runahead) {
                    ++ra_phase;
                    continue;
                }
            }
            if (runahead > 0 && ra_phase > 0) {
                // The last vblank ahead has been shown (or quitting): back to the real timeline,
                // where save states, rewind and quitting below happen
                if (!rt.load_state_fast(ra_state, ctx)) host_log("Gecikme azaltma: durum geri yuklenemedi");
                rt.runahead_snapshot(true);
                ra_phase = 0;
            }
            if (!running) {
                // Quitting, stopped at a clean point: keep the state for "continue where you left off"
                if (autosave_on) save_state_file(state_file(0), "Otomatik kayit");
                break;
            }
            if (test_save_due) {
                test_save_due = false;
                save_state_file(save_state_path, "Test durumu");
            }
            if (host_state.state_op == Win32HostState::OP_SAVE) {
                save_state_file(state_file(host_state.state_slot), "Durum " + std::to_string(host_state.state_slot));
            } else if (host_state.state_op == Win32HostState::OP_LOAD) {
                if (load_state_file(state_file(host_state.state_slot), "Durum " + std::to_string(host_state.state_slot))) achievements.reset();
            }
            host_state.state_op = Win32HostState::OP_NONE;
            if (snapshot_due) {
                snapshot_due = false;
                rewind_states.push_back(rt.save_state(ctx));
                if (rewind_states.size() > REWIND_MAX_STATES) rewind_states.pop_front();
            }
            if (rewinding && !rewind_states.empty()) {
                // One step back per ~120 ms while F8 is held; the oldest state stays
                std::vector<u8> st = rewind_states.back();
                if (rewind_states.size() > 1) rewind_states.pop_back();
                if (rt.load_state(std::move(st), ctx)) {
                    host_state.show_notice(hwnd, "Geri sariliyor");
                    achievements.reset();
                }
                Sleep(120);
            }
        }
        host_log(running ? "Emulasyon durdu (islemci durdu), pencere acik" : "Emulasyon durdu");
        while (running) {
            present_and_pump();
        }
    }
    timeEndPeriod(1);
    if (use_settings) {
        // Keep in-game changes (F4 filter, F3 16:9, M mute) for the next start
        if (d3d.is_initialized()) settings.filter = static_cast<int>(d3d.shader_mode());
        settings.widescreen = host_state.widescreen;
        settings.aspect     = host_state.aspect;
        settings.muted      = audio.is_muted();
        launcher::save_settings(launcher::settings_path(), settings);
        host_log("Ayarlar kaydedildi");
    }
    gamepad.set_rumble(false, 0);
    // Shut down step by step (a hang shows in the log); the rest goes when main returns
    if (achievements.active()) {
        achievements.shutdown(); // Waits a moment for unlocks still being sent
        host_log("RetroAchievements kapatildi");
    }
    if (audio.is_open()) {
        std::printf("[vs_host] Ses kuyrugu: %u ms (%s)\n", audio.queue_ms(), audio.backend());
        host_log("Ses kuyrugu: %u ms (%s)", audio.queue_ms(), audio.backend());
    }
    audio.shutdown();
    host_log("Ses kapatildi");
    rt.gpu().set_hr_backend(nullptr); // Before the Direct3D device it may share
    d3d.shutdown();
    host_log("Direct3D kapatildi");
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    if (g_host_log) {
        std::atexit([] {
            host_log("Surec sonlandi");
            std::fclose(g_host_log);
            g_host_log = nullptr;
        });
    }
#endif
    return 0;
}
