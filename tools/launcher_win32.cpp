#include "launcher_win32.hpp"
#include "achievements.hpp"

#include "vsport/cdrom_iso.hpp"
#include "vsport/d3d11_hr_backend.hpp" // list_graphics_adapters

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <shellapi.h>

// Common Controls v6: themed (modern) buttons, combo boxes and sliders
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace vsport::launcher {

namespace {

std::filesystem::path path_from_utf8(const std::string& s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string path_to_utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

int clamp_int(int v, int lo, int hi) { return std::min(std::max(v, lo), hi); }

} // namespace

// ---------------------------------------------------------------------------------------------
// Settings file
// ---------------------------------------------------------------------------------------------

GameSettings load_settings(const std::filesystem::path& ini) {
    GameSettings s;
    std::ifstream in(ini, std::ios::binary);
    std::string line;
    int legacy_scale = 0;
    bool have_height = false, have_aspect = false;
    bool first_line = true;
    while (std::getline(in, line)) {
        if (first_line && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3); // UTF-8 BOM (some editors add it)
        first_line = false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        const int num = std::atoi(val.c_str());
        if (key == "disc")               s.disc = path_from_utf8(val);
        else if (key == "sbi")           s.sbi = path_from_utf8(val);
        else if (key == "memcard")       s.memcard = path_from_utf8(val);
        else if (key == "fullscreen")    s.fullscreen = num != 0;
        else if (key == "scale")         legacy_scale = clamp_int(num, 2, 9); // Older ini: 320x240 * scale
        else if (key == "window_height") { s.window_height = clamp_int(num, 240, 4320); have_height = true; }
        else if (key == "aspect")        { s.aspect = clamp_int(num, 0, ASPECT_COUNT - 1); have_aspect = true; }
        else if (key == "internal_scale") s.internal_scale = clamp_int(num, 1, 8);
        else if (key == "renderer")      s.renderer = clamp_int(num, 0, 1);
        else if (key == "gpu")           s.gpu = val;
        else if (key == "texture_filter") s.texture_filter = clamp_int(num, 0, 2);
        else if (key == "rim_light")     s.rim_light = clamp_int(num, 0, 2);
        else if (key == "pgxp")          s.pgxp = clamp_int(num, 0, 2);
        else if (key == "filter")        s.filter = clamp_int(num, 0, 2);
        else if (key == "integer_scale") s.integer_scale = num != 0;
        else if (key == "widescreen")    s.widescreen = num != 0;
        else if (key == "muted")         s.muted = num != 0;
        else if (key == "volume")        s.volume = clamp_int(num, 0, 100);
        else if (key == "show_launcher") s.show_launcher = num != 0;
        else if (key == "rumble")        s.rumble = num != 0;
        else if (key == "free_camera")   s.free_camera = num != 0;
        else if (key == "touch_controls") s.touch_controls = clamp_int(num, 0, 2);
        else if (key == "autosave")      s.autosave = num != 0;
        else if (key == "cheats")        s.cheats = num != 0;
        else if (key == "reverb")        s.reverb = num != 0;
        else if (key == "audio_cubic")   s.audio_cubic = num != 0;
        else if (key == "texture_pack")  s.texture_pack = num != 0;
        else if (key == "frame_interp")  s.frame_interp = num != 0;
        else if (key == "runahead")      s.runahead = clamp_int(num, 0, 3);
        else if (key == "pal60")         s.pal60 = num != 0;
        else if (key == "ra_enabled")    s.ra_enabled = num != 0;
        else if (key == "ra_user")       s.ra_user = val;
        else if (key == "ra_token")      s.ra_token = val;
        else if (key == "texture_dump")  s.texture_dump = num != 0;
        else if (key == "rewind")        s.rewind = num != 0;
        else if (key == "cpu_overclock") s.cpu_overclock = clamp_int(num, 100, 400);
        else if (key.rfind("pad_", 0) == 0) {
            const int target = pad::remap_index_from_name(key.substr(4));
            const int source = pad::remap_index_from_name(val);
            if (target >= 0 && source >= -1) s.pad_map[static_cast<std::size_t>(target)] = static_cast<s8>(source);
        }
    }
    if (!have_height && legacy_scale > 0) s.window_height = 240 * legacy_scale;
    if (!have_aspect && s.widescreen) s.aspect = ASPECT_16_9; // Older "16:9" checkbox meant both
    return s;
}

void save_settings(const std::filesystem::path& ini, const GameSettings& s) {
    std::ofstream out(ini, std::ios::binary | std::ios::trunc);
    out << "# Vagrant Story Native Port ayarlari (oyunu Shift ile baslatinca ayar penceresi acilir)\n";
    out << "disc=" << path_to_utf8(s.disc) << "\n";
    out << "sbi=" << path_to_utf8(s.sbi) << "\n";
    out << "memcard=" << path_to_utf8(s.memcard) << "\n";
    out << "fullscreen=" << (s.fullscreen ? 1 : 0) << "\n";
    out << "window_height=" << s.window_height << "\n";
    out << "# aspect: 0=4:3 1=10:7 2=3:2 3=16:10 4=16:9 5=pencereyi doldur\n";
    out << "aspect=" << s.aspect << "\n";
    out << "internal_scale=" << s.internal_scale << "\n";
    out << "# renderer: yuksek cozunurluk cizimi 0=ekran karti (Direct3D 11) 1=islemci\n";
    out << "renderer=" << s.renderer << "\n";
    out << "# gpu: ekran karti adi; bos = otomatik (en guclu)\n";
    out << "gpu=" << s.gpu << "\n";
    out << "# texture_filter: 3D doku filtresi 0=kapali 1=yumusak (bilinear) 2=keskin yumusatma\n";
    out << "texture_filter=" << s.texture_filter << "\n";
    out << "# rim_light: karakter kenar isigi 0=yumusak 1=keskin 2=kapali\n";
    out << "rim_light=" << s.rim_light << "\n";
    out << "# pgxp: kose hassasiyeti 0=kapali (orijinal) 1=titreme duzeltmesi 2=titreme duzeltmesi + perspektifli doku\n";
    out << "pgxp=" << s.pgxp << "\n";
    out << "filter=" << s.filter << "\n";
    out << "integer_scale=" << (s.integer_scale ? 1 : 0) << "\n";
    out << "widescreen=" << (s.widescreen ? 1 : 0) << "\n";
    out << "muted=" << (s.muted ? 1 : 0) << "\n";
    out << "volume=" << s.volume << "\n";
    out << "show_launcher=" << (s.show_launcher ? 1 : 0) << "\n";
    out << "# rumble: titresim (DualShock; XInput kontrolculer)\n";
    out << "rumble=" << (s.rumble ? 1 : 0) << "\n";
    out << "# free_camera: sag cubukla serbest kamera (klavye J/L/I/K)\n";
    out << "free_camera=" << (s.free_camera ? 1 : 0) << "\n";
    out << "# autosave: cikarken durumu kaydet, acilista kaldigin yerden devam et; rewind: F8 ile geri sarma\n";
    out << "autosave=" << (s.autosave ? 1 : 0) << "\n";
    out << "rewind=" << (s.rewind ? 1 : 0) << "\n";
    out << "# cheats: VagrantStory_hileler.txt dosyasindaki hile kodlarini uygula\n";
    out << "cheats=" << (s.cheats ? 1 : 0) << "\n";
    out << "# reverb: PS1 yanki birimi; audio_cubic: kubik ses ara degerlemesi\n";
    out << "reverb=" << (s.reverb ? 1 : 0) << "\n";
    out << "audio_cubic=" << (s.audio_cubic ? 1 : 0) << "\n";
    out << "# texture_pack: dokular klasorundeki degistirme dokulari; texture_dump: dokulari dokular\\_dokum klasorune dok\n";
    out << "texture_pack=" << (s.texture_pack ? 1 : 0) << "\n";
    out << "# frame_interp: ara kare uretimi (25 -> 50 fps, ekran karti cizimi)\n";
    out << "frame_interp=" << (s.frame_interp ? 1 : 0) << "\n";
    out << "# runahead: gecikme azaltma, ileri calistirilan kare sayisi (0-3)\n";
    out << "runahead=" << s.runahead << "\n";
    out << "# pal60: Avrupa (PAL) diskini 60 Hz'de, ABD surumu hizinda calistir\n";
    out << "pal60=" << (s.pal60 ? 1 : 0) << "\n";
    out << "# RetroAchievements: kullanici adi ve sunucunun verdigi oturum anahtari (sifre saklanmaz)\n";
    out << "ra_enabled=" << (s.ra_enabled ? 1 : 0) << "\n";
    out << "ra_user=" << s.ra_user << "\n";
    out << "ra_token=" << s.ra_token << "\n";
    out << "texture_dump=" << (s.texture_dump ? 1 : 0) << "\n";
    out << "# touch_controls: ekran dokunmatik tuslari (Android/Linux) 0=ekrana dokununca 1=her zaman 2=hic\n";
    out << "touch_controls=" << s.touch_controls << "\n";
    out << "# cpu_overclock: emule edilen islemci hizi, yuzde (100 = orijinal)\n";
    out << "cpu_overclock=" << s.cpu_overclock << "\n";
    out << "# Kontrolcu tuslari: pad_<PS1 tusu>=<kontrolcude o konumdaki tus> (none = atanmamis)\n";
    for (std::size_t i = 0; i < pad::REMAP_COUNT; ++i) {
        const int src = s.pad_map[i];
        out << "pad_" << pad::REMAP_NAMES[i] << "="
            << (src >= 0 && src < static_cast<int>(pad::REMAP_COUNT) ? pad::REMAP_NAMES[src] : "none") << "\n";
    }
}

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), n);
    return out;
}

