// SDL3 host: Linux / Steam Deck and Android (also builds on Windows, for testing).
// Window, sound, controllers, touch buttons and an in-game menu through SDL3; the picture is
// drawn by the processor (software upscaled picture) and shown with SDL's renderer (OpenGL /
// Vulkan / GLES underneath). Settings are the same VagrantStory.ini as on Windows.
#include "vsport/cdrom_iso.hpp"
#include "vsport/cheats.hpp"
#include "vsport/pad_mapping.hpp"
#include "vsport/psx_exe.hpp"
#include "vsport/runtime.hpp"
#include "vsport/texture_replace.hpp"
#include "achievements.hpp"
#include "launcher_win32.hpp"

#ifdef VSPORT_HAS_AOT_MODULES
#include "vs_aot_registry.hpp"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#ifdef SDL_PLATFORM_ANDROID
#include <jni.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace vsport;
namespace fs = std::filesystem;

namespace {

constexpr const char* VERSION = "0.3.1";

// ---------------------------------------------------------------------------------------------
// Log: VagrantStory.log in the data folder, plus SDL_Log (console / Android logcat)
// ---------------------------------------------------------------------------------------------
FILE* g_log = nullptr;

void host_log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SDL_Log("%s", buf);
    if (g_log) {
        std::fprintf(g_log, "[%8.2f s] %s\n", static_cast<double>(SDL_GetTicks()) / 1000.0, buf);
        std::fflush(g_log);
    }
}

