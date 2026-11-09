#include "vsport/cdrom_iso.hpp"
#include "vsport/cfg_lifter.hpp"
#include "vsport/mips_decoder.hpp"
#include "vsport/psx_exe.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace vsport;

namespace {

const char* sector_fmt_str(DiscSectorFormat f) {
    switch (f) {
        case DiscSectorFormat::Cooked2048:     return "ISO 2048 B/sektor";
        case DiscSectorFormat::Raw2352Mode1:   return "BIN 2352 B/sektor (Mode 1)";
        case DiscSectorFormat::Raw2352Mode2XA: return "BIN 2352 B/sektor (PS1 CD-ROM XA Mode 2)";
    }
    return "Bilinmiyor";
}

const char* slot_str(VsOverlaySlot s) {
    switch (s) {
        case VsOverlaySlot::ResidentCore:   return "ResidentCore";
        case VsOverlaySlot::PrimaryEngine:  return "EngineSlot (0x80068800)";
        case VsOverlaySlot::MenuManager:    return "MenuMgr";
        case VsOverlaySlot::SubMenu:        return "SubMenuSlot";
        case VsOverlaySlot::BattleAux:      return "BattleAux";
        case VsOverlaySlot::UnknownOverlay: return "DynamicPRG";
    }
    return "Unknown";
}

bool ends_with_ci(std::string_view str, std::string_view suffix) {
    if (suffix.size() > str.size()) return false;
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        const char a = static_cast<char>(std::toupper(static_cast<unsigned char>(str[str.size() - suffix.size() + i])));
        const char b = static_cast<char>(std::toupper(static_cast<unsigned char>(suffix[i])));
        if (a != b) return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path disc_path;
    std::filesystem::path extract_dir;
    u32 disasm_addr  = 0;
    u32 disasm_count = 32;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--disc" || arg == "-d") && i + 1 < argc) {
            disc_path = argv[++i];
        } else if ((arg == "--extract-dir" || arg == "-o") && i + 1 < argc) {
            extract_dir = argv[++i];
        } else if (arg == "--disasm" && i + 1 < argc) {
            disasm_addr = static_cast<u32>(std::strtoul(argv[++i], nullptr, 0));
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                disasm_count = static_cast<u32>(std::strtoul(argv[++i], nullptr, 0));
            }
        } else if (disc_path.empty() && arg[0] != '-') {
            disc_path = arg;
        }
    }

    if (disc_path.empty()) {
        std::cout << "Kullanim: vs_inspect --disc <VagrantStory.cue|.bin|.iso> [--extract-dir <klasor>]\n";
        return 1;
    }

    DiscImage disc;
    std::string err;
    if (!disc.open(disc_path, &err)) {
        std::cerr << "[HATA] Disk acilamadi: " << err << "\n";
        return 1;
    }

    std::printf("=== Vagrant Story Disk Analizi ===\n");
    std::printf("Ikili Dosya   : %s\n", disc.binary_path().string().c_str());
    std::printf("Sektor Formati: %s\n", sector_fmt_str(disc.sector_format()));
    std::printf("Toplam Sektor : %u\n", disc.total_sectors());
    std::printf("Volume ID     : %s\n", disc.volume_id().c_str());
    std::printf("Toplam Kayit  : %zu dosya/klasor\n\n", disc.files().size());

    const auto cnf = disc.parse_system_cnf();
    if (cnf.has_value()) {
        std::printf("--- SYSTEM.CNF ---\n");
        std::printf("BOOT  : %s  (cozulmus yol: %s)\n", cnf->boot_raw.c_str(), cnf->boot_path.c_str());
        std::printf("TCB   : %u | EVENT: %u | STACK: 0x%08X\n\n", cnf->tcb, cnf->event, cnf->stack);

        const auto boot_bytes = disc.extract_file(cnf->boot_path);
        PsxExeImage exe{};
        if (!boot_bytes.empty() && parse_psx_exe(boot_bytes, exe, &err)) {
            const auto funcs = discover_functions(exe.header.t_addr, exe.payload, exe.header.pc0);
            std::printf("--- Ana Cekirdek Yurutulebilir (%s) ---\n", cnf->boot_path.c_str());
            std::printf("Yukleme Adresi (t_addr): 0x%08X\n", exe.header.t_addr);
            std::printf("Boyut          (t_size): 0x%08X (%u KB)\n", exe.header.t_size, exe.header.t_size / 1024u);
            std::printf("Giris Noktasi  (pc0)   : 0x%08X\n", exe.header.pc0);
            std::printf("Global Pointer (gp0)   : 0x%08X\n", exe.header.gp0);
            std::printf("Yigin          (sp)    : 0x%08X\n", exe.header.s_addr + exe.header.s_size);
            std::printf("Bolge Imzasi           : %s\n", exe.header.ascii_marker.c_str());
            std::printf("Kesfedilen Fonksiyon   : %zu adet\n\n", funcs.size());

            if (disasm_addr != 0) {
                const u32 start_k = to_kseg0(disasm_addr);
                const u32 base_k  = to_kseg0(exe.header.t_addr);
                std::printf("--- Disassembly @ 0x%08X (%u talimat) ---\n", start_k, disasm_count);
                for (u32 k = 0; k < disasm_count; ++k) {
                    const u32 cur = start_k + k * 4u;
                    if (cur < base_k || (cur - base_k + 4u) > exe.payload.size()) break;
                    const u32 off = cur - base_k;
                    const u32 w   = static_cast<u32>(exe.payload[off]) |
                                    (static_cast<u32>(exe.payload[off + 1]) << 8) |
                                    (static_cast<u32>(exe.payload[off + 2]) << 16) |
                                    (static_cast<u32>(exe.payload[off + 3]) << 24);
                    const auto di = decode_mips(w);
                    std::printf("  0x%08X: [%08X]  %s\n", cur, w, format_mips(cur, di).c_str());
                }
                std::printf("\n");
                return 0;
            }

            if (!extract_dir.empty()) {
                std::filesystem::create_directories(extract_dir);
                std::ofstream out(extract_dir / cnf->boot_path, std::ios::binary);
                out.write(reinterpret_cast<const char*>(boot_bytes.data()),
                          static_cast<std::streamsize>(boot_bytes.size()));
            }
        }
    }

    std::printf("--- Kesfedilen .PRG Overlay Modulleri ---\n");
    std::printf("%-22s | %-8s | %-9s | %-22s | %-10s | %-12s | %-6s | %-6s | %-6s\n",
                "Dosya Yolu", "LBA", "Boyut", "Overlay Yuvasi", "TahminiRAM", "Kod Araligi", "Fonk", "JAL", "GTE");
    std::printf("-----------------------------------------------------------------------------------------------------------------------------\n");

    u32 prg_count = 0;
    std::size_t total_prg_funcs = 0;
    for (const auto& entry : disc.files()) {
        if (entry.is_dir || !ends_with_ci(entry.path, ".PRG")) continue;

        const auto bytes = disc.extract_entry(entry);
        const PrgAnalysis info = analyze_vs_prg(entry.path, bytes);
        const std::size_t fn_count = (info.inferred_base_addr != 0 && bytes.size() >= 4u)
                                         ? discover_functions(info.inferred_base_addr, bytes).size()
                                         : 0u;
        total_prg_funcs += fn_count;
        ++prg_count;

        char code_range[32]{};
        std::snprintf(code_range, sizeof(code_range), "0x%X..0x%X", info.code_start_offset, info.code_end_offset);

        std::printf("%-22s | 0x%06X | %7u B | %-22s | 0x%08X | %-12s | %6zu | %6u | %6u\n",
                    entry.path.c_str(),
                    entry.lba,
                    info.byte_size,
                    slot_str(info.slot),
                    info.inferred_base_addr,
                    code_range,
                    fn_count,
                    info.jal_count,
                    info.gte_inst_count);

        if (!extract_dir.empty()) {
            const auto dest = extract_dir / entry.path;
            std::filesystem::create_directories(dest.parent_path());
            std::ofstream out(dest, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
    }

    std::printf("\nToplam %u adet .PRG overlay modulu analiz edildi (%zu overlay fonksiyonu).\n",
                prg_count, total_prg_funcs);
    if (!extract_dir.empty()) {
        std::printf("Dosyalar suraya cikarildi: %s\n", extract_dir.string().c_str());
    }
    return 0;
}
