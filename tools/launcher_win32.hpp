#pragma once

// Settings window shown when VagrantStory.exe starts: game folder / disc image / .sbi, display
// and sound options. Everything is stored in VagrantStory.ini next to the executable.

#include "vsport/pad_mapping.hpp"

#include <filesystem>
#include <string>

namespace vsport::launcher {

// Picture aspect ratios (index stored as "aspect" in the ini). 0.0 = stretch to fill the window.
constexpr int ASPECT_COUNT = 6;
constexpr float ASPECT_VALUES[ASPECT_COUNT] = {4.0f / 3.0f, 10.0f / 7.0f, 3.0f / 2.0f,
                                               16.0f / 10.0f, 16.0f / 9.0f, 0.0f};
constexpr const char* ASPECT_NAMES[ASPECT_COUNT] = {"4:3", "10:7", "3:2", "16:10", "16:9", "Doldur"};
constexpr int ASPECT_16_9 = 4;

// Window heights offered in the settings (client area; width follows the aspect ratio)
constexpr int WINDOW_HEIGHT_COUNT = 7;
constexpr int WINDOW_HEIGHTS[WINDOW_HEIGHT_COUNT] = {480, 720, 960, 1080, 1200, 1440, 2160};

// Window client width for a height and aspect index ("fill" windows open at 16:9)
[[nodiscard]] constexpr int window_width_for(int height, int aspect) noexcept {
    const float a = (aspect >= 0 && aspect < ASPECT_COUNT && ASPECT_VALUES[aspect] > 0.0f) ? ASPECT_VALUES[aspect]
                                                                                          : 16.0f / 9.0f;
    return static_cast<int>(static_cast<float>(height) * a + 0.5f);
}

struct GameSettings {
    std::filesystem::path disc;   // .cue / .bin / .iso
    std::filesystem::path sbi;    // LibCrypt subchannel data (PAL release)
    std::filesystem::path memcard; // Memory card 1 image (.mcr); empty = default_memcard_path()
    bool fullscreen     = false;
    int  window_height  = 720;    // Window client height (one of WINDOW_HEIGHTS)
    int  aspect         = 0;      // Index into ASPECT_VALUES
    int  internal_scale = 2;      // Rendering resolution: 1 = native 320x240, 2..6 = N x
    int  renderer       = 0;      // Upscaled picture drawn by: 0 = graphics card (Direct3D 11), 1 = processor
    std::string gpu;              // Graphics card name (UTF-8); empty = automatic (the most powerful one)
    int  texture_filter = 0;      // 3D texture filter (internal_scale >= 2): 0 = off, 1 = bilinear, 2 = sharp bilinear
    int  rim_light      = 0;      // Character rim light: 0 = soft, 1 = sharp, 2 = off
    int  pgxp           = 2;      // Vertex precision (internal res >= 2): 0 = off, 1 = no wobble, 2 = + perspective textures
    int  filter         = 0;      // 0 = sharp pixel, 1 = smooth bilinear, 2 = CRT
    bool integer_scale  = false;
    bool widescreen     = false;  // Widen the 3D view to the aspect ratio (GTE projection hack)
    bool muted          = false;
    int  volume         = 100;    // 0..100
    int  cpu_overclock  = 100;    // Emulated CPU speed in percent (100 = original)
    bool reverb         = true;   // PS1 reverb unit (echo)
    bool audio_cubic    = false;  // Cubic instead of linear sample interpolation
    bool cheats         = false;  // Apply the cheat codes in cheats_path()
    bool pal60          = false;  // PAL disc at 60 Hz (NTSC speed)
    int  runahead       = 0;      // Input lag reduction: vblanks run ahead (0..3)
    bool ra_enabled     = false;  // RetroAchievements (softcore)
    std::string ra_user;          // RetroAchievements user name
    std::string ra_token;         // Login token from the server (the password is never stored)
    bool frame_interp   = false;  // Frame interpolation: in-between pictures, 25 -> 50 fps (graphics card renderer)
    bool texture_pack   = true;   // Use replacement textures from texture_pack_dir() (if any)
    bool texture_dump   = false;  // Dump the game's textures (for making packs)
    bool autosave       = false;  // Save the machine state on exit and continue from it on the next start
    bool rewind         = false;  // Keep ~30 s of states in memory; hold F8 to rewind
    bool rumble         = true;   // DualShock vibration (XInput controllers)
    bool show_launcher  = true;   // Show this window on every start (Shift forces it)
    pad::ButtonMap pad_map = pad::default_button_map(); // Controller button remap
};

[[nodiscard]] std::filesystem::path settings_path();  // VagrantStory.ini next to the exe
[[nodiscard]] std::filesystem::path cheats_path();    // VagrantStory_hileler.txt next to the exe
// Texture pack folder next to the exe ("dokular"); dumps go to its "_dokum" subfolder
[[nodiscard]] std::filesystem::path texture_pack_dir();
[[nodiscard]] std::filesystem::path default_memcard_path(); // saves\VagrantStory_kart1.mcr next to the exe
[[nodiscard]] GameSettings load_settings(const std::filesystem::path& ini);
void save_settings(const std::filesystem::path& ini, const GameSettings& s);

// Shows the settings window. Returns true when the player pressed "Oyna" (settings are saved),
// false when the window was closed or "Cikis" was pressed.
[[nodiscard]] bool run_settings_window(GameSettings& s);

// Diagnostics: opens the settings window off-screen, saves each page as DIR\page_N.bmp, closes it
void dump_settings_window(GameSettings s, const std::filesystem::path& dir);

// Message box helper (UTF-8 text)
void show_message(const std::string& text, bool error);

} // namespace vsport::launcher
