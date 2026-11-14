#include "launcher_win32.hpp"

#include "vsport/cdrom_iso.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
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
        else if (key == "internal_scale") s.internal_scale = clamp_int(num, 1, 6);
        else if (key == "renderer")      s.renderer = clamp_int(num, 0, 1);
        else if (key == "texture_filter") s.texture_filter = num != 0;
        else if (key == "rim_light")     s.rim_light = clamp_int(num, 0, 2);
        else if (key == "pgxp")          s.pgxp = clamp_int(num, 0, 2);
        else if (key == "filter")        s.filter = clamp_int(num, 0, 2);
        else if (key == "integer_scale") s.integer_scale = num != 0;
        else if (key == "widescreen")    s.widescreen = num != 0;
        else if (key == "muted")         s.muted = num != 0;
        else if (key == "volume")        s.volume = clamp_int(num, 0, 100);
        else if (key == "show_launcher") s.show_launcher = num != 0;
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
    out << "texture_filter=" << (s.texture_filter ? 1 : 0) << "\n";
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
    ID_SHOW_CHECK, ID_PAD_BUTTON, ID_ASPECT_COMBO, ID_RIM_COMBO, ID_PGXP_COMBO, ID_RENDERER_COMBO,
    ID_PAD_COMBO0 = 300, // .. ID_PAD_COMBO0 + REMAP_COUNT - 1
    ID_PAD_DEFAULTS = 400,
};

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

    int px(int v) const { return static_cast<int>(v * dpi + 0.5f); }
};