fs::path path_from_utf8(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
std::string path_to_utf8(const fs::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// SDL's debug font is ASCII: Turkish letters become their base letters
std::string ascii(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { out += static_cast<char>(c); continue; }
        const unsigned char d = i + 1 < s.size() ? static_cast<unsigned char>(s[i + 1]) : 0;
        char r = '?';
        if (c == 0xC3) {
            switch (d) {
                case 0xA7: r = 'c'; break; case 0x87: r = 'C'; break;
                case 0xB6: r = 'o'; break; case 0x96: r = 'O'; break;
                case 0xBC: r = 'u'; break; case 0x9C: r = 'U'; break;
                default: break;
            }
        } else if (c == 0xC4) {
            switch (d) {
                case 0x9F: r = 'g'; break; case 0x9E: r = 'G'; break;
                case 0xB1: r = 'i'; break; case 0xB0: r = 'I'; break;
                default: break;
            }
        } else if (c == 0xC5) {
            if (d == 0x9F) r = 's';
            else if (d == 0x9E) r = 'S';
        }
        out += r;
        while (i + 1 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) ++i; // Rest of the character
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Drawing helpers (window pixels)
// ---------------------------------------------------------------------------------------------
struct Rgba { u8 r, g, b, a; };

struct Draw {
    SDL_Renderer* r = nullptr;
    float w = 0, h = 0; // Output size in pixels

    void color(Rgba c) const { SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a); }
    void fill(float x, float y, float rw, float rh, Rgba c) const {
        color(c);
        const SDL_FRect fr{x, y, rw, rh};
        SDL_RenderFillRect(r, &fr);
    }
    // Text with the 8x8 debug font at `size` pixels per character
    void text(float x, float y, const std::string& s, float size, Rgba c) const {
        const float k = size / static_cast<float>(SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE);
        SDL_SetRenderScale(r, k, k);
        color(c);
        SDL_RenderDebugText(r, x / k, y / k, ascii(s).c_str());
        SDL_SetRenderScale(r, 1.0f, 1.0f);
    }
    static float text_w(const std::string& s, float size) { return static_cast<float>(ascii(s).size()) * size; }
    void text_centered(float cx, float y, const std::string& s, float size, Rgba c) const { text(cx - text_w(s, size) / 2, y, s, size, c); }

    void fill_circle(float cx, float cy, float rad, Rgba c) const {
        constexpr int N = 28;
        SDL_Vertex v[N + 1];
        int idx[N * 3];
        const SDL_FColor fc{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
        v[0] = {{cx, cy}, fc, {0, 0}};
        for (int i = 0; i < N; ++i) {
            const float a = static_cast<float>(i) * 6.2831853f / N;
            v[i + 1] = {{cx + std::cos(a) * rad, cy + std::sin(a) * rad}, fc, {0, 0}};
            idx[i * 3] = 0;
            idx[i * 3 + 1] = i + 1;
            idx[i * 3 + 2] = (i + 1) % N + 1;
        }
        SDL_RenderGeometry(r, nullptr, v, N + 1, idx, N * 3);
    }
    void line(float x0, float y0, float x1, float y1, float th, Rgba c) const {
        const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.0f) return;
        const float nx = -dy / len * th / 2, ny = dx / len * th / 2;
        const SDL_FColor fc{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
        const SDL_Vertex v[4] = {{{x0 + nx, y0 + ny}, fc, {0, 0}}, {{x1 + nx, y1 + ny}, fc, {0, 0}},
                                 {{x1 - nx, y1 - ny}, fc, {0, 0}}, {{x0 - nx, y0 - ny}, fc, {0, 0}}};
        const int idx[6] = {0, 1, 2, 0, 2, 3};
        SDL_RenderGeometry(r, nullptr, v, 4, idx, 6);
    }
    void ring(float cx, float cy, float rad, float th, Rgba c) const {
        constexpr int N = 28;
        for (int i = 0; i < N; ++i) {
            const float a0 = static_cast<float>(i) * 6.2831853f / N, a1 = static_cast<float>(i + 1) * 6.2831853f / N;
            line(cx + std::cos(a0) * rad, cy + std::sin(a0) * rad, cx + std::cos(a1) * rad, cy + std::sin(a1) * rad, th, c);
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Menus: a list of items, driven by keyboard, controller or touch
// ---------------------------------------------------------------------------------------------
struct MenuItem {
    std::string label;
    std::function<std::string()> value; // Shown on the right (optional)
    std::function<void(int)> act;       // 0 = chosen, -1 / +1 = changed left / right
};

enum class MenuInput { None, Up, Down, Left, Right, Ok, Back };

struct Menu {
    std::string title;
    std::string footer;
    int sel = 0, top = 0;
    // Layout of the last draw (touch hit testing)
    float row_y0 = 0, row_h = 1, box_x = 0, box_w = 0;
    int rows_shown = 0;

    void draw(const Draw& d, const std::vector<MenuItem>& items) {
        const float size = std::clamp(std::floor(std::min(d.w, d.h) / 30.0f), 10.0f, 40.0f);
        row_h = size * 2.0f;
        box_w = std::min(d.w - size * 2, size * 44);
        box_x = (d.w - box_w) / 2;
        const float title_h = size * 3;
        rows_shown = std::max(1, static_cast<int>((d.h - title_h - size * 4) / row_h));
        const int n = static_cast<int>(items.size());
        sel = std::clamp(sel, 0, std::max(0, n - 1));
        if (sel < top) top = sel;
        if (sel >= top + rows_shown) top = sel - rows_shown + 1;
        top = std::clamp(top, 0, std::max(0, n - rows_shown));
        const int shown = std::min(n - top, rows_shown);
        const float box_h = title_h + static_cast<float>(shown) * row_h + size * 2.5f;
        const float y = std::max(0.0f, (d.h - box_h) / 2);
        d.fill(0, 0, d.w, d.h, {0, 0, 0, 150});
        d.fill(box_x, y, box_w, box_h, {20, 22, 30, 235});
        d.text_centered(d.w / 2, y + size, title, size * 1.2f, {235, 210, 150, 255});
        row_y0 = y + title_h;
        for (int i = 0; i < shown; ++i) {
            const MenuItem& it = items[static_cast<std::size_t>(top + i)];
            const float ry = row_y0 + static_cast<float>(i) * row_h;
            const bool on = top + i == sel;
            if (on) d.fill(box_x + size * 0.5f, ry, box_w - size, row_h, {70, 80, 120, 255});
            const Rgba tc = on ? Rgba{255, 255, 255, 255} : Rgba{200, 200, 210, 255};
            const float ty = ry + (row_h - size) / 2;
            d.text(box_x + size * 1.2f, ty, it.label, size, tc);
            if (it.value) {
                const std::string v = it.act ? "< " + it.value() + " >" : it.value();
                d.text(box_x + box_w - size * 1.2f - Draw::text_w(v, size), ty, v, size, tc);
            }
        }
        if (top > 0) d.text(box_x + box_w - size * 2, row_y0 - size * 1.2f, "^", size, {200, 200, 210, 255});
        if (top + shown < n) d.text(box_x + box_w - size * 2, row_y0 + static_cast<float>(shown) * row_h, "v", size, {200, 200, 210, 255});
        if (!footer.empty()) d.text_centered(d.w / 2, y + box_h - size * 1.6f, footer, size * 0.8f, {150, 150, 160, 255});
    }

    // Returns true when an item was chosen (or a value changed)
    bool input(MenuInput in, const std::vector<MenuItem>& items) {
        const int n = static_cast<int>(items.size());
        if (n == 0) return false;
        switch (in) {
            case MenuInput::Up: sel = (sel + n - 1) % n; return false;
            case MenuInput::Down: sel = (sel + 1) % n; return false;
            case MenuInput::Left:
            case MenuInput::Right:
                if (items[static_cast<std::size_t>(sel)].value && items[static_cast<std::size_t>(sel)].act) {
                    items[static_cast<std::size_t>(sel)].act(in == MenuInput::Left ? -1 : 1);
                    return true;
                }
                return false;
            case MenuInput::Ok:
                if (items[static_cast<std::size_t>(sel)].act) items[static_cast<std::size_t>(sel)].act(items[static_cast<std::size_t>(sel)].value ? 1 : 0);
                return true;
            default: return false;
        }
    }
    // Touch / mouse: tapping a row chooses it
    bool tap(float x, float y, const std::vector<MenuItem>& items) {
        if (x < box_x || x > box_x + box_w || y < row_y0) return false;
        const int i = top + static_cast<int>((y - row_y0) / row_h);
        if (i < top || i >= top + rows_shown || i >= static_cast<int>(items.size())) return false;
        sel = i;
        return input(MenuInput::Ok, items);
    }
};

// ---------------------------------------------------------------------------------------------
// Touch buttons
// ---------------------------------------------------------------------------------------------
struct TouchPad {
    struct Finger { float x, y; };
    std::map<SDL_FingerID, Finger> fingers;
    bool visible = false;

    struct Layout {
        float u = 0;
        float dpad_x = 0, dpad_y = 0, dpad_r = 0;
        float face_x = 0, face_y = 0, face_off = 0, face_r = 0;
        SDL_FRect l1{}, l2{}, r1{}, r2{}, select{}, start{}, menu{};
    };
    static Layout layout(float w, float h) {
        Layout l;
        l.u = std::min(w, h);
        const float u = l.u;
        l.dpad_r = 0.17f * u;
        l.dpad_x = 0.05f * u + l.dpad_r;
        l.dpad_y = h - 0.06f * u - l.dpad_r;
        l.face_r = 0.072f * u;
        l.face_off = 0.11f * u;
        l.face_x = w - 0.05f * u - l.face_off - l.face_r;
        l.face_y = l.dpad_y;
        const float bw = 0.2f * u, bh = 0.085f * u;
        const bool landscape = w > h * 1.1f;
        const float ly = landscape ? 0.03f * u : l.dpad_y - l.dpad_r - 0.26f * u;
        l.l2 = {0.03f * u, ly, bw, bh};
        l.l1 = {0.03f * u, ly + bh + 0.025f * u, bw, bh};
        l.r2 = {w - 0.03f * u - bw, ly, bw, bh};
        l.r1 = {w - 0.03f * u - bw, ly + bh + 0.025f * u, bw, bh};
        const float sw = 0.15f * u, sh = 0.065f * u;
        l.select = {w / 2 - sw - 0.02f * u, h - 0.03f * u - sh, sw, sh};
        l.start = {w / 2 + 0.02f * u, h - 0.03f * u - sh, sw, sh};
        l.menu = {w / 2 - 0.06f * u, 0.02f * u, 0.12f * u, 0.075f * u};
        return l;
    }
    static bool in_rect(const SDL_FRect& r, float x, float y, float pad) {
        return x >= r.x - pad && x <= r.x + r.w + pad && y >= r.y - pad && y <= r.y + r.h + pad;
    }

    u16 buttons(float w, float h) const {
        if (!visible) return 0;
        const Layout l = layout(w, h);
        u16 p = 0;
        for (const auto& [id, f] : fingers) {
            (void)id;
            const float dx = f.x - l.dpad_x, dy = f.y - l.dpad_y, dist = std::sqrt(dx * dx + dy * dy);
            if (dist < l.dpad_r * 1.5f) {
                if (dist > l.dpad_r * 0.2f) {
                    if (dx < -0.38f * dist) p |= pad::LEFT;
                    if (dx > 0.38f * dist) p |= pad::RIGHT;
                    if (dy < -0.38f * dist) p |= pad::UP;
                    if (dy > 0.38f * dist) p |= pad::DOWN;
                }
                continue;
            }
            struct Face { float x, y; u16 bit; };
            const Face faces[4] = {{l.face_x, l.face_y - l.face_off, pad::TRIANGLE}, {l.face_x + l.face_off, l.face_y, pad::CIRCLE},
                                   {l.face_x, l.face_y + l.face_off, pad::CROSS}, {l.face_x - l.face_off, l.face_y, pad::SQUARE}};
            float best = l.face_r * 1.45f;
            u16 bit = 0;
            for (const Face& fc : faces) {
                const float d = std::hypot(f.x - fc.x, f.y - fc.y);
                if (d < best) { best = d; bit = fc.bit; }
            }
            if (bit) { p |= bit; continue; }
            const float pad_px = 0.015f * l.u;
            if (in_rect(l.l1, f.x, f.y, pad_px)) p |= pad::L1;
            else if (in_rect(l.l2, f.x, f.y, pad_px)) p |= pad::L2;
            else if (in_rect(l.r1, f.x, f.y, pad_px)) p |= pad::R1;
            else if (in_rect(l.r2, f.x, f.y, pad_px)) p |= pad::R2;
            else if (in_rect(l.select, f.x, f.y, pad_px)) p |= pad::SELECT;
            else if (in_rect(l.start, f.x, f.y, pad_px)) p |= pad::START;
        }
        return p;
    }

    void draw(const Draw& d, u16 held) const {
        if (!visible) return;
        const Layout l = layout(d.w, d.h);
        const Rgba base{255, 255, 255, 46}, edge{255, 255, 255, 110}, lit{255, 255, 255, 120};
        const float th = std::max(2.0f, 0.008f * l.u);
        // D-pad: a cross of four arms around a centre square
        const float a = l.dpad_r * 0.3f, len = l.dpad_r * 0.9f - a, cx = l.dpad_x, cy = l.dpad_y;
        d.fill_circle(cx, cy, l.dpad_r, {255, 255, 255, 22});
        d.fill(cx - a, cy - a, a * 2, a * 2, base);
        d.fill(cx - a, cy - a - len, a * 2, len, (held & pad::UP) ? lit : base);
        d.fill(cx - a, cy + a, a * 2, len, (held & pad::DOWN) ? lit : base);
        d.fill(cx - a - len, cy - a, len, a * 2, (held & pad::LEFT) ? lit : base);
        d.fill(cx + a, cy - a, len, a * 2, (held & pad::RIGHT) ? lit : base);
        // Face buttons with their PlayStation symbols
        struct Face { float x, y; u16 bit; int sym; Rgba c; };
        const Face faces[4] = {{l.face_x, l.face_y - l.face_off, pad::TRIANGLE, 0, {120, 230, 190, 200}},
                               {l.face_x + l.face_off, l.face_y, pad::CIRCLE, 1, {240, 120, 120, 200}},
                               {l.face_x, l.face_y + l.face_off, pad::CROSS, 2, {130, 170, 250, 200}},
                               {l.face_x - l.face_off, l.face_y, pad::SQUARE, 3, {240, 150, 220, 200}}};
        for (const Face& f : faces) {
            d.fill_circle(f.x, f.y, l.face_r, (held & f.bit) ? lit : base);
            d.ring(f.x, f.y, l.face_r, th * 0.6f, edge);
            const float s = l.face_r * 0.42f;
            switch (f.sym) {
                case 0:
                    d.line(f.x, f.y - s, f.x + s, f.y + s * 0.75f, th, f.c);
                    d.line(f.x + s, f.y + s * 0.75f, f.x - s, f.y + s * 0.75f, th, f.c);
                    d.line(f.x - s, f.y + s * 0.75f, f.x, f.y - s, th, f.c);
                    break;
                case 1: d.ring(f.x, f.y, s, th, f.c); break;
                case 2:
                    d.line(f.x - s, f.y - s, f.x + s, f.y + s, th, f.c);
                    d.line(f.x - s, f.y + s, f.x + s, f.y - s, th, f.c);
                    break;
                default:
                    d.line(f.x - s, f.y - s, f.x + s, f.y - s, th, f.c);
                    d.line(f.x + s, f.y - s, f.x + s, f.y + s, th, f.c);
                    d.line(f.x + s, f.y + s, f.x - s, f.y + s, th, f.c);
                    d.line(f.x - s, f.y + s, f.x - s, f.y - s, th, f.c);
                    break;
            }
        }
        const float ts = std::max(10.0f, std::floor(l.u * 0.035f));
        const auto button = [&](const SDL_FRect& r, const char* name, bool on) {
            d.fill(r.x, r.y, r.w, r.h, on ? lit : base);
            const float s = std::floor(std::min(ts, r.w * 0.85f / static_cast<float>(std::strlen(name))));
            d.text_centered(r.x + r.w / 2, r.y + (r.h - s) / 2, name, s, {255, 255, 255, 170});
        };
        button(l.l1, "L1", held & pad::L1);
        button(l.l2, "L2", held & pad::L2);
        button(l.r1, "R1", held & pad::R1);
        button(l.r2, "R2", held & pad::R2);
        button(l.select, "SELECT", held & pad::SELECT);
        button(l.start, "START", held & pad::START);
        d.fill(l.menu.x, l.menu.y, l.menu.w, l.menu.h, base);
        for (int i = 0; i < 3; ++i) {
            const float y = l.menu.y + l.menu.h * (0.3f + 0.2f * static_cast<float>(i));
            d.line(l.menu.x + l.menu.w * 0.3f, y, l.menu.x + l.menu.w * 0.7f, y, th * 0.7f, {255, 255, 255, 170});
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Controllers
// ---------------------------------------------------------------------------------------------
struct Gamepads {
    std::vector<SDL_Gamepad*> pads;
    pad::ButtonMap map = pad::default_button_map();
    int last_small = -1, last_large = -1;

    void add(SDL_JoystickID id) {
        if (SDL_Gamepad* g = SDL_OpenGamepad(id)) {
            pads.push_back(g);
            const char* name = SDL_GetGamepadName(g);
            host_log("Kontrolcu takildi: %s", name ? name : "?");
        }
    }
    void remove(SDL_JoystickID id) {
        for (auto it = pads.begin(); it != pads.end(); ++it) {
            if (SDL_GetGamepadID(*it) == id) {
                host_log("Kontrolcu cikarildi");
                SDL_CloseGamepad(*it);
                pads.erase(it);
                return;
            }
        }
    }
    // Active-high PS1 bits by position (bottom face button = Cross), then the button map
    u16 poll() const {
        u16 p = 0;
        for (SDL_Gamepad* g : pads) {
            const auto b = [g](SDL_GamepadButton x) { return SDL_GetGamepadButton(g, x); };
            if (b(SDL_GAMEPAD_BUTTON_DPAD_UP)) p |= pad::UP;
            if (b(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) p |= pad::DOWN;
            if (b(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) p |= pad::LEFT;
            if (b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) p |= pad::RIGHT;
            if (b(SDL_GAMEPAD_BUTTON_START)) p |= pad::START;
            if (b(SDL_GAMEPAD_BUTTON_BACK)) p |= pad::SELECT;
            if (b(SDL_GAMEPAD_BUTTON_LEFT_STICK)) p |= pad::L3;
            if (b(SDL_GAMEPAD_BUTTON_RIGHT_STICK)) p |= pad::R3;
            if (b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) p |= pad::L1;
            if (b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) p |= pad::R1;
            if (b(SDL_GAMEPAD_BUTTON_SOUTH)) p |= pad::CROSS;
            if (b(SDL_GAMEPAD_BUTTON_EAST)) p |= pad::CIRCLE;
            if (b(SDL_GAMEPAD_BUTTON_WEST)) p |= pad::SQUARE;
            if (b(SDL_GAMEPAD_BUTTON_NORTH)) p |= pad::TRIANGLE;
            if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000) p |= pad::L2;
            if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000) p |= pad::R2;
            const int x = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
            if (y < -pad::XINPUT_STICK_DEADZONE) p |= pad::UP; // SDL: +Y is down
            if (y > pad::XINPUT_STICK_DEADZONE) p |= pad::DOWN;
            if (x < -pad::XINPUT_STICK_DEADZONE) p |= pad::LEFT;
            if (x > pad::XINPUT_STICK_DEADZONE) p |= pad::RIGHT;
        }
        return pad::apply_button_map(p, map);
    }
    void rumble(bool small_motor, u8 large) {
        const int s = small_motor ? 1 : 0, l = large;
        if (s == last_small && l == last_large) return;
        last_small = s;
        last_large = l;
        for (SDL_Gamepad* g : pads) SDL_RumbleGamepad(g, static_cast<Uint16>(l * 257), small_motor ? 0xFFFF : 0, (s || l) ? 5000 : 0);
    }
    void close() {
        for (SDL_Gamepad* g : pads) {
            SDL_RumbleGamepad(g, 0, 0, 0);
            SDL_CloseGamepad(g);
        }
        pads.clear();
    }
};

// ---------------------------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------------------------
struct Audio {
    SDL_AudioStream* stream = nullptr;
    std::vector<s16> buf;
    bool muted = false;
    int volume = 100;

    bool open() {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(PsxSpu::SAMPLE_RATE)};
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream) return false;
        apply_gain();
        SDL_ResumeAudioStreamDevice(stream);
        return true;
    }
    void apply_gain() const {
        if (stream) SDL_SetAudioStreamGain(stream, muted ? 0.0f : static_cast<float>(volume) / 100.0f);
    }
    void pause(bool p) const {
        if (!stream) return;
        if (p) SDL_PauseAudioStreamDevice(stream);
        else SDL_ResumeAudioStreamDevice(stream);
    }
    // Everything the SPU has made goes to SDL; while more than ~150 ms is waiting (fast forward,
    // a slow start) new sound is dropped so the delay never grows
    void pump(PsxSpu& spu) {
        const std::size_t n = spu.buffered_frames();
        if (n == 0) return;
        const std::size_t got = spu.pop_samples(buf, n);
        if (!stream || got == 0) return;
        const int queued = SDL_GetAudioStreamQueued(stream);
        if (queued > static_cast<int>(PsxSpu::SAMPLE_RATE * 4u * 15u / 100u)) return;
        SDL_PutAudioStreamData(stream, buf.data(), static_cast<int>(got * 4u));
    }
    void close() {
        if (stream) SDL_DestroyAudioStream(stream);
        stream = nullptr;
    }
};

// ---------------------------------------------------------------------------------------------
// Picture
// ---------------------------------------------------------------------------------------------
struct Video {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* tex = nullptr;
    int tex_w = 0, tex_h = 0;
    SDL_PixelFormat tex_fmt = SDL_PIXELFORMAT_UNKNOWN;
    std::vector<u32> frame;
    bool have_picture = false;

    Draw draw() const {
        Draw d;
        d.r = renderer;
        int w = 0, h = 0;
        SDL_GetRenderOutputSize(renderer, &w, &h);
        d.w = static_cast<float>(w);
        d.h = static_cast<float>(h);
        return d;
    }
    bool ensure(int w, int h, SDL_PixelFormat fmt, int filter) {
        if (tex && w == tex_w && h == tex_h && fmt == tex_fmt) return true;
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(renderer, fmt, SDL_TEXTUREACCESS_STREAMING, w, h);
        tex_w = w;
        tex_h = h;
        tex_fmt = fmt;
        set_filter(filter);
        return tex != nullptr;
    }
    void set_filter(int filter) const {
        if (tex) SDL_SetTextureScaleMode(tex, filter == 0 ? SDL_SCALEMODE_PIXELART : SDL_SCALEMODE_LINEAR);
    }

    // Copies the game's display area into the texture (upscaled picture when there is one)
    void upload(PsxGpu& gpu, int filter) {
        have_picture = false;
        if (!gpu.display_enabled() || gpu.display_width() == 0 || gpu.display_height() == 0) return;
        const u32 s = gpu.internal_scale();
        if (s > 1u && !gpu.is_24bit_display()) {
            gpu.flush_hr();
            const std::vector<u32>& hr = gpu.hr_vram(); // 0x00BBGGRR (bit 24 = mask bit)
            const u32 hw = PsxGpu::VRAM_WIDTH * s, hh = PsxGpu::VRAM_HEIGHT * s;
            if (hr.size() == static_cast<std::size_t>(hw) * hh) {
                const u32 x0 = (gpu.display_vram_x() * s) % hw, y0 = gpu.display_vram_y() * s;
                const u32 w = std::min(gpu.display_width() * s, hw - x0), h = std::min(gpu.display_height() * s, hh);
                if (!ensure(static_cast<int>(w), static_cast<int>(h), SDL_PIXELFORMAT_XBGR8888, filter)) return;
                void* px = nullptr;
                int pitch = 0;
                if (!SDL_LockTexture(tex, nullptr, &px, &pitch)) return;
                for (u32 y = 0; y < h; ++y) {
                    std::memcpy(static_cast<u8*>(px) + static_cast<std::size_t>(y) * static_cast<std::size_t>(pitch),
                                hr.data() + static_cast<std::size_t>((y0 + y) % hh) * hw + x0, w * 4u);
                }
                SDL_UnlockTexture(tex);
                have_picture = true;
                return;
            }
        }
        u32 fw = 0, fh = 0;
        gpu.export_bgra8888(frame, fw, fh, false); // 0xFFRRGGBB
        if (fw == 0 || fh == 0 || !ensure(static_cast<int>(fw), static_cast<int>(fh), SDL_PIXELFORMAT_ARGB8888, filter)) return;
        SDL_UpdateTexture(tex, nullptr, frame.data(), static_cast<int>(fw * 4u));
        have_picture = true;
    }

    // Picture rectangle in the window for an aspect ratio (0 = fill)
    static SDL_FRect fit(float W, float H, float aspect, bool integer, u32 native_h) {
        if (aspect <= 0.0f) return {0, 0, W, H};
        float w = W, h = W / aspect;
        if (h > H) { h = H; w = H * aspect; }
        if (integer && native_h > 0 && h >= static_cast<float>(native_h)) {
            h = std::floor(h / static_cast<float>(native_h)) * static_cast<float>(native_h);
            w = h * aspect;
        }
        return {std::floor((W - w) / 2), std::floor((H - h) / 2), std::floor(w), std::floor(h)};
    }
};

bool write_ppm(const fs::path& path, SDL_Surface* s) {
    SDL_Surface* rgb = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGB24);
    if (!rgb) return false;
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << rgb->w << " " << rgb->h << "\n255\n";
    for (int y = 0; y < rgb->h; ++y) out.write(static_cast<const char*>(rgb->pixels) + static_cast<std::size_t>(y) * rgb->pitch, rgb->w * 3);
    SDL_DestroySurface(rgb);
    return static_cast<bool>(out);
}

// ---------------------------------------------------------------------------------------------
// Disc import on Android: the system file picker gives content:// links; the chosen files are
// copied into the app's folder (the game reads its disc with ordinary file access)
// ---------------------------------------------------------------------------------------------
struct Importer {
    std::mutex m;
    std::vector<std::string> picked;
    bool answered = false, cancelled = false;
    std::atomic<u64> done{0}, total{0};
    std::atomic<bool> running{false}, finished{false};
    std::string result_disc, result_sbi, error;
    std::thread worker;

    static void SDLCALL on_files(void* ud, const char* const* list, int) {
        auto* self = static_cast<Importer*>(ud);
        std::lock_guard<std::mutex> lock(self->m);
        self->answered = true;
        if (!list || !list[0]) { self->cancelled = true; return; }
        for (int i = 0; list[i]; ++i) self->picked.emplace_back(list[i]);
    }

    // Recognised by content (the picker does not always give file names): .sbi starts with
    // "SBI", a raw .bin is a whole number of 2352-byte sectors, an .iso of 2048-byte ones.
    // A .cue is not needed (single-track disc).
    void start(const fs::path& dir) {
        running = true;
        finished = false;
        worker = std::thread([this, dir] {
            std::error_code ec;
            fs::create_directories(dir, ec);
            std::vector<std::string> list;
            {
                std::lock_guard<std::mutex> lock(m);
                list = picked;
            }
            u64 sum = 0;
            for (const std::string& u : list) {
                if (SDL_IOStream* io = SDL_IOFromFile(u.c_str(), "rb")) {
                    sum += static_cast<u64>(std::max<Sint64>(0, SDL_GetIOSize(io)));
                    SDL_CloseIO(io);
                }
            }
            total = sum;
            std::vector<u8> chunk(4u << 20);
            for (const std::string& u : list) {
                SDL_IOStream* io = SDL_IOFromFile(u.c_str(), "rb");
                if (!io) { error = std::string("Dosya acilamadi: ") + SDL_GetError(); continue; }
                const Sint64 size = SDL_GetIOSize(io);
                char head[4] = {};
                const std::size_t hn = SDL_ReadIO(io, head, 4);
                SDL_SeekIO(io, 0, SDL_IO_SEEK_SET);
                std::string name;
                if (hn == 4 && std::memcmp(head, "SBI", 3) == 0) name = "disk.sbi";
                else if (size > 0 && size % 2352 == 0) name = "disk.bin";
                else if (size > 0 && size % 2048 == 0) name = "disk.iso";
                if (name.empty()) {
                    done += static_cast<u64>(std::max<Sint64>(0, size));
                    SDL_CloseIO(io);
                    continue;
                }
                const fs::path target = dir / name;
                std::ofstream out(target, std::ios::binary | std::ios::trunc);
                for (;;) {
                    const std::size_t got = SDL_ReadIO(io, chunk.data(), chunk.size());
                    if (got == 0) break;
                    out.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(got));
                    done += got;
                }
                SDL_CloseIO(io);
                if (!out) { error = "Kopyalanamadi (yer kalmamis olabilir)"; continue; }
                if (name == "disk.sbi") result_sbi = path_to_utf8(target);
                else result_disc = path_to_utf8(target);
            }
            if (result_disc.empty() && error.empty()) error = "Secilen dosyalarda disk imaji (.bin / .iso) yok";
            finished = true;
            running = false;
        });
    }
    ~Importer() {
        if (worker.joinable()) worker.join();
    }
};

bool is_disc_file(const fs::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e == ".cue" || e == ".bin" || e == ".iso" || e == ".img";
}

// ---------------------------------------------------------------------------------------------
// The application
// ---------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------
// Cover picture: the Rood Inverse from the end of the opening movie, for the Android home
// screen shortcut. A second, hidden machine plays the start of the game without input (the
// emulation is deterministic) and keeps the frame that looks most like the bare emblem:
// copper in the middle, dark sides, no blue logo glow, few sparks. Nothing of the game is in
// the app; the picture comes from the player's own disc.
// ---------------------------------------------------------------------------------------------
struct CoverPick {
    std::vector<u32> px; // 0xFFRRGGBB
    u32 w = 0, h = 0, vb = 0;
    double sparks = 0;
};

// Share of blue-violet pixels (the logo's burst of light) and of white ones (sparks)
void cover_stats(const std::vector<u32>& px, u32 w, u32 h, double& blue, double& white) {
    blue = white = 0;
    if (w < 64 || h < 64) return;
    for (u32 y = 0; y < h; y += 2) {
        for (u32 x = 0; x < w; x += 2) {
            const u32 c = px[static_cast<std::size_t>(y) * w + x];
            const int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
            if (r > 180 && g > 180 && b > 180) white += 1;
            else if (b > 90 && b > r + 30) blue += 1;
        }
    }
    const double n = (w / 2.0) * (h / 2.0);
    blue /= n;
    white /= n;
}
// 432 x 432 PNG (Android adaptive icon size at 4x): the emblem centred on black, inside the
// part that every launcher mask keeps
bool write_cover_png(const CoverPick& p, const fs::path& out) {
    constexpr int S = 432, INNER = 300; // Adaptive icons show the middle 288 px (round masks less)
    TexImage img;
    img.w = img.h = S;
    img.rgba.assign(static_cast<std::size_t>(S) * S, 0xFF000000u);
    const float side = static_cast<float>(std::min(p.w, p.h));
    const float sx0 = (static_cast<float>(p.w) - side) / 2, sy0 = (static_cast<float>(p.h) - side) / 2;
    const int o = (S - INNER) / 2;
    const auto at = [&](int x, int y) {
        x = std::clamp(x, 0, static_cast<int>(p.w) - 1);
        y = std::clamp(y, 0, static_cast<int>(p.h) - 1);
        return p.px[static_cast<std::size_t>(y) * p.w + static_cast<std::size_t>(x)];
    };
    for (int y = 0; y < INNER; ++y) {
        for (int x = 0; x < INNER; ++x) {
            // Bilinear, slightly brightened (the movie is dark on a phone screen)
            const float fx = sx0 + (x + 0.5f) * side / INNER - 0.5f, fy = sy0 + (y + 0.5f) * side / INNER - 0.5f;
            const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
            const float ax = fx - ix, ay = fy - iy;
            const u32 c00 = at(ix, iy), c10 = at(ix + 1, iy), c01 = at(ix, iy + 1), c11 = at(ix + 1, iy + 1);
            u32 out_px = 0xFF000000u;
            for (int sh = 0; sh <= 16; sh += 8) {
                const auto ch = [&](u32 c) { return static_cast<float>((c >> sh) & 255u); };
                const float v = (ch(c00) * (1 - ax) + ch(c10) * ax) * (1 - ay) + (ch(c01) * (1 - ax) + ch(c11) * ax) * ay;
                const u32 b = static_cast<u32>(std::clamp(v * 1.25f, 0.0f, 255.0f));
                // 0xFFRRGGBB in -> 0xAABBGGRR out
                out_px |= (sh == 16 ? b : sh == 8 ? b << 8 : b << 16);
            }
            img.rgba[static_cast<std::size_t>(y + o) * S + static_cast<std::size_t>(x + o)] = out_px;
        }
    }
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    return save_png(out, img);
}

bool capture_cover(const fs::path& disc_path, const fs::path& sbi_path, const fs::path& out, const std::atomic<bool>& cancel) {
    const u64 t0 = SDL_GetTicks();
    PsxRuntime rt(PSX_RAM_SIZE_2MB);
    CpuContext ctx{};
    DiscImage disc;
    std::string err;
    if (!disc.open(disc_path, &err) || (!sbi_path.empty() && !disc.load_sbi(sbi_path, &err))) {
        host_log("Kapak: disk acilamadi: %s", err.c_str());
        return false;
    }
    rt.attach_disc(&disc);
    const auto cnf = disc.parse_system_cnf();
    PsxExeImage exe{};
    if (!cnf || !parse_psx_exe(disc.extract_file(cnf->boot_path), exe, &err)) {
        host_log("Kapak: oyun dosyasi okunamadi");
        return false;
    }
    rt.load_exe(exe, ctx);
    rt.set_pad_buttons(0xFFFFu); // Nothing pressed: the opening movie plays to its end

    // The emblem stands alone just before a violet burst of light brings in the logo (PAL frame
    // ~2485, NTSC later). That burst is the brightest violet of the whole opening, so the
    // picture is the calmest frame (fewest sparks) from 0.9 to 0.5 s before it.
    constexpr u32 FROM = 1800, TO = 4500;
    constexpr double BURST = 0.35;
    std::deque<CoverPick> recent; // Every second frame of the last second
    CoverPick best;
    bool found = false;
    std::vector<u32> frame;
    rt.set_vblank_callback([&] {
        const u32 vb = rt.vblank_count();
        if (found || cancel.load(std::memory_order_relaxed) || vb >= TO) {
            rt.request_break();
            return;
        }
        if (vb < FROM || (vb & 1u)) return;
        u32 fw = 0, fh = 0;
        rt.gpu().export_bgra8888(frame, fw, fh, false);
        double blue = 0, white = 0;
        cover_stats(frame, fw, fh, blue, white);
        if (SDL_getenv("VSPORT_COVER_DEBUG")) host_log("kapak kare %u mavi %.3f beyaz %.3f", vb, blue, white);
        if (blue >= BURST) {
            const u32 fps = rt.video_60hz() ? 60u : 50u;
            const u32 lo = vb - fps * 9 / 10, hi = vb - fps / 2;
            double least = 1e9;
            for (const CoverPick& c : recent) {
                if (c.vb >= lo && c.vb <= hi && c.sparks < least) {
                    least = c.sparks;
                    best = c;
                }
            }
            found = best.vb != 0;
            if (!found) recent.clear();
            rt.request_break();
            return;
        }
        recent.push_back({frame, fw, fh, vb, blue + 2.0 * white});
        if (recent.size() > 30) recent.pop_front();
    });
    while (!ctx.halted && !found && !cancel.load() && rt.vblank_count() < TO) rt.run(ctx, 0x00FF'FFFF'FFFF'FFFFull);    rt.set_vblank_callback(nullptr);
    if (cancel.load()) return false;
    if (best.vb == 0) {
        host_log("Kapak: uygun kare bulunamadi");
        return false;
    }
    const bool ok = write_cover_png(best, out);
    host_log("Kapak resmi %s: kare %u, %.1f s", ok ? "kaydedildi" : "KAYDEDILEMEDI", best.vb,
             static_cast<double>(SDL_GetTicks() - t0) / 1000.0);
    return ok;
}

// Runs capture_cover on a low priority thread next to the game
struct CoverJob {
    std::thread th;
    std::atomic<bool> cancel{false}, done{false};
    bool ok = false, handled = false;

    void start(fs::path disc_path, fs::path sbi_path, fs::path out) {
        if (th.joinable()) {
            if (!done.load()) return;
            th.join();
        }
        cancel.store(false);
        done.store(false);
        ok = handled = false;
        th = std::thread([this, disc_path, sbi_path, out] {
            SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
            ok = capture_cover(disc_path, sbi_path, out, cancel);
            done.store(true);
        });
    }
    bool running() const { return th.joinable() && !done.load(); }
    void stop() {
        cancel.store(true);
        if (th.joinable()) th.join();
    }
    ~CoverJob() { stop(); }
};

#ifdef SDL_PLATFORM_ANDROID
// Asks the launcher to add a shortcut with this picture (the phone asks the player first).
// VagrantStoryActivity.pinShortcut; false when the launcher cannot pin shortcuts.
bool android_pin_shortcut(const fs::path& png) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto act = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !act) return false;
    bool ok = false;
    jclass cls = env->GetObjectClass(act);
    if (jmethodID m = env->GetMethodID(cls, "pinShortcut", "(Ljava/lang/String;)Z")) {
        jstring s = env->NewStringUTF(path_to_utf8(png).c_str());
        ok = env->CallBooleanMethod(act, m, s) == JNI_TRUE;
        env->DeleteLocalRef(s);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        ok = false;
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(act);
    return ok;
}
#endif

struct App {
    Video video;
    Audio audio;
    Gamepads gamepads;
    TouchPad touch;
    launcher::GameSettings settings;
    bool have_ini = false;
    fs::path data;

    CoverJob cover; // Android: home screen shortcut picture

    // Command line (tests)
    fs::path disc_arg, sbi_arg, load_state_arg, window_dump_path, cover_arg, cover_bg_arg;
    u32 window_dump_frame = 0;
    bool hidden = false, no_throttle = false;
    bool test_menu = false; // --show-menu: the menu is open from the start (screenshots)
    int scale_arg = 0;

    bool quit = false;
    std::string notice;
    u64 notice_until = 0;

    void show_notice(const std::string& t, u64 ms = 2500) {
        notice = t;
        notice_until = SDL_GetTicks() + ms;
    }
    void save_settings() const { launcher::save_settings(launcher::settings_path(), settings); }

    fs::path cover_png() const { return data / "kapak.png"; }
    // Android: asks for the Rood Inverse shortcut on the home screen (made first if needed)
    void offer_shortcut() {
#ifdef SDL_PLATFORM_ANDROID
        std::error_code ec;
        if (!fs::exists(cover_png(), ec)) {
            if (!cover.running()) cover.start(settings.disc, settings.sbi, cover_png());
            show_notice("Simge hazirlaniyor (yarim dakika kadar)", 4000);
            return;
        }
        if (android_pin_shortcut(cover_png())) show_notice("Ana ekrana Rood Inverse simgesi eklensin mi?", 6000);
        else show_notice("Bu telefon ana ekrana kisayol eklemeyi desteklemiyor", 5000);
#endif
    }

    void draw_notice(const Draw& d) {
        if (notice.empty()) return;
        if (SDL_GetTicks() >= notice_until) { notice.clear(); return; }
        const float size = std::clamp(std::floor(d.h / 34.0f), 10.0f, 36.0f);
        const float w = Draw::text_w(notice, size) + size * 2;
        // Bottom centre; with the touch buttons on screen, at the top (under the menu button)
        const float y = touch.visible ? std::min(d.w, d.h) * 0.12f : d.h - d.h * 0.03f - size * 2;
        d.fill((d.w - w) / 2, y, w, size * 2, {0, 0, 0, 170});
        d.text_centered(d.w / 2, y + size * 0.5f, notice, size, {255, 255, 255, 255});
    }

    // Input events shared by every screen. Returns the menu action they mean (if any).
    MenuInput handle_event(const SDL_Event& e, bool& menu_request, float& tap_x, float& tap_y) {
        switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_TERMINATING: quit = true; break;
            case SDL_EVENT_GAMEPAD_ADDED: gamepads.add(e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: gamepads.remove(e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (settings.touch_controls == 0) touch.visible = false;
                switch (e.gbutton.button) {
                    case SDL_GAMEPAD_BUTTON_DPAD_UP: return MenuInput::Up;
                    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return MenuInput::Down;
                    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return MenuInput::Left;
                    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return MenuInput::Right;
                    case SDL_GAMEPAD_BUTTON_SOUTH: return MenuInput::Ok;
                    case SDL_GAMEPAD_BUTTON_EAST: return MenuInput::Back;
                    case SDL_GAMEPAD_BUTTON_GUIDE: menu_request = true; break;
                    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
                    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: {
                        // L3 + R3 together: menu (Steam Deck / pads without a free button)
                        SDL_Gamepad* g = SDL_GetGamepadFromID(e.gbutton.which);
                        if (g && SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_STICK) && SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) {
                            menu_request = true;
                        }
                        break;
                    }
                    default: break;
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                if (settings.touch_controls == 0) touch.visible = false;
                switch (e.key.scancode) {
                    case SDL_SCANCODE_UP: return MenuInput::Up;
                    case SDL_SCANCODE_DOWN: return MenuInput::Down;
                    case SDL_SCANCODE_LEFT: return MenuInput::Left;
                    case SDL_SCANCODE_RIGHT: return MenuInput::Right;
                    case SDL_SCANCODE_RETURN:
                        if (e.key.mod & SDL_KMOD_ALT) { toggle_fullscreen(); break; }
                        return MenuInput::Ok;
                    case SDL_SCANCODE_Z: return MenuInput::Ok;
                    case SDL_SCANCODE_BACKSPACE:
                    case SDL_SCANCODE_X: return MenuInput::Back;
                    case SDL_SCANCODE_ESCAPE:
                    case SDL_SCANCODE_AC_BACK: menu_request = true; break;
                    case SDL_SCANCODE_F11: toggle_fullscreen(); break;
                    default: break;
                }
                break;
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_MOTION: {
                const Draw d = video.draw();
                const float x = e.tfinger.x * d.w, y = e.tfinger.y * d.h;
                touch.fingers[e.tfinger.fingerID] = {x, y};
                if (e.type == SDL_EVENT_FINGER_DOWN) {
                    if (settings.touch_controls != 2) touch.visible = true;
                    tap_x = x;
                    tap_y = y;
                    const auto l = TouchPad::layout(d.w, d.h);
                    if (touch.visible && TouchPad::in_rect(l.menu, x, y, 0.02f * l.u)) menu_request = true;
                }
                break;
            }
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_CANCELED: touch.fingers.erase(e.tfinger.fingerID); break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.which != SDL_TOUCH_MOUSEID && e.button.button == SDL_BUTTON_LEFT) {
                    float px = e.button.x, py = e.button.y;
                    SDL_RenderCoordinatesFromWindow(video.renderer, e.button.x, e.button.y, &px, &py);
                    tap_x = px;
                    tap_y = py;
                }
                break;
            default: break;
        }
        return MenuInput::None;
    }

    void toggle_fullscreen() {
        settings.fullscreen = !settings.fullscreen;
        SDL_SetWindowFullscreen(video.window, settings.fullscreen);
    }

    // Finds the disc to play: from the settings, else chosen by the player. False = quit.
    bool choose_disc(const std::string& problem) {
#ifdef SDL_PLATFORM_ANDROID
        Importer imp;
        Menu menu;
        menu.title = "Vagrant Story";
        menu.footer = problem.empty() ? "Kendi diskinizin imaji gerekir" : problem;
        bool picking = false;
        const fs::path disc_dir = data / "disk";
        const auto build = [&]() {
            std::vector<MenuItem> items;
            if (imp.running) {
                items.push_back({"Kopyalaniyor...", [&] {
                                     const u64 t = imp.total.load();
                                     return t ? std::to_string(static_cast<int>(imp.done.load() * 100 / t)) + "%" : std::string("...");
                                 }, nullptr});
                return items;
            }
            items.push_back({"Disk imaji sec (.bin + .sbi)", nullptr, [&](int) {
                                 picking = true;
                                 {
                                     std::lock_guard<std::mutex> lock(imp.m);
                                     imp.answered = imp.cancelled = false;
                                     imp.picked.clear();
                                 }
                                 SDL_ShowOpenFileDialog(Importer::on_files, &imp, video.window, nullptr, 0, nullptr, true);
                             }});
            items.push_back({"Cikis", nullptr, [&](int) { quit = true; }});
            return items;
        };
        while (!quit) {
            {
                std::lock_guard<std::mutex> lock(imp.m);
                if (picking && imp.answered) {
                    picking = false;
                    if (!imp.cancelled) {
                        imp.result_disc.clear();
                        imp.result_sbi.clear();
                        imp.error.clear();
                        if (imp.worker.joinable()) imp.worker.join();
                    }
                }
            }
            if (!picking && !imp.running && !imp.finished && !imp.picked.empty() && !imp.cancelled) {
                imp.start(disc_dir);
            }
            if (imp.finished) {
                imp.finished = false;
                if (imp.worker.joinable()) imp.worker.join();
                std::lock_guard<std::mutex> lock(imp.m);
                imp.picked.clear();
                if (!imp.result_disc.empty()) {
                    settings.disc = path_from_utf8(imp.result_disc);
                    settings.sbi = imp.result_sbi.empty() ? fs::path() : path_from_utf8(imp.result_sbi);
                    save_settings();
                    host_log("Disk kopyalandi: %s", imp.result_disc.c_str());
                    return true;
                }
                menu.footer = imp.error;
            }
            SDL_Event e;
            bool menu_request = false;
            while (SDL_PollEvent(&e)) {
                float tx = -1, ty = -1;
                const MenuInput in = handle_event(e, menu_request, tx, ty);
                const auto items = build();
                if (tx >= 0) menu.tap(tx, ty, items);
                else if (in != MenuInput::None && in != MenuInput::Back) menu.input(in, items);
                else if (in == MenuInput::Back || menu_request) quit = true;
            }
            const Draw d = video.draw();
            SDL_SetRenderDrawColor(video.renderer, 8, 8, 12, 255);
            SDL_RenderClear(video.renderer);
            menu.draw(d, build());
            SDL_RenderPresent(video.renderer);
            SDL_Delay(16);
        }
        return false;
#else
        // Desktop / Steam Deck: a file browser inside the window (works in Steam's Game Mode)
        std::error_code ec;
        fs::path dir;
        if (!settings.disc.empty()) dir = settings.disc.parent_path();
        if (dir.empty() || !fs::is_directory(dir, ec)) {
            const char* home = SDL_GetUserFolder(SDL_FOLDER_HOME);
            dir = home ? path_from_utf8(home) : fs::current_path(ec);
        }
        Menu menu;
        menu.footer = problem.empty() ? "Vagrant Story disk imajini secin (.cue / .bin / .iso)" : problem;
        bool done = false, chosen = false;
        std::vector<fs::path> entries;
        fs::path listed;
        const auto relist = [&]() {
            entries.clear();
            std::vector<fs::path> dirs, files;
            for (auto it = fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
                 !ec && it != fs::directory_iterator(); it.increment(ec)) {
                const std::string name = path_to_utf8(it->path().filename());
                if (name.empty() || name[0] == '.') continue;
                std::error_code e2;
                if (it->is_directory(e2)) dirs.push_back(it->path());
                else if (is_disc_file(it->path())) files.push_back(it->path());
            }
            ec.clear();
            std::sort(dirs.begin(), dirs.end());
            std::sort(files.begin(), files.end());
            entries = files;
            entries.insert(entries.end(), dirs.begin(), dirs.end());
            listed = dir;
            menu.sel = menu.top = 0;
        };
        const auto build = [&]() {
            if (listed != dir) relist();
            std::vector<MenuItem> items;
            menu.title = ascii(path_to_utf8(dir));
            if (menu.title.size() > 40) menu.title = "..." + menu.title.substr(menu.title.size() - 37);
            if (dir.has_parent_path() && dir.parent_path() != dir) {
                items.push_back({"[..] Ust klasor", nullptr, [&](int) { dir = dir.parent_path(); }});
            }
            for (const fs::path& p : entries) {
                std::error_code e2;
                const bool is_dir = fs::is_directory(p, e2);
                const std::string name = path_to_utf8(p.filename());
                items.push_back({is_dir ? "[" + name + "]" : name, nullptr, [&, p, is_dir](int) {
                                     if (is_dir) {
                                         dir = p;
                                     } else {
                                         settings.disc = p;
                                         settings.sbi.clear();
                                         chosen = done = true;
                                     }
                                 }});
            }
            items.push_back({"Cikis", nullptr, [&](int) { quit = true; }});
            return items;
        };
        while (!done && !quit) {
            SDL_Event e;
            bool menu_request = false;
            while (SDL_PollEvent(&e) && !done) {
                float tx = -1, ty = -1;
                const MenuInput in = handle_event(e, menu_request, tx, ty);
                const auto items = build();
                if (tx >= 0) menu.tap(tx, ty, items);
                else if (in == MenuInput::Back) {
                    if (dir.has_parent_path() && dir.parent_path() != dir) dir = dir.parent_path();
                } else if (in != MenuInput::None) menu.input(in, items);
            }
            if (done || quit) break;
            const Draw d = video.draw();
            SDL_SetRenderDrawColor(video.renderer, 8, 8, 12, 255);
            SDL_RenderClear(video.renderer);
            menu.draw(d, build());
            SDL_RenderPresent(video.renderer);
            SDL_Delay(16);
        }
        if (chosen) {
            save_settings();
            host_log("Disk secildi: %s", path_to_utf8(settings.disc).c_str());
        }
        return chosen;
#endif
    }

    enum class GameEnd { Quit, ChangeDisc, Error };
    std::string last_error;

    GameEnd run_game();
};

App::GameEnd App::run_game() {
    const auto fail = [&](const std::string& m) {
        last_error = m;
        host_log("HATA: %s", m.c_str());
        return GameEnd::Error;
    };

    PsxRuntime rt(PSX_RAM_SIZE_2MB);
    CpuContext ctx{};
    DiscImage disc;
    const int max_scale = 4; // Drawn by the processor
    const u32 scale = static_cast<u32>(std::clamp(scale_arg > 0 ? scale_arg : settings.internal_scale, 1, max_scale));
    rt.gpu().set_internal_scale(scale);
    rt.set_pal60(settings.pal60);
    rt.set_cpu_overclock(static_cast<u32>(settings.cpu_overclock));
    rt.set_pad_dualshock(settings.rumble);
    rt.gpu().set_texture_filtering(settings.texture_filter != 0);
    rt.gpu().set_texture_filter_sharp(settings.texture_filter == 2);
    rt.spu().set_reverb_enabled(settings.reverb);
    rt.spu().set_cubic_interpolation(settings.audio_cubic);
    rt.pgxp().set_enabled(settings.pgxp > 0 && scale > 1u);
    rt.gpu().set_perspective_textures(settings.pgxp >= 2);
    rt.gpu().set_rim_light(static_cast<PsxGpu::RimLight>(std::clamp(settings.rim_light, 0, 2)));

    CheatList cheats;
    if (settings.cheats) {
        std::ifstream in(launcher::cheats_path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        cheats.parse(text, nullptr);
        host_log("Hile kodlari: %zu acik", cheats.enabled_count());
    }
    TextureReplacer texrep;
    if (settings.texture_pack && scale > 1u) {
        const std::size_t n = texrep.scan(launcher::texture_pack_dir());
        if (n) host_log("Doku paketi: %zu degistirme dokusu", n);
    }
    if (settings.texture_dump && scale > 1u) texrep.set_dump_dir(launcher::texture_pack_dir() / "_dokum");
    if (texrep.active()) rt.gpu().set_texture_replacer(&texrep);

#ifdef VSPORT_HAS_AOT_MODULES
    host_log("Derlenmis kod: %zu fonksiyon", register_all_generated_modules(rt));
#endif

    std::string err;
    if (!disc.open(settings.disc, &err)) return fail("Disk acilamadi: " + err);
    if (!settings.sbi.empty() && !disc.load_sbi(settings.sbi, &err)) return fail(err);
    if (disc.subq_override_count() == 0 && disc.volume_id() == "SLES-02754") {
        host_log("UYARI: PAL disk icin .sbi dosyasi yok; oyun New Game'den sonra durur");
        show_notice("PAL disk: .sbi dosyasi bulunamadi", 6000);
    }
    rt.attach_disc(&disc);
    const fs::path memcard = settings.memcard.empty() ? launcher::default_memcard_path() : settings.memcard;
    {
        std::error_code ec;
        fs::create_directories(memcard.parent_path(), ec);
    }
    if (!rt.attach_memory_card(0, memcard, &err)) {
        host_log("Hafiza karti acilamadi: %s", err.c_str());
        show_notice("Hafiza karti acilamadi", 6000);
    }
    const auto cnf = disc.parse_system_cnf();
    if (!cnf) return fail("Diskte SYSTEM.CNF yok; bu bir PlayStation diski degil gibi.");
    PsxExeImage exe{};
    if (!parse_psx_exe(disc.extract_file(cnf->boot_path), exe, &err)) return fail("PS-X EXE okunamadi: " + err);
    rt.load_exe(exe, ctx);
    host_log("Yuklendi: %s (%s, %ux ic cozunurluk)", cnf->boot_path.c_str(), disc.volume_id().c_str(), scale);
    {
        // Android, first start: the Rood Inverse for the home screen shortcut, made in the
        // background while the game runs (elsewhere only for tests)
        std::error_code ec;
#ifdef SDL_PLATFORM_ANDROID
        const bool want_cover = !hidden && !fs::exists(cover_png(), ec);
        const fs::path cover_out = cover_png();
#else
        const bool want_cover = !cover_bg_arg.empty();
        const fs::path cover_out = cover_bg_arg;
#endif
        if (want_cover && !cover.running()) cover.start(settings.disc, settings.sbi, cover_out);
    }

    gamepads.map = settings.pad_map;
    audio.muted = settings.muted || hidden;
    audio.volume = settings.volume;
    audio.apply_gain();

    ra::Achievements achievements;
    if (settings.ra_enabled && !settings.ra_token.empty() && !hidden) {
        achievements.start(rt, settings.ra_user, settings.ra_token, settings.disc,
                           [&](const std::string& m) { show_notice(m, 5000); host_log("%s", m.c_str()); },
                           [&](const std::string& m) { host_log("%s", m.c_str()); });
    }

    // Save states next to the memory card (slot 0 = "continue where you left off")
    enum StateOp { OP_NONE, OP_SAVE, OP_LOAD };
    StateOp state_op = OP_NONE;
    int slot = 1;
    const fs::path state_dir = memcard.parent_path();
    const auto state_file = [&](int s) {
        return state_dir / (s == 0 ? std::string("VagrantStory_devam.vss") : "VagrantStory_durum" + std::to_string(s) + ".vss");
    };
    const auto save_state_file = [&](const fs::path& path, const std::string& what) {
        const std::vector<u8> st = rt.save_state(ctx);
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(st.data()), static_cast<std::streamsize>(st.size()));
        const bool ok = static_cast<bool>(out);
        host_log("%s: %s", what.c_str(), ok ? "kaydedildi" : "KAYDEDILEMEDI");
        show_notice(what + (ok ? " kaydedildi" : " KAYDEDILEMEDI"));
        return ok;
    };
    const auto load_state_file = [&](const fs::path& path, const std::string& what) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            show_notice(what + ": kayit yok");
            return false;
        }
        std::vector<u8> data_bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const bool ok = rt.load_state(std::move(data_bytes), ctx);
        host_log("%s: %s", what.c_str(), ok ? "yuklendi" : "YUKLENEMEDI");
        show_notice(what + (ok ? " yuklendi" : " yuklenemedi"));
        return ok;
    };
    const bool autosave_on = settings.autosave && !hidden;
    const bool rewind_on = settings.rewind;
    std::deque<std::vector<u8>> rewind_states;
    bool snapshot_due = false, rewinding = false, autosave_due = false;
    const int runahead = std::clamp(settings.runahead, 0, 3);
    int ra_phase = 0;
    u32 ra_real_vb = 0;
    std::vector<u8> ra_state;

    bool running = true;
    GameEnd end = GameEnd::Quit;
    bool menu_open = false, backgrounded = false;
    Menu menu;
    menu.title = "Vagrant Story";
    menu.footer = "Menu: Esc / L3+R3 / ekrandaki = tusu";
    u16 touch_held = 0;
    u64 next_frame = SDL_GetTicksNS();
    int scale_choice = static_cast<int>(scale);

    const auto aspect_value = [&]() {
        return launcher::ASPECT_VALUES[std::clamp(settings.aspect, 0, launcher::ASPECT_COUNT - 1)];
    };
    const auto apply_widening = [&](const Draw& d) {
        float a = aspect_value();
        if (a <= 0.0f) a = d.h > 0 ? d.w / d.h : 4.0f / 3.0f;
        const float k = settings.widescreen ? std::clamp((4.0f / 3.0f) / a, 0.25f, 1.0f) : 1.0f;
        if (rt.gte().widescreen_scale() != k) rt.gte().set_widescreen_scale(k);
    };
    const auto request = [&](StateOp op) {
        state_op = op;
        rt.request_break();
    };
    const auto close_menu = [&]() {
        menu_open = false;
        audio.pause(false);
        next_frame = SDL_GetTicksNS();
    };
    const auto pause_items = [&]() {
        std::vector<MenuItem> it;
        it.push_back({"Devam et", nullptr, [&](int) { close_menu(); }});
        it.push_back({"Durumu kaydet", [&] { return "yuva " + std::to_string(slot); }, nullptr});
        it.back().act = [&](int dir) {
            if (dir == 0 || dir == 1) { request(OP_SAVE); close_menu(); }
        };
        it.push_back({"Durumu yukle", [&] { return "yuva " + std::to_string(slot); }, nullptr});
        it.back().act = [&](int dir) {
            if (dir == 0 || dir == 1) { request(OP_LOAD); close_menu(); }
        };
        it.push_back({"Durum yuvasi", [&] { return std::to_string(slot); }, [&](int dir) { slot = (slot - 1 + (dir < 0 ? 4 : 1)) % 5 + 1; }});
        it.push_back({"Ekran orani", [&] { return std::string(launcher::ASPECT_NAMES[std::clamp(settings.aspect, 0, launcher::ASPECT_COUNT - 1)]); },
                      [&](int dir) { settings.aspect = (settings.aspect + (dir < 0 ? launcher::ASPECT_COUNT - 1 : 1)) % launcher::ASPECT_COUNT; }});
        it.push_back({"3D genislet", [&] { return std::string(settings.widescreen ? "Acik" : "Kapali"); },
                      [&](int) { settings.widescreen = !settings.widescreen; }});
        it.push_back({"Goruntu", [&] { return std::string(settings.filter == 0 ? "Keskin" : "Yumusak"); }, [&](int) {
                          settings.filter = settings.filter == 0 ? 1 : 0;
                          video.set_filter(settings.filter);
                      }});
        it.push_back({"Ic cozunurluk", [&] { return std::to_string(scale_choice) + "x"; }, [&](int dir) {
                          scale_choice = std::clamp(scale_choice + (dir < 0 ? -1 : 1), 1, max_scale);
                          settings.internal_scale = scale_choice;
                          rt.gpu().set_internal_scale(static_cast<u32>(scale_choice));
                          rt.pgxp().set_enabled(settings.pgxp > 0 && scale_choice > 1);
                      }});
        it.push_back({"PAL 60 Hz", [&] { return std::string(settings.pal60 ? "Acik" : "Kapali"); }, [&](int) {
                          settings.pal60 = !settings.pal60;
                          rt.set_pal60(settings.pal60);
                      }});
        it.push_back({"Ses", [&] { return std::string(audio.muted ? "Kapali" : "Acik"); }, [&](int) {
                          audio.muted = !audio.muted;
                          settings.muted = audio.muted;
                          audio.apply_gain();
                      }});
        it.push_back({"Ses duzeyi", [&] { return std::to_string(settings.volume) + "%"; }, [&](int dir) {
                          settings.volume = std::clamp(settings.volume + (dir < 0 ? -10 : 10), 0, 100);
                          audio.volume = settings.volume;
                          audio.apply_gain();
                      }});
        it.push_back({"Dokunmatik tuslar", [&] {
                          static const char* names[3] = {"Otomatik", "Her zaman", "Kapali"};
                          return std::string(names[std::clamp(settings.touch_controls, 0, 2)]);
                      },
                      [&](int dir) {
                          settings.touch_controls = (settings.touch_controls + (dir < 0 ? 2 : 1)) % 3;
                          touch.visible = settings.touch_controls == 1;
                      }});
#ifndef SDL_PLATFORM_ANDROID
        it.push_back({"Tam ekran", [&] { return std::string(settings.fullscreen ? "Acik" : "Kapali"); }, [&](int) { toggle_fullscreen(); }});
#endif
#ifdef SDL_PLATFORM_ANDROID
        it.push_back({"Ana ekran simgesi", nullptr, [&](int dir) {
                          if (dir == 0) { close_menu(); offer_shortcut(); }
                      }});
#endif
        it.push_back({"Disk degistir", nullptr, [&](int) {
                          end = GameEnd::ChangeDisc;
                          running = false;
                          close_menu();
                          rt.request_break();
                      }});
        it.push_back({"Cikis", nullptr, [&](int) {
                          running = false;
                          close_menu();
                          rt.request_break();
                      }});
        return it;
    };

    const auto render = [&](u32 shown_vb) {
        const Draw d = video.draw();
        apply_widening(d);
        video.upload(rt.gpu(), settings.filter);
        SDL_SetRenderDrawColor(video.renderer, 0, 0, 0, 255);
        SDL_RenderClear(video.renderer);
        if (video.have_picture) {
            const SDL_FRect dst = Video::fit(d.w, d.h, aspect_value(), settings.integer_scale, rt.gpu().display_height());
            SDL_RenderTexture(video.renderer, video.tex, nullptr, &dst);
        }
        touch.draw(d, touch_held);
        draw_notice(d);
        if (menu_open) menu.draw(d, pause_items());
        if (!window_dump_path.empty() && shown_vb >= window_dump_frame) {
            if (SDL_Surface* s = SDL_RenderReadPixels(video.renderer, nullptr)) {
                const bool ok = write_ppm(window_dump_path, s);
                SDL_DestroySurface(s);
                host_log("Pencere goruntusu %s: %s (kare %u)", ok ? "kaydedildi" : "KAYDEDILEMEDI", path_to_utf8(window_dump_path).c_str(), shown_vb);
            }
            window_dump_path.clear();
            running = false;
            rt.request_break();
        }
        SDL_RenderPresent(video.renderer);
    };

    // Events; while the menu is open (or the app is in the background) the game waits here
    const auto pump_events = [&]() {
        for (;;) {
            SDL_Event e;
            bool menu_request = false;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_DID_ENTER_BACKGROUND || e.type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
                    if (!backgrounded) {
                        backgrounded = true;
                        audio.pause(true);
                        if (autosave_on) { autosave_due = true; rt.request_break(); }
                        save_settings();
                    }
                    continue;
                }
                if (e.type == SDL_EVENT_DID_ENTER_FOREGROUND) {
                    backgrounded = false;
                    if (!menu_open) audio.pause(false);
                    next_frame = SDL_GetTicksNS();
                    continue;
                }
                float tx = -1, ty = -1;
                const MenuInput in = handle_event(e, menu_request, tx, ty);
                if (quit) {
                    running = false;
                    rt.request_break();
                }
                if (menu_open) {
                    const auto items = pause_items();
                    if (tx >= 0) menu.tap(tx, ty, items);
                    else if (in == MenuInput::Back || menu_request) close_menu();
                    else if (in != MenuInput::None) menu.input(in, items);
                    continue;
                }
                if (menu_request && !hidden) {
                    menu_open = true;
                    menu.sel = 0;
                    audio.pause(true);
                    continue;
                }
                if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                    switch (e.key.scancode) {
                        case SDL_SCANCODE_F5: request(OP_SAVE); break;
                        case SDL_SCANCODE_F7: request(OP_LOAD); break;
                        case SDL_SCANCODE_F6:
                            slot = slot % 5 + 1;
                            show_notice("Durum yuvasi " + std::to_string(slot));
                            break;
                        case SDL_SCANCODE_F3:
                            if (e.key.mod & SDL_KMOD_SHIFT) settings.widescreen = !settings.widescreen;
                            else settings.aspect = (settings.aspect + 1) % launcher::ASPECT_COUNT;
                            show_notice(std::string("Oran: ") + launcher::ASPECT_NAMES[settings.aspect] + (settings.widescreen ? " (3D genis)" : ""));
                            break;
                        case SDL_SCANCODE_F4:
                            settings.filter = settings.filter == 0 ? 1 : 0;
                            video.set_filter(settings.filter);
                            break;
                        case SDL_SCANCODE_M:
                            audio.muted = !audio.muted;
                            settings.muted = audio.muted;
                            audio.apply_gain();
                            show_notice(audio.muted ? "Ses kapali" : "Ses acik");
                            break;
                        case SDL_SCANCODE_F12: rt.gpu().dump_ppm(data / "vram_snapshot.ppm", true); break;
                        default: break;
                    }
                }
            }
            if (!running || !(menu_open || backgrounded)) return;
            if (autosave_due) return; // Saved by the main loop, then waits on here
            if (!backgrounded) render(rt.vblank_count());
            SDL_Delay(backgrounded ? 50 : 16);
        }
    };

    const auto present_and_pump = [&]() {
        pump_events();
        if (!running) return;
        if (cover.done.load() && !cover.handled) {
            cover.handled = true;
            if (cover.ok) offer_shortcut();
        }
        achievements.pump();
        if (runahead > 0) {
            rt.request_break();
            if (ra_phase > 0 && ra_phase < runahead) return;
        }
        const bool ra_show = runahead > 0 && ra_phase == runahead;
        if (!ra_show) {
            if (runahead > 0) ra_real_vb = rt.vblank_count();
            const u32 vb = rt.vblank_count();
            const bool* keys = SDL_GetKeyboardState(nullptr);
            rewinding = rewind_on && !hidden && keys[SDL_SCANCODE_F8];
            if (rewinding) rt.request_break();
            else if (rewind_on && vb % 50u == 0) { snapshot_due = true; rt.request_break(); }

            u16 pressed = hidden ? 0u : gamepads.poll();
            const Draw d = video.draw();
            touch_held = hidden ? 0u : touch.buttons(d.w, d.h);
            pressed |= touch_held;
            if (!hidden && (SDL_GetWindowFlags(video.window) & SDL_WINDOW_INPUT_FOCUS)) {
                struct KeyBind { SDL_Scancode sc; u16 bit; };
                constexpr KeyBind KEYS[] = {
                    {SDL_SCANCODE_RETURN, pad::START}, {SDL_SCANCODE_BACKSPACE, pad::SELECT},
                    {SDL_SCANCODE_UP, pad::UP}, {SDL_SCANCODE_RIGHT, pad::RIGHT}, {SDL_SCANCODE_DOWN, pad::DOWN}, {SDL_SCANCODE_LEFT, pad::LEFT},
                    {SDL_SCANCODE_Z, pad::CROSS}, {SDL_SCANCODE_X, pad::CIRCLE}, {SDL_SCANCODE_A, pad::SQUARE}, {SDL_SCANCODE_S, pad::TRIANGLE},
                    {SDL_SCANCODE_Q, pad::L1}, {SDL_SCANCODE_W, pad::R1}, {SDL_SCANCODE_E, pad::L2}, {SDL_SCANCODE_R, pad::R2},
                };
                for (const auto& k : KEYS) {
                    if (keys[k.sc]) pressed |= k.bit;
                }
            }
            rt.set_pad_buttons(static_cast<u16>(~pressed));
            cheats.apply(rt);
            if (settings.rumble && !hidden) gamepads.rumble(rt.pad_motor_small(), rt.pad_motor_large());
            if (runahead == 0) audio.pump(rt.spu());
            achievements.do_frame();
            if (runahead > 0) return;
        }
        render(runahead > 0 ? ra_real_vb : rt.vblank_count());
        if (!no_throttle) {
            const bool* keys = SDL_GetKeyboardState(nullptr);
            const u64 period = 1'000'000'000ull / (rt.video_60hz() ? 60u : 50u);
            const u64 now = SDL_GetTicksNS();
            if (keys[SDL_SCANCODE_TAB]) {
                next_frame = now;
            } else {
                next_frame += period;
                if (now > next_frame + period * 4) next_frame = now;
                else if (next_frame > now) SDL_DelayPrecise(next_frame - now);
            }
        }
    };

    rt.set_vblank_callback(present_and_pump);
    menu_open = test_menu;
    if (!load_state_arg.empty()) load_state_file(load_state_arg, "Test durumu");
    else if (autosave_on && fs::exists(state_file(0))) load_state_file(state_file(0), "Kaldiginiz yerden devam");
    host_log("Emulasyon basladi");

    while (!ctx.halted) {
        rt.run(ctx, 0x00FF'FFFF'FFFF'FFFFull);
        if (ctx.halted) break;
        if (runahead > 0 && running) {
            if (ra_phase == 0) {
                audio.pump(rt.spu());
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
            if (!rt.load_state_fast(ra_state, ctx)) host_log("Gecikme azaltma: durum geri yuklenemedi");
            rt.runahead_snapshot(true);
            ra_phase = 0;
        }
        if (!running) {
            if (autosave_on) save_state_file(state_file(0), "Otomatik kayit");
            break;
        }
        if (autosave_due) {
            autosave_due = false;
            save_state_file(state_file(0), "Otomatik kayit");
        }
        if (state_op == OP_SAVE) save_state_file(state_file(slot), "Durum " + std::to_string(slot));
        else if (state_op == OP_LOAD && load_state_file(state_file(slot), "Durum " + std::to_string(slot))) achievements.reset();
        state_op = OP_NONE;
        if (snapshot_due) {
            snapshot_due = false;
            rewind_states.push_back(rt.save_state(ctx));
            if (rewind_states.size() > 30) rewind_states.pop_front();
        }
        if (rewinding && !rewind_states.empty()) {
            std::vector<u8> st = rewind_states.back();
            if (rewind_states.size() > 1) rewind_states.pop_back();
            if (rt.load_state(std::move(st), ctx)) {
                show_notice("Geri sariliyor");
                achievements.reset();
            }
            SDL_Delay(120);
        }
    }
    if (ctx.halted) {
        host_log("Islemci durdu (PC=0x%08X)", ctx.pc);
        last_error = "Oyun durdu (emulasyon hatasi). Ayrinti: VagrantStory.log";
        end = GameEnd::Error;
    }
    gamepads.rumble(false, 0);
    if (achievements.active()) achievements.shutdown();
    rt.set_vblank_callback(nullptr);
    return end;
}

fs::path find_data_dir() {
#ifdef SDL_PLATFORM_ANDROID
    // /sdcard/Android/data/<app>/files: reachable over USB and with file managers
    if (const char* ext = SDL_GetAndroidExternalStoragePath()) return path_from_utf8(ext);
    if (const char* in = SDL_GetAndroidInternalStoragePath()) return path_from_utf8(in);
    return {};
#else
    // Portable: an ini next to the program wins; else the user's data folder
    // (~/.local/share/VagrantStory on Linux)
    std::error_code ec;
    if (const char* base = SDL_GetBasePath()) {
        const fs::path b = path_from_utf8(base);
        if (fs::exists(b / "VagrantStory.ini", ec)) return b;
#ifdef _WIN32
        return b;
#endif
    }
    if (char* pref = SDL_GetPrefPath("", "VagrantStory")) {
        fs::path p = path_from_utf8(pref);
        SDL_free(pref);
        return p;
    }
    return fs::current_path(ec);
#endif
}

} // namespace

int main(int argc, char** argv) {
    App app;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if ((a == "--disc" || a == "-d") && i + 1 < argc) app.disc_arg = path_from_utf8(argv[++i]);
        else if (a == "--sbi" && i + 1 < argc) app.sbi_arg = path_from_utf8(argv[++i]);
        else if (a == "--load-state" && i + 1 < argc) app.load_state_arg = path_from_utf8(argv[++i]);
        else if (a == "--scale" && i + 1 < argc) app.scale_arg = std::atoi(argv[++i]);
        else if (a == "--hidden") app.hidden = true;
        else if (a == "--show-menu") app.test_menu = true;
        else if (a == "--show-touch") app.touch.visible = true;
        else if (a == "--no-throttle") app.no_throttle = true;
        else if (a == "--window-dump" && i + 2 < argc) {
            app.window_dump_path = path_from_utf8(argv[++i]);
            app.window_dump_frame = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--capture-cover" && i + 1 < argc) {
            app.cover_arg = path_from_utf8(argv[++i]); // Test: write the shortcut picture and quit
        } else if (a == "--cover-in-background" && i + 1 < argc) {
            app.cover_bg_arg = path_from_utf8(argv[++i]); // Test: same, next to the running game
        } else if (a == "--version") {
            std::printf("Vagrant Story Native Port %s (SDL %d.%d.%d)\n", VERSION, SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);
            return 0;
        }
    }

    SDL_SetAppMetadata("Vagrant Story", VERSION, "io.github.aligothenburg.vagrantstory");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");          // Touches are handled as touches
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");    // Back button opens the menu
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL baslatilamadi: %s\n", SDL_GetError());
        return 1;
    }

    app.data = find_data_dir();
    {
        std::error_code ec;
        fs::create_directories(app.data, ec);
    }
