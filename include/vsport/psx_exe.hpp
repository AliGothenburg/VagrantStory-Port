#pragma once

#include "vsport/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vsport {

// Sony PlayStation PS-X EXE Header (loaded from the first 2048 bytes of SLUS_010.40)
struct PsxExeHeader {
    u32 pc0    = 0; // Initial Program Counter (e.g. 0x80010000+)
    u32 gp0    = 0; // Initial Global Pointer ($gp)
    u32 t_addr = 0; // Destination RAM address of text/data payload
    u32 t_size = 0; // Payload size in bytes (multiple of 2048)
    u32 b_addr = 0; // BSS start address to zero-fill
    u32 b_size = 0; // BSS size in bytes
    u32 s_addr = 0; // Stack base address
    u32 s_size = 0; // Stack offset
    std::string ascii_marker; // Region license text at 0x4C
};

struct PsxExeImage {
    PsxExeHeader header;
    std::vector<u8> payload; // Raw bytes loaded to header.t_addr
};

// Vagrant Story (.PRG) Overlay Slot Classification
enum class VsOverlaySlot : u8 {
    ResidentCore = 0, // SLUS_010.40
    PrimaryEngine,    // TITLE.PRG / BATTLE.PRG / ENDING.PRG (0x80068800)
    MenuManager,      // MAINMENU.PRG
    SubMenu,          // MENU0.PRG .. MENUF.PRG
    BattleAux,        // INITBTL.PRG, SCREFF2.PRG, etc.
    UnknownOverlay
};

struct VsOverlayProfile {
    std::string name;
    VsOverlaySlot slot    = VsOverlaySlot::UnknownOverlay;
    u32 expected_ram_base = 0;
    std::string description;
};

struct PrgAnalysis {
    std::string name;
    VsOverlaySlot slot       = VsOverlaySlot::UnknownOverlay;
    u32 byte_size            = 0;
    u32 inferred_base_addr   = 0; // Inferred or known KSEG0 load base
    u32 code_start_offset    = 0; // Offset where MIPS instructions begin
    u32 code_end_offset      = 0; // Offset where MIPS instructions end
    u32 valid_inst_count     = 0;
    u32 jal_count            = 0;
    u32 gte_inst_count       = 0;
    u32 prologue_count       = 0;
};

// Parses a 2048-byte-headed PS-X EXE binary
bool parse_psx_exe(std::span<const u8> bytes, PsxExeImage& out_exe, std::string* err_out = nullptr);

// Builds a minimal valid PS-X EXE byte buffer (useful for unit tests and homebrew payloads)
std::vector<u8> build_psx_exe(u32 t_addr, u32 pc0, u32 gp0, u32 sp_base, std::span<const u32> code_words);

// Looks up known Vagrant Story (SLUS_010.40) overlay metadata by filename
[[nodiscard]] std::optional<VsOverlayProfile> lookup_vs_overlay_profile(std::string_view filename);

// Analyzes a Vagrant Story .PRG overlay binary to locate its MIPS code partition & base address
[[nodiscard]] PrgAnalysis analyze_vs_prg(std::string_view filename, std::span<const u8> bytes);

} // namespace vsport