HWND make(LauncherState& st, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
          int id, DWORD ex_style = 0) {
    HWND c = CreateWindowExW(ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, st.px(x), st.px(y), st.px(w), st.px(h),
                             st.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
    return c;
}

HWND item(const LauncherState& st, int id) { return GetDlgItem(st.hwnd, id); }

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

void build_controls(LauncherState& st) {
    GameSettings& s = *st.s;
    const DWORD tab = WS_TABSTOP;
    constexpr int W = 520;   // Client width (logical px)
    constexpr int M = 16;    // Margin
    int y = 14;

    HWND title = make(st, L"STATIC", L"Vagrant Story — Ayarlar", SS_LEFT, M, y, W - 2 * M, 28, 0);
    SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(st.title_font), TRUE);
    y += 40;

    // --- Game ---
    make(st, L"BUTTON", L"Oyun", BS_GROUPBOX, M, y, W - 2 * M, 228, 0);
    y += 24;
    make(st, L"STATIC", L"Oyun klasörü / disk imajı:", SS_LEFT, M + 12, y, 300, 18, 0);
    y += 20;
    make(st, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_BORDER, M + 12, y, 286, 24, ID_DISC_EDIT);
    make(st, L"BUTTON", L"Klasör seç…", BS_PUSHBUTTON | tab, M + 304, y, 86, 24, ID_DISC_FOLDER);
    make(st, L"BUTTON", L"Dosya seç…", BS_PUSHBUTTON | tab, M + 396, y, 80, 24, ID_DISC_FILE);
    y += 32;
    make(st, L"STATIC", L"Alt kanal dosyası (.sbi, yalnızca Avrupa sürümü):", SS_LEFT, M + 12, y, 400, 18, 0);
    y += 20;
    make(st, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_BORDER, M + 12, y, 286, 24, ID_SBI_EDIT);
    make(st, L"BUTTON", L"Dosya seç…", BS_PUSHBUTTON | tab, M + 304, y, 86, 24, ID_SBI_FILE);
    make(st, L"BUTTON", L"Kaldır", BS_PUSHBUTTON | tab, M + 396, y, 80, 24, ID_SBI_CLEAR);
    y += 32;
    make(st, L"STATIC", L"Hafıza kartı 1 (kayıtlar bu dosyada tutulur):", SS_LEFT, M + 12, y, 400, 18, 0);
    y += 20;
    make(st, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_BORDER, M + 12, y, 286, 24, ID_CARD_EDIT);
    make(st, L"BUTTON", L"Değiştir…", BS_PUSHBUTTON | tab, M + 304, y, 86, 24, ID_CARD_CHANGE);
    make(st, L"BUTTON", L"Klasörü aç", BS_PUSHBUTTON | tab, M + 396, y, 80, 24, ID_CARD_OPEN);
    y += 32;
    make(st, L"STATIC", L"", SS_LEFT, M + 12, y, W - 2 * M - 24, 36, ID_STATUS);
    y += 54;

    // --- Display ---
    make(st, L"BUTTON", L"Görüntü", BS_GROUPBOX, M, y, W - 2 * M, 390, 0);
    y += 26;
    constexpr int LX = M + 12, CX = M + 150, CW = 220;
    make(st, L"STATIC", L"İç çözünürlük:", SS_LEFT, LX, y + 3, 130, 20, 0);
    HWND res = make(st, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | tab | WS_VSCROLL, CX, y, CW, 200, ID_RES_COMBO);
    SendMessageW(res, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"1×  orijinal (320 × 240)"));
    SendMessageW(res, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"2×  (640 × 480)"));
    SendMessageW(res, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"3×  (960 × 720)"));
    SendMessageW(res, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"4×  (1280 × 960)"));
    SendMessageW(res, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"5×  (1600 × 1200) — 1080p ekranlar"));
    SendMessageW(res, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"6×  (1920 × 1440) — 1440p ekranlar"));
    SendMessageW(res, CB_SETCURSEL, clamp_int(s.internal_scale, 1, 6) - 1, 0);
    y += 32;
    make(st, L"STATIC", L"Çizim:", SS_LEFT, LX, y + 3, 130, 20, 0);
    HWND rend = make(st, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | tab | WS_VSCROLL, CX, y, CW, 200, ID_RENDERER_COMBO);
    SendMessageW(rend, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Ekran kartı (önerilen)"));
    SendMessageW(rend, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"İşlemci"));
    SendMessageW(rend, CB_SETCURSEL, clamp_int(s.renderer, 0, 1), 0);
    y += 30;
    HWND texf = make(st, L"BUTTON", L"3D dokuları yumuşat (iç çözünürlük 2× ve üstünde)", BS_AUTOCHECKBOX | tab, LX, y, 420, 20, ID_TEXFILTER_CHECK);
    SendMessageW(texf, BM_SETCHECK, s.texture_filter ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 30;
    make(st, L"STATIC", L"Karakter kenar ışığı:", SS_LEFT, LX, y + 3, 135, 20, 0);
    HWND rim = make(st, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | tab | WS_VSCROLL, CX, y, CW, 200, ID_RIM_COMBO);
    SendMessageW(rim, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Yumuşak"));
    SendMessageW(rim, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Keskin"));
    SendMessageW(rim, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Kapalı"));
    SendMessageW(rim, CB_SETCURSEL, clamp_int(s.rim_light, 0, 2), 0);
    y += 32;
    make(st, L"STATIC", L"Köşe hassasiyeti:", SS_LEFT, LX, y + 3, 135, 20, 0);
    HWND pgxp = make(st, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | tab | WS_VSCROLL, CX, y, CW, 200, ID_PGXP_COMBO);
    SendMessageW(pgxp, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Kapalı (orijinal titreme)"));
    SendMessageW(pgxp, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Titreme düzeltmesi"));
    SendMessageW(pgxp, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Titreme + perspektifli doku"));
    SendMessageW(pgxp, CB_SETCURSEL, clamp_int(s.pgxp, 0, 2), 0);
    y += 32;
    make(st, L"STATIC", L"Ekran modu:", SS_LEFT, LX, y + 3, 130, 20, 0);
    HWND mode = make(st, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | tab | WS_VSCROLL, CX, y, CW, 200, ID_MODE_COMBO);
    SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Pencere"));
    SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Tam ekran"));
    SendMessageW(mode, CB_SETCURSEL, s.fullscreen ? 1 : 0, 0);
    y += 32;
    make(st, L"STATIC", L"Görüntü oranı:", SS_LEFT, LX, y + 3, 130, 20, 0);
    HWND aspect = make(st, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | tab | WS_VSCROLL, CX, y, CW, 220, ID_ASPECT_COMBO);
    for (const wchar_t* a : {L"4:3  (orijinal)", L"10:7", L"3:2", L"16:10", L"16:9", L"Pencereyi / ekranı doldur"}) {
        SendMessageW(aspect, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(a));
    }
    SendMessageW(aspect, CB_SETCURSEL, clamp_int(s.aspect, 0, ASPECT_COUNT - 1), 0);
    y += 32;
    make(st, L"STATIC", L"Pencere boyutu:", SS_LEFT, LX, y + 3, 130, 20, 0);
    make(st, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | tab | WS_VSCROLL, CX, y, CW, 240, ID_SCALE_COMBO);
    fill_window_sizes(st, s.window_height);
    y += 32;
    make(st, L"STATIC", L"Görüntü filtresi:", SS_LEFT, LX, y + 3, 130, 20, 0);
    HWND filter = make(st, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | tab | WS_VSCROLL, CX, y, CW, 200, ID_FILTER_COMBO);
    SendMessageW(filter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Keskin piksel"));
    SendMessageW(filter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Yumuşak (bilinear)"));
    SendMessageW(filter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"CRT tarama çizgileri"));
    SendMessageW(filter, CB_SETCURSEL, clamp_int(s.filter, 0, 2), 0);
    y += 34;
    HWND integer = make(st, L"BUTTON", L"Tam sayı ölçekleme (yalnızca 1× iç çözünürlükte; kenarlarda boşluk bırakır)", BS_AUTOCHECKBOX | tab, LX, y, 470, 20, ID_INTEGER_CHECK);
    SendMessageW(integer, BM_SETCHECK, s.integer_scale ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 24;
    HWND wide = make(st, L"BUTTON", L"3D sahneyi orana göre genişlet (kapalıysa görüntü esnetilir;\n"
                                    L"deneysel: kenarlarda boşluklar görülebilir)",
                     BS_AUTOCHECKBOX | BS_MULTILINE | tab, LX, y, 470, 36, ID_WIDE_CHECK);
    SendMessageW(wide, BM_SETCHECK, s.widescreen ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 62;

    // --- Sound ---
    make(st, L"BUTTON", L"Ses", BS_GROUPBOX, M, y, W - 2 * M, 66, 0);
    y += 26;
    HWND mute = make(st, L"BUTTON", L"Sesi kapat", BS_AUTOCHECKBOX | tab, LX, y + 2, 110, 20, ID_MUTE_CHECK);
    SendMessageW(mute, BM_SETCHECK, s.muted ? BST_CHECKED : BST_UNCHECKED, 0);
    make(st, L"STATIC", L"Seviye:", SS_LEFT, LX + 130, y + 3, 50, 20, 0);
    HWND vol = make(st, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | tab, LX + 180, y, 220, 26, ID_VOLUME_SLIDER);
    SendMessageW(vol, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(vol, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(vol, TBM_SETPOS, TRUE, clamp_int(s.volume, 0, 100));
    make(st, L"STATIC", L"", SS_LEFT, LX + 406, y + 3, 50, 20, ID_VOLUME_LABEL);
    y += 52;

    // --- Footer ---
    HWND show = make(st, L"BUTTON", L"Bu pencereyi her açılışta göster  (kapalıyken oyunu Shift ile başlatın)",
                     BS_AUTOCHECKBOX | tab, M, y, W - 2 * M, 20, ID_SHOW_CHECK);
    SendMessageW(show, BM_SETCHECK, s.show_launcher ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 32;
    make(st, L"BUTTON", L"Kontrolcü tuşları…", BS_PUSHBUTTON | tab, M, y, 150, 30, ID_PAD_BUTTON);
    make(st, L"BUTTON", L"Çıkış", BS_PUSHBUTTON | tab, W - M - 100 - 8 - 120, y, 100, 30, IDCANCEL);
    make(st, L"BUTTON", L"Oyna", BS_DEFPUSHBUTTON | tab, W - M - 120, y, 120, 30, IDOK);

    update_status(st);
    update_volume_label(st);
}

void read_controls(LauncherState& st) {
    GameSettings& s = *st.s;
    s.fullscreen    = SendMessageW(item(st, ID_MODE_COMBO), CB_GETCURSEL, 0, 0) == 1;
    s.window_height = selected_window_height(st);
    s.aspect        = clamp_int(static_cast<int>(SendMessageW(item(st, ID_ASPECT_COMBO), CB_GETCURSEL, 0, 0)), 0, ASPECT_COUNT - 1);
    s.internal_scale = clamp_int(static_cast<int>(SendMessageW(item(st, ID_RES_COMBO), CB_GETCURSEL, 0, 0)) + 1, 1, 6);
    s.renderer      = clamp_int(static_cast<int>(SendMessageW(item(st, ID_RENDERER_COMBO), CB_GETCURSEL, 0, 0)), 0, 1);
    s.texture_filter = SendMessageW(item(st, ID_TEXFILTER_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.rim_light     = clamp_int(static_cast<int>(SendMessageW(item(st, ID_RIM_COMBO), CB_GETCURSEL, 0, 0)), 0, 2);
    s.pgxp          = clamp_int(static_cast<int>(SendMessageW(item(st, ID_PGXP_COMBO), CB_GETCURSEL, 0, 0)), 0, 2);
    s.filter        = clamp_int(static_cast<int>(SendMessageW(item(st, ID_FILTER_COMBO), CB_GETCURSEL, 0, 0)), 0, 2);
    s.integer_scale = SendMessageW(item(st, ID_INTEGER_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.widescreen    = SendMessageW(item(st, ID_WIDE_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.muted         = SendMessageW(item(st, ID_MUTE_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
    s.volume        = clamp_int(static_cast<int>(SendMessageW(item(st, ID_VOLUME_SLIDER), TBM_GETPOS, 0, 0)), 0, 100);
    s.show_launcher = SendMessageW(item(st, ID_SHOW_CHECK), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

// ---------------------------------------------------------------------------------------------
// Controller button window (modal over the settings window)
// ---------------------------------------------------------------------------------------------

constexpr const wchar_t* PAD_WND_CLASS = L"VagrantStoryPadMapWnd";

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

struct PadDialogState {
    LauncherState ui;              // hwnd = the pad window; shares font / DPI with the launcher
    pad::ButtonMap* map = nullptr; // Written only on "Tamam"
    bool done = false;
};

void pad_dialog_select(PadDialogState& st, const pad::ButtonMap& m) {
    for (std::size_t i = 0; i < pad::REMAP_COUNT; ++i) {
        const int src = m[i];
        // Combo entry 0 = "(atanmamış)", 1.. = physical buttons
        SendMessageW(item(st.ui, ID_PAD_COMBO0 + static_cast<int>(i)), CB_SETCURSEL,
                     src >= 0 && src < static_cast<int>(pad::REMAP_COUNT) ? src + 1 : 0, 0);
    }
}

LRESULT CALLBACK PadWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* st = reinterpret_cast<PadDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            st = static_cast<PadDialogState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
            st->ui.hwnd = hwnd;
            LauncherState& ui = st->ui;
            constexpr int W = 420, M = 16;
            int y = 12;
            make(ui, L"STATIC", L"Her PS1 tuşu için kontrolcünüzde hangi tuşun kullanılacağını seçin.\n"
                                L"Yön tuşları ve sol çubuk her zaman yön olarak çalışır.",
                 SS_LEFT, M, y, W - 2 * M, 36, 0);
            y += 46;
            for (std::size_t i = 0; i < pad::REMAP_COUNT; ++i) {
                make(ui, L"STATIC", PS1_BUTTON_NAMES[i], SS_LEFT, M, y + 3, 110, 20, 0);
                HWND c = make(ui, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, M + 120, y, W - 2 * M - 120,
                              300, ID_PAD_COMBO0 + static_cast<int>(i));
                SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(atanmamış)"));
                for (const wchar_t* name : PHYSICAL_BUTTON_NAMES) {
                    SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
                }
                y += 30;
            }
            y += 10;
            make(ui, L"BUTTON", L"Varsayılana döndür", BS_PUSHBUTTON | WS_TABSTOP, M, y, 140, 30, ID_PAD_DEFAULTS);
            make(ui, L"BUTTON", L"İptal", BS_PUSHBUTTON | WS_TABSTOP, W - M - 90 - 8 - 90, y, 90, 30, IDCANCEL);
            make(ui, L"BUTTON", L"Tamam", BS_DEFPUSHBUTTON | WS_TABSTOP, W - M - 90, y, 90, 30, IDOK);
            pad_dialog_select(*st, *st->map);
            return 0;
        }
        case WM_COMMAND:
            if (!st) break;
            switch (LOWORD(wparam)) {
                case ID_PAD_DEFAULTS:
                    pad_dialog_select(*st, pad::default_button_map());
                    return 0;
                case IDOK:
                    for (std::size_t i = 0; i < pad::REMAP_COUNT; ++i) {
                        const auto sel = SendMessageW(item(st->ui, ID_PAD_COMBO0 + static_cast<int>(i)), CB_GETCURSEL, 0, 0);
                        (*st->map)[i] = static_cast<s8>(sel <= 0 ? -1 : sel - 1);
                    }
                    DestroyWindow(hwnd);
                    return 0;
                case IDCANCEL:
                    DestroyWindow(hwnd);
                    return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (st) st->done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void run_pad_dialog(LauncherState& owner) {
    PadDialogState st;
    st.ui.dpi        = owner.dpi;
    st.ui.font       = owner.font;
    st.ui.title_font = owner.title_font;
    st.map           = &owner.s->pad_map;

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT r{0, 0, owner.px(420), owner.px(12 + 46 + static_cast<int>(pad::REMAP_COUNT) * 30 + 10 + 30 + 14)};
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME);
    RECT o{};
    GetWindowRect(owner.hwnd, &o);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    const int x = o.left + ((o.right - o.left) - ww) / 2, y = o.top + ((o.bottom - o.top) - wh) / 2;

    EnableWindow(owner.hwnd, FALSE);
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME, PAD_WND_CLASS, L"Kontrolcü tuşları", style,
                                x, std::max(0, y), ww, wh, owner.hwnd, nullptr, GetModuleHandleW(nullptr), &st);
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        SetFocus(GetDlgItem(hwnd, IDOK));
        MSG msg{};
        while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    EnableWindow(owner.hwnd, TRUE);
    SetForegroundWindow(owner.hwnd);
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
            switch (LOWORD(wparam)) {
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
                case ID_PAD_BUTTON:
                    run_pad_dialog(*st);
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

bool run_settings_window(GameSettings& s) {
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
    WNDCLASSEXW pc = wc;
    pc.lpfnWndProc   = PadWndProc;
    pc.lpszClassName = PAD_WND_CLASS;
    RegisterClassExW(&pc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, st.px(520), st.px(880)};
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_CONTROLPARENT);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    const int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Vagrant Story — Ayarlar", style,
                                sx, sy, ww, wh, nullptr, nullptr, wc.hInstance, &st);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    SetFocus(GetDlgItem(hwnd, IDOK));

    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    DeleteObject(st.font);
    DeleteObject(st.title_font);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    UnregisterClassW(PAD_WND_CLASS, wc.hInstance);
    if (SUCCEEDED(com)) CoUninitialize();

    if (st.play) {
        save_settings(settings_path(), s);
    }
    return st.play;
}

#else // !_WIN32

void show_message(const std::string&, bool) {}
std::filesystem::path settings_path() { return "VagrantStory.ini"; }
std::filesystem::path default_memcard_path() { return "saves/VagrantStory_kart1.mcr"; }
bool run_settings_window(GameSettings&) { return false; }

#endif

} // namespace vsport::launcher