// ---------------------------------------------------------------------------------------------
// File / folder pickers
// ---------------------------------------------------------------------------------------------

std::filesystem::path pick_file(HWND owner, const wchar_t* title, const wchar_t* filter,
                                const std::filesystem::path& start) {
    std::wstring buf(32768, L'\0');
    std::wstring dir = start.empty() ? std::wstring() : start.parent_path().wstring();
    OPENFILENAMEW ofn{};
    ofn.lStructSize     = sizeof(ofn);
    ofn.hwndOwner       = owner;
    ofn.lpstrFilter     = filter;
    ofn.lpstrFile       = buf.data();
    ofn.nMaxFile        = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle      = title;
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.Flags           = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return {};
    buf.resize(wcslen(buf.c_str()));
    return std::filesystem::path(buf);
}

std::filesystem::path pick_folder(HWND owner, const wchar_t* title) {
    std::filesystem::path result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        return result;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(title);
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = std::filesystem::path(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

std::wstring lower_ext(const std::filesystem::path& p) {
    std::wstring e = p.extension().wstring();
    std::transform(e.begin(), e.end(), e.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return e;
}

// Finds the disc image (.cue preferred, then .bin / .iso) and a matching .sbi in a folder
void scan_game_folder(const std::filesystem::path& folder, std::filesystem::path& disc, std::filesystem::path& sbi) {
    std::vector<std::filesystem::path> cues, bins, isos, sbis;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const auto ext = lower_ext(entry.path());
        if (ext == L".cue") cues.push_back(entry.path());
        else if (ext == L".bin") bins.push_back(entry.path());
        else if (ext == L".iso") isos.push_back(entry.path());
        else if (ext == L".sbi") sbis.push_back(entry.path());
    }
    std::sort(cues.begin(), cues.end());
    std::sort(bins.begin(), bins.end());
    std::sort(isos.begin(), isos.end());
    disc = !cues.empty() ? cues.front() : !bins.empty() ? bins.front() : !isos.empty() ? isos.front() : std::filesystem::path{};
    sbi.clear();
    for (const auto& s : sbis) {
        if (!disc.empty() && s.stem() == disc.stem()) sbi = s;
    }
    if (sbi.empty() && !sbis.empty()) sbi = sbis.front();
}

// ---------------------------------------------------------------------------------------------
// Settings window
// ---------------------------------------------------------------------------------------------

enum ControlId : int {
    ID_DISC_EDIT = 100, ID_DISC_FOLDER, ID_DISC_FILE,
    ID_SBI_EDIT, ID_SBI_FILE, ID_SBI_CLEAR,
    ID_STATUS,
    ID_MODE_COMBO, ID_SCALE_COMBO, ID_FILTER_COMBO, ID_INTEGER_CHECK, ID_WIDE_CHECK,
    ID_RES_COMBO, ID_TEXFILTER_CHECK,
    ID_CARD_EDIT, ID_CARD_CHANGE, ID_CARD_OPEN,
    ID_MUTE_CHECK, ID_VOLUME_SLIDER, ID_VOLUME_LABEL,
    ID_SHOW_CHECK, ID_ASPECT_COMBO, ID_RIM_COMBO, ID_PGXP_COMBO, ID_RENDERER_COMBO, ID_GPU_COMBO, ID_RUMBLE_CHECK, ID_FREECAM_CHECK,ID_OVERCLOCK_COMBO, ID_AUTOSAVE_CHECK, ID_REWIND_CHECK, ID_CHEATS_CHECK, ID_CHEATS_OPEN, ID_REVERB_CHECK, ID_CUBIC_CHECK, ID_TEXPACK_CHECK, ID_TEXPACK_OPEN, ID_TEXDUMP_CHECK, ID_INTERP_CHECK, ID_RUNAHEAD_COMBO, ID_RA_CHECK, ID_RA_USER, ID_RA_PASS, ID_RA_LOGIN, ID_RA_LOGOUT, ID_RA_STATUS, ID_PAL60_CHECK,
    ID_PAD_COMBO0 = 300, // .. ID_PAD_COMBO0 + REMAP_COUNT - 1
    ID_PAD_DEFAULTS = 400,
    ID_NAV0 = 500,       // .. ID_NAV0 + PAGE_COUNT - 1: page buttons on the left
};

// Pages of the settings window (buttons on the left). A new group of settings = a new page.
enum Page : int { PAGE_GAME, PAGE_GRAPHICS, PAGE_DISPLAY, PAGE_SOUND, PAGE_CONTROLS, PAGE_ADVANCED, PAGE_ACHIEVEMENTS, PAGE_COUNT };
constexpr const wchar_t* PAGE_NAMES[PAGE_COUNT] = {L"Oyun", L"Grafik", L"Ekran", L"Ses", L"Kontroller", L"Gelişmiş", L"Başarımlar"};
constexpr int OVERCLOCK_VALUES[] = {100, 150, 200, 300};

// Layout (logical px): title on top, page buttons on the left, the page on the right, footer below
constexpr int WIN_W = 760, WIN_H = 470;
constexpr int M = 16, NAV_W = 150, BODY_Y = 48, BODY_H = 360;
constexpr int PAGE_X = M + NAV_W + 16, PAGE_W = WIN_W - PAGE_X - M;

struct LauncherState {
    GameSettings* s   = nullptr;
    bool play         = false;
    bool done         = false;
    float dpi         = 1.0f;
    HFONT font        = nullptr;
    HFONT title_font  = nullptr;
    HWND hwnd         = nullptr;
    bool disc_ok      = false;  // A readable disc image is selected (enables "Oyna")
    bool status_good  = false;  // ...and nothing else is missing (.sbi for the PAL release)
    std::vector<std::string> gpus; // Graphics cards in the "Ekran kartı" list (after "automatic")
    int building_page = -1;        // Controls made now belong to this page (-1 = always visible)
    std::vector<HWND> pages[PAGE_COUNT];

    int px(int v) const { return static_cast<int>(v * dpi + 0.5f); }
};

HWND make(LauncherState& st, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
          int id, DWORD ex_style = 0) {
    HWND c = CreateWindowExW(ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, st.px(x), st.px(y), st.px(w), st.px(h),
                             st.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
    if (st.building_page >= 0) st.pages[st.building_page].push_back(c);
    return c;
}

HWND item(const LauncherState& st, int id) { return GetDlgItem(st.hwnd, id); }

std::string window_text_utf8(HWND w) {
    std::wstring t(static_cast<std::size_t>(GetWindowTextLengthW(w)) + 1u, L'\0');
    t.resize(static_cast<std::size_t>(GetWindowTextW(w, t.data(), static_cast<int>(t.size()))));
    if (t.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, t.data(), static_cast<int>(t.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, t.data(), static_cast<int>(t.size()), s.data(), n, nullptr, nullptr);
    SecureZeroMemory(t.data(), t.size() * sizeof(wchar_t));
    return s;
}

void update_ra_status(LauncherState& st, const std::wstring& extra = {});

void update_ra_status(LauncherState& st, const std::wstring& extra) {
    const GameSettings& s = *st.s;
    const std::wstring t = !extra.empty() ? extra : (!s.ra_token.empty() ? L"Giriş yapıldı: " + widen(s.ra_user) : L"Giriş yapılmadı");
    SetWindowTextW(item(st, ID_RA_STATUS), t.c_str());
}

void update_status(LauncherState& st) {
    GameSettings& s = *st.s;
    SetWindowTextW(item(st, ID_DISC_EDIT), s.disc.wstring().c_str());
    SetWindowTextW(item(st, ID_SBI_EDIT), s.sbi.wstring().c_str());
    SetWindowTextW(item(st, ID_CARD_EDIT), (s.memcard.empty() ? default_memcard_path() : s.memcard).wstring().c_str());

    std::wstring status;
    st.disc_ok     = false;
    st.status_good = false;
    std::error_code ec;
    if (s.disc.empty()) {
        status = L"Oyun klasörünü ya da disk imajını (.cue / .bin / .iso) seçin.";
    } else if (!std::filesystem::exists(s.disc, ec)) {
        status = L"⚠ Disk imajı bulunamadı. Lütfen yeniden seçin.";
    } else {
        DiscImage probe;
        std::string err;
        if (!probe.open(s.disc, &err)) {
            status = L"⚠ Disk açılamadı: " + widen(err);
        } else {
            st.disc_ok = true;
            bool sbi_loaded = probe.subq_override_count() > 0;
            if (!s.sbi.empty()) {
                sbi_loaded = probe.load_sbi(s.sbi, &err);
            }
            const std::string& vol = probe.volume_id();
            if (vol == "SLES-02754") {
                status = L"✓ Vagrant Story — Avrupa sürümü (PAL, SLES-02754)\n";
                status += sbi_loaded ? L"✓ LibCrypt alt kanal verisi (.sbi) hazır"
                                     : L"⚠ Bu sürüm .sbi dosyası ister; yoksa New Game'den sonra takılır";
                st.status_good = sbi_loaded;
            } else if (vol == "SLUS-01040") {
                status = L"✓ Vagrant Story — ABD sürümü (NTSC, SLUS-01040)\n.sbi gerekmez";
                st.status_good = true;
            } else if (vol == "SLPS-02377") {
                status = L"✓ Vagrant Story — Japonya sürümü (NTSC-J, SLPS-02377)";
                st.status_good = true;
            } else {
                status = L"⚠ Tanınmayan disk: " + widen(vol) + L"\nBu port Vagrant Story için yapıldı.";
            }
        }
    }
    SetWindowTextW(item(st, ID_STATUS), status.c_str());
    InvalidateRect(item(st, ID_STATUS), nullptr, TRUE);
    EnableWindow(item(st, IDOK), st.disc_ok ? TRUE : FALSE);
}

void update_volume_label(LauncherState& st) {
    wchar_t buf[16];
    swprintf_s(buf, L"%d%%", st.s->volume);
    SetWindowTextW(item(st, ID_VOLUME_LABEL), buf);
    EnableWindow(item(st, ID_VOLUME_SLIDER), st.s->muted ? FALSE : TRUE);
}

// Window size list: widths follow the selected aspect ratio, so the labels are rebuilt on change
void fill_window_sizes(LauncherState& st, int selected_height) {
    HWND c = item(st, ID_SCALE_COMBO);
    const int aspect = clamp_int(static_cast<int>(SendMessageW(item(st, ID_ASPECT_COMBO), CB_GETCURSEL, 0, 0)), 0,
                                 ASPECT_COUNT - 1);
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    int best = 0;
    for (int i = 0; i < WINDOW_HEIGHT_COUNT; ++i) {
        const int h = WINDOW_HEIGHTS[i];
        const wchar_t* tag = h == 720 ? L"  HD" : h == 1080 ? L"  Full HD" : h == 1440 ? L"  2K / QHD"
                           : h == 2160 ? L"  4K" : L"";
        wchar_t buf[64];
        swprintf_s(buf, L"%d × %d%s", window_width_for(h, aspect), h, tag);
        SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
        if (std::abs(h - selected_height) < std::abs(WINDOW_HEIGHTS[best] - selected_height)) best = i;
    }
    SendMessageW(c, CB_SETCURSEL, best, 0);
}

int selected_window_height(const LauncherState& st) {
    const int i = static_cast<int>(SendMessageW(item(st, ID_SCALE_COMBO), CB_GETCURSEL, 0, 0));
    return WINDOW_HEIGHTS[clamp_int(i, 0, WINDOW_HEIGHT_COUNT - 1)];
}

// PS1 buttons, in pad::REMAP_BITS order
constexpr const wchar_t* PS1_BUTTON_NAMES[pad::REMAP_COUNT] = {
    L"×  Çarpı", L"○  Daire", L"□  Kare", L"△  Üçgen", L"L1", L"R1",
    L"L2", L"R2", L"START", L"SELECT", L"L3", L"R3",
};
// Physical controller buttons by position (Xbox name / PlayStation name), same order
constexpr const wchar_t* PHYSICAL_BUTTON_NAMES[pad::REMAP_COUNT] = {
    L"Alt yüz tuşu  (A / ×)", L"Sağ yüz tuşu  (B / ○)", L"Sol yüz tuşu  (X / □)", L"Üst yüz tuşu  (Y / △)",
    L"Sol omuz  (LB / L1)", L"Sağ omuz  (RB / R1)", L"Sol tetik  (LT / L2)", L"Sağ tetik  (RT / R2)",
    L"Start / Options", L"Back / Share", L"Sol çubuğa bas  (L3)", L"Sağ çubuğa bas  (R3)",
};

void pad_select(LauncherState& st, const pad::ButtonMap& m) {
    for (std::size_t i = 0; i < pad::REMAP_COUNT; ++i) {
        const int src = m[i];
        // Combo entry 0 = "(atanmamış)", 1.. = physical buttons
        SendMessageW(item(st, ID_PAD_COMBO0 + static_cast<int>(i)), CB_SETCURSEL,
                     src >= 0 && src < static_cast<int>(pad::REMAP_COUNT) ? src + 1 : 0, 0);
    }
}

void show_page(LauncherState& st, int page) {
    for (int p = 0; p < PAGE_COUNT; ++p) {
        for (HWND c : st.pages[p]) ShowWindow(c, p == page ? SW_SHOWNA : SW_HIDE);
        SendMessageW(item(st, ID_NAV0 + p), BM_SETCHECK, p == page ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

HWND add_combo(LauncherState& st, int x, int y, int w, int id, std::initializer_list<const wchar_t*> entries, int sel) {
    HWND c = make(st, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, x, y, w, 240, id);
    for (const wchar_t* e : entries) SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(e));
    SendMessageW(c, CB_SETCURSEL, sel, 0);
    return c;
}

HWND add_check(LauncherState& st, const wchar_t* text, int x, int y, int w, int h, int id, bool on, DWORD extra = 0) {
    HWND c = make(st, L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP | extra, x, y, w, h, id);
    SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    return c;
}

void build_controls(LauncherState& st) {
    GameSettings& s = *st.s;
    const DWORD tab = WS_TABSTOP;

    HWND title = make(st, L"STATIC", L"Vagrant Story — Ayarlar", SS_LEFT, M, 12, WIN_W - 2 * M, 28, 0);
    SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(st.title_font), TRUE);

    // --- Page buttons (left) ---
    for (int p = 0; p < PAGE_COUNT; ++p) {
        make(st, L"BUTTON", PAGE_NAMES[p], BS_AUTORADIOBUTTON | BS_PUSHLIKE | tab | (p == 0 ? WS_GROUP : 0),
             M, BODY_Y + 8 + p * 40, NAV_W, 34, ID_NAV0 + p);
    }

    // Page frame and its first content row
    constexpr int LX = PAGE_X + 14, IW = PAGE_W - 28;          // Content left / width
    constexpr int CX = PAGE_X + 170, CW = 260;                  // Combo column
    const auto begin_page = [&](int p) {
        st.building_page = p;
        make(st, L"BUTTON", PAGE_NAMES[p], BS_GROUPBOX | WS_GROUP, PAGE_X, BODY_Y, PAGE_W, BODY_H, 0);
        return BODY_Y + 28;
    };
    const auto label = [&](const wchar_t* t, int y) { make(st, L"STATIC", t, SS_LEFT, LX, y + 3, 150, 20, 0); };

    // --- Oyun: disc image, .sbi, memory card ---
    int y = begin_page(PAGE_GAME);
    constexpr int BW = 100, EW = IW - 2 * BW - 12, B1 = LX + EW + 6, B2 = B1 + BW + 6;
    make(st, L"STATIC", L"Oyun klasörü / disk imajı:", SS_LEFT, LX, y, IW, 18, 0);
    make(st, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_BORDER, LX, y + 20, EW, 24, ID_DISC_EDIT);
    make(st, L"BUTTON", L"Klasör seç…", BS_PUSHBUTTON | tab, B1, y + 20, BW, 24, ID_DISC_FOLDER);
    make(st, L"BUTTON", L"Dosya seç…", BS_PUSHBUTTON | tab, B2, y + 20, BW, 24, ID_DISC_FILE);
    y += 56;
    make(st, L"STATIC", L"Alt kanal dosyası (.sbi, yalnızca Avrupa sürümü):", SS_LEFT, LX, y, IW, 18, 0);
    make(st, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_BORDER, LX, y + 20, EW, 24, ID_SBI_EDIT);
    make(st, L"BUTTON", L"Dosya seç…", BS_PUSHBUTTON | tab, B1, y + 20, BW, 24, ID_SBI_FILE);
    make(st, L"BUTTON", L"Kaldır", BS_PUSHBUTTON | tab, B2, y + 20, BW, 24, ID_SBI_CLEAR);
    y += 56;
    make(st, L"STATIC", L"Hafıza kartı 1 (kayıtlar bu dosyada tutulur):", SS_LEFT, LX, y, IW, 18, 0);
    make(st, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_BORDER, LX, y + 20, EW, 24, ID_CARD_EDIT);
    make(st, L"BUTTON", L"Değiştir…", BS_PUSHBUTTON | tab, B1, y + 20, BW, 24, ID_CARD_CHANGE);
    make(st, L"BUTTON", L"Klasörü aç", BS_PUSHBUTTON | tab, B2, y + 20, BW, 24, ID_CARD_OPEN);
    y += 64;
    make(st, L"STATIC", L"", SS_LEFT, LX, y, IW, 40, ID_STATUS);

    // --- Grafik: how the 3D picture is drawn ---
    y = begin_page(PAGE_GRAPHICS);
    label(L"İç çözünürlük:", y);
    add_combo(st, CX, y, CW, ID_RES_COMBO,
              {L"1×  orijinal (320 × 240)", L"2×  (640 × 480)", L"3×  (960 × 720)", L"4×  (1280 × 960)",
               L"5×  (1600 × 1200) — 1080p ekranlar", L"6×  (1920 × 1440) — 1440p ekranlar",
               L"7×  (2240 × 1680) — kenar yumuşatma", L"8×  (2560 × 1920) — kenar yumuşatma, güçlü ekran kartı"},
              clamp_int(s.internal_scale, 1, 8) - 1);
    y += 34;
    label(L"Çizim:", y);
    add_combo(st, CX, y, CW, ID_RENDERER_COMBO, {L"Ekran kartı (önerilen)", L"İşlemci"}, clamp_int(s.renderer, 0, 1));
    y += 34;
    label(L"Ekran kartı:", y);
    HWND gpu = add_combo(st, CX, y, IW - (CX - LX), ID_GPU_COMBO, {L"Otomatik (en güçlü)"}, 0);
    st.gpus = list_graphics_adapters();
    int gpu_sel = 0;
    for (std::size_t i = 0; i < st.gpus.size(); ++i) {
        SendMessageW(gpu, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(st.gpus[i]).c_str()));
        if (st.gpus[i] == s.gpu) gpu_sel = static_cast<int>(i) + 1;
    }
    SendMessageW(gpu, CB_SETCURSEL, gpu_sel, 0); // A card that is no longer there falls back to automatic
    y += 34;
    label(L"Karakter kenar ışığı:", y);
    add_combo(st, CX, y, CW, ID_RIM_COMBO, {L"Yumuşak", L"Keskin", L"Kapalı"}, clamp_int(s.rim_light, 0, 2));
    y += 34;
    label(L"Köşe hassasiyeti:", y);
    add_combo(st, CX, y, CW, ID_PGXP_COMBO, {L"Kapalı (orijinal titreme)", L"Titreme düzeltmesi", L"Titreme + perspektifli doku"},
              clamp_int(s.pgxp, 0, 2));
    y += 34;
    label(L"3D doku filtresi:", y);
    add_combo(st, CX, y, CW, ID_TEXFILTER_CHECK, {L"Kapalı (keskin pikseller)", L"Yumuşak (bilinear)", L"Keskin yumuşatma"},
              clamp_int(s.texture_filter, 0, 2));
    y += 34;
    make(st, L"STATIC", L"Kenar yumuşatma: iç çözünürlük pencereden büyükse görüntü ortalanarak küçültülür "
                        L"(süper örnekleme). 1080p ekranda 7× ya da 8× seçin.",
         SS_LEFT, LX, y, IW, 36, 0);
    y += 44;
    add_check(st, L"Ara kare üretimi: 25 yerine 50 fps görüntü (ekran kartı çizimi, 2× ve üstü; deneysel)", LX, y, IW, 20,
              ID_INTERP_CHECK, s.frame_interp);

    // --- Ekran: window, aspect ratio, filter ---
    y = begin_page(PAGE_DISPLAY);
    label(L"Ekran modu:", y);
    add_combo(st, CX, y, CW, ID_MODE_COMBO, {L"Pencere", L"Tam ekran"}, s.fullscreen ? 1 : 0);
    y += 34;
    label(L"Görüntü oranı:", y);
    add_combo(st, CX, y, CW, ID_ASPECT_COMBO, {L"4:3  (orijinal)", L"10:7", L"3:2", L"16:10", L"16:9", L"Pencereyi / ekranı doldur"},
              clamp_int(s.aspect, 0, ASPECT_COUNT - 1));
    y += 34;
    label(L"Pencere boyutu:", y);
    make(st, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | tab | WS_VSCROLL, CX, y, CW, 240, ID_SCALE_COMBO);
    fill_window_sizes(st, s.window_height);
    y += 34;
    label(L"Görüntü filtresi:", y);
    add_combo(st, CX, y, CW, ID_FILTER_COMBO, {L"Keskin piksel", L"Yumuşak (bilinear)", L"CRT tarama çizgileri"},
              clamp_int(s.filter, 0, 2));
    y += 40;
    add_check(st, L"Tam sayı ölçekleme (yalnızca 1× iç çözünürlükte; kenarlarda boşluk bırakır)", LX, y, IW, 20,
              ID_INTEGER_CHECK, s.integer_scale);
    y += 28;
    add_check(st, L"3D sahneyi orana göre genişlet (kapalıysa görüntü esnetilir;\n"
                  L"2D yazılar ve göstergeler yatayda esner)",
              LX, y, IW, 36, ID_WIDE_CHECK, s.widescreen, BS_MULTILINE);
    y += 46;
    add_check(st, L"Avrupa (PAL) diskini 60 Hz'de çalıştır: oyun ABD sürümü hızında akar\n"
                  L"(PAL sürümü normalde ~%17 yavaştır; müzik temposu değişmez)",
              LX, y, IW, 36, ID_PAL60_CHECK, s.pal60, BS_MULTILINE);

    // --- Ses ---
    y = begin_page(PAGE_SOUND);
    add_check(st, L"Sesi kapat", LX, y + 2, 140, 20, ID_MUTE_CHECK, s.muted);
    y += 36;
    label(L"Ses seviyesi:", y);
    HWND vol = make(st, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | tab, CX - 6, y, CW + 12, 26, ID_VOLUME_SLIDER);
    SendMessageW(vol, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(vol, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(vol, TBM_SETPOS, TRUE, clamp_int(s.volume, 0, 100));
    make(st, L"STATIC", L"", SS_LEFT, CX + CW + 14, y + 3, 50, 20, ID_VOLUME_LABEL);
    y += 44;
    add_check(st, L"Yankı (PS1 reverb birimi: mekân ve zindan yankısı)", LX, y, IW, 20, ID_REVERB_CHECK, s.reverb);
    y += 28;
    add_check(st, L"Kübik ses ara değerlemesi (daha temiz tizler; kapalıyken doğrusal)", LX, y, IW, 20, ID_CUBIC_CHECK, s.audio_cubic);

    // --- Kontroller: which controller button acts as each PS1 button ---
    y = begin_page(PAGE_CONTROLS);
    make(st, L"STATIC", L"Her PS1 tuşu için kontrolcünüzde hangi tuşun kullanılacağını seçin. Yön tuşları ve sol "
                        L"çubuk her zaman yön olarak çalışır; klavye tuşları değişmez.",
         SS_LEFT, LX, y, IW, 36, 0);
    y += 46;
    constexpr int COL_W = 268, PAD_LW = 82, PAD_CW = COL_W - PAD_LW - 10;
    constexpr int PAD_ROWS = static_cast<int>(pad::REMAP_COUNT + 1) / 2;
    for (std::size_t i = 0; i < pad::REMAP_COUNT; ++i) {
        const int col = static_cast<int>(i) / PAD_ROWS, row = static_cast<int>(i) % PAD_ROWS;
        const int x = LX + col * COL_W, yy = y + row * 32;
        make(st, L"STATIC", PS1_BUTTON_NAMES[i], SS_LEFT, x, yy + 3, PAD_LW, 20, 0);
        HWND c = make(st, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | tab | WS_VSCROLL, x + PAD_LW, yy, PAD_CW, 300,
                      ID_PAD_COMBO0 + static_cast<int>(i));
        SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(atanmamış)"));
        for (const wchar_t* name : PHYSICAL_BUTTON_NAMES) SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
    }
    y += PAD_ROWS * 32 + 8;
    make(st, L"BUTTON", L"Varsayılana döndür", BS_PUSHBUTTON | tab, LX, y, 150, 28, ID_PAD_DEFAULTS);
    add_check(st, L"Titreşim (Xbox / XInput uyumlu kontrolcüler)", LX + 180, y + 4, IW - 180, 20, ID_RUMBLE_CHECK, s.rumble);
    y += 36;
    add_check(st, L"Sağ çubukla serbest kamera (klavyede J / L döndür, I / K eğ)", LX, y, IW, 20, ID_FREECAM_CHECK, s.free_camera);
    pad_select(st, s.pad_map);

    // --- Gelişmiş: emulation tweaks ---
    y = begin_page(PAGE_ADVANCED);
    label(L"İşlemci hızı:", y);
    {
        int sel = 0;
        for (int i = 0; i < 4; ++i) if (s.cpu_overclock >= OVERCLOCK_VALUES[i]) sel = i;
        add_combo(st, CX, y, CW, ID_OVERCLOCK_COMBO, {L"Orijinal (%100)", L"%150", L"%200", L"%300"}, sel);
    }
    y += 30;
    make(st, L"STATIC", L"Emüle edilen PS1 işlemcisini hızlandırır: oyunun kalabalık sahnelerdeki kendi "
                        L"yavaşlamaları azalır. Oyun hızlanmaz; bilgisayarın işlemcisi daha çok çalışır.",
         SS_LEFT, LX, y, IW, 36, 0);

    y += 46;
    make(st, L"STATIC", L"Anlık kayıt (oyunun kendi kayıt noktalarından bağımsız):  F5 kaydet,  F7 yükle,  "
                        L"F6 kayıt yuvasını değiştir (1–5).",
         SS_LEFT, LX, y, IW, 36, 0);
    y += 42;
    add_check(st, L"Çıkarken durumu kaydet, açılışta kaldığım yerden devam et", LX, y, IW, 20, ID_AUTOSAVE_CHECK, s.autosave);
    y += 28;
    add_check(st, L"Geri sarma: F8 basılı tutulunca son ~30 saniye geri sarılır (~110 MB bellek)", LX, y, IW, 20,
              ID_REWIND_CHECK, s.rewind);

    y += 32;
    add_check(st, L"Hile kodlarını uygula (GameShark biçimi)", LX, y + 4, 300, 20, ID_CHEATS_CHECK, s.cheats);
    make(st, L"BUTTON", L"Hile dosyasını aç…", BS_PUSHBUTTON | tab, LX + IW - 160, y, 160, 28, ID_CHEATS_OPEN);

    y += 34;
    add_check(st, L"Doku paketi kullan (\"dokular\" klasörü; 2× ve üstü iç çözünürlükte)", LX, y + 4, IW - 170, 20,
              ID_TEXPACK_CHECK, s.texture_pack);
    make(st, L"BUTTON", L"Doku klasörünü aç…", BS_PUSHBUTTON | tab, LX + IW - 160, y, 160, 28, ID_TEXPACK_OPEN);
    y += 32;
    add_check(st, L"Oyunun dokularını dök (paket hazırlamak için, dokular\\_dokum klasörüne)", LX, y, IW, 20,
              ID_TEXDUMP_CHECK, s.texture_dump);
    y += 32;
    label(L"Gecikme azaltma:", y);
    add_combo(st, CX, y, CW, ID_RUNAHEAD_COMBO,
              {L"Kapalı", L"1 kare (20 ms, önerilen)", L"2 kare (40 ms, güçlü işlemci)", L"3 kare (60 ms, çok güçlü işlemci)"},
              clamp_int(s.runahead, 0, 3));
    y += 28;
    make(st, L"STATIC", L"Tuşa basışın sonucu bu kadar erken görünür (emülasyon 2–4 kat çalışır; ara kare üretimi kapanır).",
         SS_LEFT, LX, y, IW, 20, 0);

    // --- Başarımlar: RetroAchievements ---
    y = begin_page(PAGE_ACHIEVEMENTS);
    add_check(st, L"RetroAchievements başarımlarını etkinleştir (softcore)", LX, y, IW, 20, ID_RA_CHECK, s.ra_enabled);
    y += 36;
    label(L"Kullanıcı adı:", y);
    make(st, L"EDIT", widen(s.ra_user).c_str(), ES_AUTOHSCROLL | WS_BORDER | tab, CX, y, CW, 24, ID_RA_USER);
    y += 32;
    label(L"Şifre:", y);
    make(st, L"EDIT", L"", ES_AUTOHSCROLL | ES_PASSWORD | WS_BORDER | tab, CX, y, CW, 24, ID_RA_PASS);
    make(st, L"BUTTON", L"Giriş yap", BS_PUSHBUTTON | tab, CX + CW + 12, y - 2, 120, 28, ID_RA_LOGIN);
    y += 40;
    make(st, L"STATIC", L"", SS_LEFT, LX, y + 4, CX + CW - LX, 20, ID_RA_STATUS);
    make(st, L"BUTTON", L"Çıkış yap", BS_PUSHBUTTON | tab, CX + CW + 12, y, 120, 28, ID_RA_LOGOUT);
    y += 44;
    make(st, L"STATIC", L"Başarım seti yalnızca ABD sürümünde (SLUS-01040) var; Avrupa diskinde başarım yüklenmez.\n\n"
                        L"Yalnızca softcore: anlık kayıt, geri sarma ve hileler kullanılabilir (hardcore için "
                        L"RetroAchievements onayı gerekir).\n\n"
                        L"Şifre saklanmaz: giriş yapınca sunucunun verdiği oturum anahtarı VagrantStory.ini'ye yazılır. "
                        L"Açılan başarımlar pencere başlığında gösterilir.",
         SS_LEFT, LX, y, IW, 130, 0);
    update_ra_status(st);

    // --- Footer (always visible) ---
    st.building_page = -1;
    const int fy = BODY_Y + BODY_H + 14;
    HWND show = add_check(st, L"Bu pencereyi her açılışta göster  (kapalıyken oyunu Shift ile başlatın)", M, fy + 5,
                          WIN_W - 2 * M - 240, 20, ID_SHOW_CHECK, s.show_launcher);
    SendMessageW(show, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
    make(st, L"BUTTON", L"Çıkış", BS_PUSHBUTTON | tab | WS_GROUP, WIN_W - M - 120 - 8 - 100, fy, 100, 30, IDCANCEL);
    make(st, L"BUTTON", L"Oyna", BS_DEFPUSHBUTTON | tab, WIN_W - M - 120, fy, 120, 30, IDOK);

    update_status(st);
    update_volume_label(st);
    // Start on the game page when the disc still has to be chosen, otherwise on the graphics page
    show_page(st, st.disc_ok ? PAGE_GRAPHICS : PAGE_GAME);
}

void read_controls(LauncherState& st) {
    GameSettings& s = *st.s;
    s.fullscreen    = SendMessageW(item(st, ID_MODE_COMBO), CB_GETCURSEL, 0, 0) == 1;
    s.window_height = selected_window_height(st);
    s.aspect        = clamp_int(static_cast<int>(SendMessageW(item(st, ID_ASPECT_COMBO), CB_GETCURSEL, 0, 0)), 0, ASPECT_COUNT - 1);
    s.internal_scale = clamp_int(static_cast<int>(SendMessageW(item(st, ID_RES_COMBO), CB_GETCURSEL, 0, 0)) + 1, 1, 8);
    s.renderer      = clamp_int(static_cast<int>(SendMessageW(item(st, ID_RENDERER_COMBO), CB_GETCURSEL, 0, 0)), 0, 1);
    const int gpu_sel = static_cast<int>(SendMessageW(item(st, ID_GPU_COMBO), CB_GETCURSEL, 0, 0));
    s.gpu = (gpu_sel >= 1 && gpu_sel <= static_cast<int>(st.gpus.size())) ? st.gpus[static_cast<std::size_t>(gpu_sel - 1)] : std::string();
    s.texture_filter = clamp_int(static_cast<int>(SendMessageW(item(st, ID_TEXFILTER_CHECK), CB_GETCURSEL, 0, 0)), 0, 2);
    s.rim_light     = clamp_int(static_cast<int>(SendMessageW(item(st, ID_RIM_COMBO), CB_GETCURSEL, 0, 0)), 0, 2);
    s.pgxp          = clamp_int(static_cast<int>(SendMessageW(item(st, ID_PGXP_COMBO), CB_GETCURSEL, 0, 0)), 0, 2);
    s.filter        = clamp_int(static_cast<int>(SendMessageW(item(st, ID_FILTER_COMBO), CB_GETCURSEL, 0, 0)), 0, 2);
    s.integer_scale = SendMessageW(item(st, ID_INTEGER_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.widescreen    = SendMessageW(item(st, ID_WIDE_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.muted         = SendMessageW(item(st, ID_MUTE_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.volume        = clamp_int(static_cast<int>(SendMessageW(item(st, ID_VOLUME_SLIDER), TBM_GETPOS, 0, 0)), 0, 100);
    s.show_launcher = SendMessageW(item(st, ID_SHOW_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.rumble        = SendMessageW(item(st, ID_RUMBLE_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.free_camera   = SendMessageW(item(st, ID_FREECAM_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.autosave      = SendMessageW(item(st, ID_AUTOSAVE_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.cheats        = SendMessageW(item(st, ID_CHEATS_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.texture_pack  = SendMessageW(item(st, ID_TEXPACK_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.pal60         = SendMessageW(item(st, ID_PAL60_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.frame_interp  = SendMessageW(item(st, ID_INTERP_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.texture_dump  = SendMessageW(item(st, ID_TEXDUMP_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.reverb        = SendMessageW(item(st, ID_REVERB_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.audio_cubic   = SendMessageW(item(st, ID_CUBIC_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.rewind        = SendMessageW(item(st, ID_REWIND_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.cpu_overclock = OVERCLOCK_VALUES[clamp_int(static_cast<int>(SendMessageW(item(st, ID_OVERCLOCK_COMBO), CB_GETCURSEL, 0, 0)), 0, 3)];
    {
        const bool ra_on = SendMessageW(item(st, ID_RA_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
        const std::string user = window_text_utf8(item(st, ID_RA_USER));
        if (user != s.ra_user) s.ra_token.clear(); // The token belongs to the name it was made for
        s.ra_user = user;
        s.ra_enabled = ra_on;
    }
    s.runahead      = clamp_int(static_cast<int>(SendMessageW(item(st, ID_RUNAHEAD_COMBO), CB_GETCURSEL, 0, 0)), 0, 3);
    for (std::size_t i = 0; i < pad::REMAP_COUNT; ++i) {
        const auto sel = SendMessageW(item(st, ID_PAD_COMBO0 + static_cast<int>(i)), CB_GETCURSEL, 0, 0);
        s.pad_map[i] = static_cast<s8>(sel <= 0 ? -1 : sel - 1);
    }
}

LRESULT CALLBACK LauncherWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* st = reinterpret_cast<LauncherState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            st = static_cast<LauncherState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
            st->hwnd = hwnd;
            build_controls(*st);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            // Status line: green when everything needed to play is there, amber otherwise
            if (st && reinterpret_cast<HWND>(lparam) == item(*st, ID_STATUS)) {
                HDC dc = reinterpret_cast<HDC>(wparam);
                SetTextColor(dc, st->status_good ? RGB(0, 120, 60) : RGB(170, 90, 0));
                SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
            }
            break;
        }
        case WM_HSCROLL:
            if (st && reinterpret_cast<HWND>(lparam) == item(*st, ID_VOLUME_SLIDER)) {
                st->s->volume = static_cast<int>(SendMessageW(item(*st, ID_VOLUME_SLIDER), TBM_GETPOS, 0, 0));
                update_volume_label(*st);
            }
            return 0;
        case WM_COMMAND: {
            if (!st) break;
            GameSettings& s = *st->s;
            const int id = LOWORD(wparam);
            if (id >= ID_NAV0 && id < static_cast<int>(ID_NAV0) + static_cast<int>(PAGE_COUNT)) {
                if (HIWORD(wparam) == BN_CLICKED) show_page(*st, id - ID_NAV0);
                return 0;
            }
            switch (id) {
                case ID_DISC_FOLDER: {
                    const auto folder = pick_folder(hwnd, L"Vagrant Story disk imajının bulunduğu klasörü seçin");
                    if (!folder.empty()) {
                        std::filesystem::path disc, sbi;
                        scan_game_folder(folder, disc, sbi);
                        if (disc.empty()) {
                            MessageBoxW(hwnd, L"Bu klasörde .cue, .bin ya da .iso dosyası bulunamadı.",
                                        L"Vagrant Story", MB_OK | MB_ICONWARNING);
                        } else {
                            s.disc = disc;
                            s.sbi  = sbi;
                            update_status(*st);
                        }
                    }
                    return 0;
                }
                case ID_DISC_FILE: {
                    const auto f = pick_file(hwnd, L"Vagrant Story disk imajını seçin",
                                             L"PS1 disk imajı (*.cue;*.bin;*.iso)\0*.cue;*.bin;*.iso\0Tüm dosyalar (*.*)\0*.*\0",
                                             s.disc);
                    if (!f.empty()) {
                        s.disc = f;
                        // Pick up a same-named .sbi next to the new image
                        auto sbi = f;
                        sbi.replace_extension(".sbi");
                        std::error_code ec;
                        if (std::filesystem::exists(sbi, ec)) s.sbi = sbi;
                        update_status(*st);
                    }
                    return 0;
                }
                case ID_SBI_FILE: {
                    const auto f = pick_file(hwnd, L"Diskin .sbi alt kanal dosyasını seçin",
                                             L"Alt kanal verisi (*.sbi)\0*.sbi\0Tüm dosyalar (*.*)\0*.*\0",
                                             s.sbi.empty() ? s.disc : s.sbi);
                    if (!f.empty()) {
                        s.sbi = f;
                        update_status(*st);
                    }
                    return 0;
                }
                case ID_SBI_CLEAR:
                    s.sbi.clear();
                    update_status(*st);
                    return 0;
                case ID_CARD_CHANGE: {
                    // Pick an existing card image or name a new one (created empty on first start)
                    const auto current = s.memcard.empty() ? default_memcard_path() : s.memcard;
                    std::wstring buf(32768, L'\0');
                    const std::wstring init = current.filename().wstring();
                    std::copy(init.begin(), init.end(), buf.begin());
                    const std::wstring dir = current.parent_path().wstring();
                    OPENFILENAMEW ofn{};
                    ofn.lStructSize     = sizeof(ofn);
                    ofn.hwndOwner       = hwnd;
                    ofn.lpstrFilter     = L"Hafıza kartı (*.mcr;*.mcd;*.mc;*.srm)\0*.mcr;*.mcd;*.mc;*.srm\0Tüm dosyalar (*.*)\0*.*\0";
                    ofn.lpstrFile       = buf.data();
                    ofn.nMaxFile        = static_cast<DWORD>(buf.size());
                    ofn.lpstrInitialDir = dir.c_str();
                    ofn.lpstrDefExt     = L"mcr";
                    ofn.lpstrTitle      = L"Hafıza kartı dosyasını seçin ya da yeni bir ad verin";
                    ofn.Flags           = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
                    if (GetSaveFileNameW(&ofn)) {
                        buf.resize(wcslen(buf.c_str()));
                        s.memcard = std::filesystem::path(buf);
                        update_status(*st);
                    }
                    return 0;
                }
                case ID_CARD_OPEN: {
                    const auto folder = (s.memcard.empty() ? default_memcard_path() : s.memcard).parent_path();
                    std::error_code ec;
                    std::filesystem::create_directories(folder, ec);
                    ShellExecuteW(hwnd, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                }
                case ID_ASPECT_COMBO:
                    if (HIWORD(wparam) == CBN_SELCHANGE) fill_window_sizes(*st, selected_window_height(*st));
                    return 0;
                case ID_RA_LOGIN: {
                    const std::string user = window_text_utf8(item(*st, ID_RA_USER));
                    std::string pass = window_text_utf8(item(*st, ID_RA_PASS));
                    if (user.empty() || pass.empty()) {
                        update_ra_status(*st, L"Kullanıcı adı ve şifre girin");
                        return 0;
                    }
                    update_ra_status(*st, L"Giriş yapılıyor…");
                    UpdateWindow(item(*st, ID_RA_STATUS));
                    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
                    std::string token, display, error;
                    const bool ok = ra::login_with_password(user, pass, token, display, error);
                    SetCursor(old);
                    SecureZeroMemory(pass.data(), pass.size());
                    SetWindowTextW(item(*st, ID_RA_PASS), L""); // The password is not kept anywhere
                    if (ok) {
                        s.ra_user = user;
                        s.ra_token = token;
                        s.ra_enabled = true;
                        SendMessageW(item(*st, ID_RA_CHECK), BM_SETCHECK, BST_CHECKED, 0);
                        update_ra_status(*st, L"✓ Giriş yapıldı: " + widen(display));
                    } else {
                        update_ra_status(*st, L"⚠ Giriş başarısız: " + widen(error));
                    }
                    return 0;
                }
                case ID_RA_LOGOUT:
                    s.ra_token.clear();
                    update_ra_status(*st);
                    return 0;
                case ID_TEXPACK_OPEN: {
                    std::error_code ec;
                    std::filesystem::create_directories(texture_pack_dir(), ec);
                    ShellExecuteW(hwnd, L"open", texture_pack_dir().wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                }
                case ID_CHEATS_OPEN: {
                    // Create the file with an explanation on first use, then open it in the text editor
                    const auto path = cheats_path();
                    std::error_code ec;
                    if (!std::filesystem::exists(path, ec)) {
                        std::ofstream out(path, std::ios::binary);
                        out << "\xEF\xBB\xBF"
                               "# Vagrant Story hile kodlari (GameShark / Action Replay bicimi)\r\n"
                               "#\r\n"
                               "# Kodlar oyunun SURUMUNE ozeldir: diskinizin surumu icin yazilmis kodlari kullanin\r\n"
                               "# (Avrupa: SLES-02754, ABD: SLUS-01040, Japonya: SLPS-02377).\r\n"
                               "#\r\n"
                               "# [Hile adi]            -> bir hile baslatir (acik)\r\n"
                               "# [Hile adi] off        -> kapali bir hile\r\n"
                               "# 80012345 03E7         -> kod satirlari (bu satir yalnizca ornektir)\r\n"
                               "#\r\n"
                               "# Desteklenen kod turleri: 80 (16 bit yaz), 30 (8 bit yaz),\r\n"
                               "# D0-D3 ve E0-E3 (kosul: sonraki satir yalnizca kosul saglaninca calisir).\r\n"
                               "# Degisiklikler oyunun bir sonraki acilisinda gecerli olur.\r\n"
                               "\r\n";
                    }
                    ShellExecuteW(hwnd, L"open", L"notepad.exe", (L"\"" + path.wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
                    return 0;
                }
                case ID_PAD_DEFAULTS:
                    pad_select(*st, pad::default_button_map());
                    return 0;
                case ID_MUTE_CHECK:
                    s.muted = SendMessageW(item(*st, ID_MUTE_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
                    update_volume_label(*st);
                    return 0;
                case IDOK:
                    if (!st->disc_ok) return 0;
                    read_controls(*st);
                    st->play = true;
                    DestroyWindow(hwnd);
                    return 0;
                case IDCANCEL:
                    DestroyWindow(hwnd);
                    return 0;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (st) st->done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

void show_message(const std::string& text, bool error) {
    MessageBoxW(nullptr, widen(text).c_str(), L"Vagrant Story",
                MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

std::filesystem::path settings_path() {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return std::filesystem::path(buf).parent_path() / "VagrantStory.ini";
}

std::filesystem::path default_memcard_path() {
    return settings_path().parent_path() / "saves" / "VagrantStory_kart1.mcr";
}

std::filesystem::path cheats_path() {
    return settings_path().parent_path() / "VagrantStory_hileler.txt";
}

std::filesystem::path texture_pack_dir() {
    return settings_path().parent_path() / "dokular";
}

namespace {

// Saves a window's picture (PrintWindow) as a 24-bit BMP
void save_window_bmp(HWND hwnd, const std::filesystem::path& path) {
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h; // Bottom-up, as BMP files store it
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(hwnd, mem, 0); // Classic WM_PRINT drawing (works off-screen)
    GdiFlush();
    const DWORD stride = static_cast<DWORD>((w * 3 + 3) & ~3), size = stride * static_cast<DWORD>(h);
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
    fh.bfSize = fh.bfOffBits + size;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
    out.write(reinterpret_cast<const char*>(&bi.bmiHeader), sizeof(bi.bmiHeader));
    out.write(static_cast<const char*>(bits), size);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

// Shows the settings window. dump_dir: diagnostics, the window opens off-screen, each page is saved
// as page_N.bmp and the window closes (nothing is saved to the ini).
bool run_window(GameSettings& s, const std::filesystem::path* dump_dir) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    LauncherState st;
    st.s   = &s;
    st.dpi = static_cast<float>(GetDpiForSystem()) / 96.0f;

    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    st.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW tf = ncm.lfMessageFont;
    tf.lfHeight = -st.px(20);
    tf.lfWeight = FW_SEMIBOLD;
    st.title_font = CreateFontIndirectW(&tf);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc   = LauncherWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VagrantStoryLauncherWnd";
    wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.hIcon         = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));   // IDI_APPLICATION
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const DWORD ex = WS_EX_CONTROLPARENT | (dump_dir ? WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE : 0u);
    RECT r{0, 0, st.px(WIN_W), st.px(WIN_H)};
    AdjustWindowRectEx(&r, style, FALSE, ex);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
    if (dump_dir) { sx = -32000; sy = -32000; } // Off-screen: never seen
    HWND hwnd = CreateWindowExW(ex, wc.lpszClassName, L"Vagrant Story — Ayarlar", style,
                                sx, sy, ww, wh, nullptr, nullptr, wc.hInstance, &st);
    MSG msg{};
    if (dump_dir) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        for (int p = 0; p < PAGE_COUNT; ++p) {
            show_page(st, p);
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            save_window_bmp(hwnd, *dump_dir / ("page_" + std::to_string(p) + ".bmp"));
        }
        DestroyWindow(hwnd);
        st.play = false;
    } else {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd);
        SetFocus(GetDlgItem(hwnd, IDOK));
        while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    DeleteObject(st.font);
    DeleteObject(st.title_font);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (SUCCEEDED(com)) CoUninitialize();

    if (st.play) {
        save_settings(settings_path(), s);
    }
    return st.play;
}

} // namespace

bool run_settings_window(GameSettings& s) { return run_window(s, nullptr); }

void dump_settings_window(GameSettings s, const std::filesystem::path& dir) { run_window(s, &dir); }
#else // !_WIN32

namespace {
std::filesystem::path g_data_dir; // Set by the SDL host (empty = current folder)
}
void set_data_dir(const std::filesystem::path& dir) { g_data_dir = dir; }
std::filesystem::path data_dir() { return g_data_dir; }
void show_message(const std::string& text, bool error) { std::fprintf(error ? stderr : stdout, "%s\n", text.c_str()); }
std::filesystem::path settings_path() { return g_data_dir / "VagrantStory.ini"; }
std::filesystem::path default_memcard_path() { return g_data_dir / "saves" / "VagrantStory_kart1.mcr"; }
std::filesystem::path cheats_path() { return g_data_dir / "VagrantStory_hileler.txt"; }
std::filesystem::path texture_pack_dir() { return g_data_dir / "dokular"; }
bool run_settings_window(GameSettings&) { return false; }
void dump_settings_window(GameSettings, const std::filesystem::path&) {}

#endif

} // namespace vsport::launcher