#ifndef _WIN32
    launcher::set_data_dir(app.data);
#endif
    g_log = std::fopen(path_to_utf8(app.data / "VagrantStory.log").c_str(), "w");
    host_log("Vagrant Story Native Port %s, SDL %d.%d.%d, %s", VERSION, SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION, SDL_GetPlatform());
    host_log("Veri klasoru: %s", path_to_utf8(app.data).c_str());

    {
        std::error_code ec;
        app.have_ini = fs::exists(launcher::settings_path(), ec);
    }
    app.settings = launcher::load_settings(launcher::settings_path());
    if (!app.have_ini) {
        // First start: defaults that suit the device (the processor draws the picture)
#ifdef SDL_PLATFORM_ANDROID
        app.settings.internal_scale = 1;
        app.settings.fullscreen = true;
        app.settings.filter = 1;
#else
        const char* deck = SDL_getenv("SteamDeck");
        if (deck && std::strcmp(deck, "1") == 0) app.settings.fullscreen = true;
#endif
        app.settings.rumble = true;
    }
    if (!app.disc_arg.empty()) {
        app.settings.disc = app.disc_arg;
        app.settings.sbi = app.sbi_arg;
    }
    if (app.settings.touch_controls == 1) app.touch.visible = true;
    if (!app.cover_arg.empty()) {
        const std::atomic<bool> no_cancel{false};
        const bool ok = capture_cover(app.settings.disc, app.settings.sbi, app.cover_arg, no_cancel);
        SDL_Quit();
        if (g_log) std::fclose(g_log);
        return ok ? 0 : 1;
    }

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (app.hidden) flags |= SDL_WINDOW_HIDDEN;
    if (app.settings.fullscreen && !app.hidden) flags |= SDL_WINDOW_FULLSCREEN;
    const int wh = std::clamp(app.settings.window_height, 240, 2160);
    app.video.window = SDL_CreateWindow("Vagrant Story", launcher::window_width_for(wh, app.settings.aspect), wh, flags);
    if (!app.video.window) {
        host_log("Pencere acilamadi: %s", SDL_GetError());
        return 1;
    }
    app.video.renderer = SDL_CreateRenderer(app.video.window, nullptr);
    if (!app.video.renderer) {
        host_log("Cizici acilamadi: %s", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(app.video.renderer, 0); // Pacing comes from the game's own frame timing
    SDL_SetRenderDrawBlendMode(app.video.renderer, SDL_BLENDMODE_BLEND);
    host_log("Cizici: %s", SDL_GetRendererName(app.video.renderer));
    SDL_DisableScreenSaver();
    if (app.audio.open()) host_log("Ses acik (44.1 kHz)");
    else host_log("Ses acilamadi: %s", SDL_GetError());

    std::string problem;
    for (;;) {
        std::error_code ec;
        const bool disc_ok = !app.settings.disc.empty() && fs::exists(app.settings.disc, ec);
        if (!disc_ok || !problem.empty()) {
            if (app.hidden) {
                host_log("Disk yok ya da acilamadi: %s", problem.c_str());
                break;
            }
            if (!app.choose_disc(problem)) break;
        }
        problem.clear();
        const App::GameEnd end = app.run_game();
        if (end == App::GameEnd::Error) {
            problem = app.last_error;
            continue;
        }
        if (end == App::GameEnd::ChangeDisc) {
            problem.clear();
            app.settings.disc.clear();
            continue;
        }
        break;
    }

    if (!app.hidden) app.save_settings();
    app.cover.stop();
    host_log("Kapaniyor");
    app.gamepads.close();
    app.audio.close();
    if (app.video.tex) SDL_DestroyTexture(app.video.tex);
    SDL_DestroyRenderer(app.video.renderer);
    SDL_DestroyWindow(app.video.window);
    SDL_Quit();
    if (g_log) std::fclose(g_log);
    return 0;
}
