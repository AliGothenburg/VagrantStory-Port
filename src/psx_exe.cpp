#include "vsport/psx_exe.hpp"
#include "vsport/mips_decoder.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

namespace vsport {

namespace {

u32 read_le32(const u8* p) noexcept {
    return static_cast<u32>(p[0]) |
           (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

void write_le32(u8* p, u32 v) noexcept {
    p[0] = static_cast<u8>(v & 0xFFu);
    p[1] = static_cast<u8>((v >> 8) & 0xFFu);
    p[2] = static_cast<u8>((v >> 16) & 0xFFu);
    p[3] = static_cast<u8>((v >> 24) & 0xFFu);
}

std::string upper_basename(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    if (slash != std::string_view::npos) {
        path.remove_prefix(slash + 1);
    }
    std::string out(path);
    const auto semi = out.find(';');
    if (semi != std::string::npos) out.erase(semi);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

} // namespace

bool parse_psx_exe(std::span<const u8> bytes, PsxExeImage& out_exe, std::string* err_out) {
    if (bytes.size() < 2048) {
        if (err_out) *err_out = "PS-X EXE boyutu 2048 bayttan kucuk.";
        return false;
    }

    if (std::memcmp(bytes.data(), "PS-X EXE", 8) != 0) {
        if (err_out) *err_out = "Gecersiz PS-X EXE imzasi ('PS-X EXE' beklendi).";
        return false;
    }

    PsxExeHeader hdr{};
    hdr.pc0    = read_le32(&bytes[0x10]);
    hdr.gp0    = read_le32(&bytes[0x14]);
    hdr.t_addr = read_le32(&bytes[0x18]);
    hdr.t_size = read_le32(&bytes[0x1C]);
    hdr.b_addr = read_le32(&bytes[0x28]);
    hdr.b_size = read_le32(&bytes[0x2C]);
    hdr.s_addr = read_le32(&bytes[0x30]);
    hdr.s_size = read_le32(&bytes[0x34]);

    const char* marker_ptr = reinterpret_cast<const char*>(&bytes[0x4C]);
    std::size_t marker_len = 0;
    while (marker_len < 128 && marker_ptr[marker_len] != '\0') {
        ++marker_len;
    }
    hdr.ascii_marker.assign(marker_ptr, marker_len);

    const std::size_t avail_payload = bytes.size() - 2048;
    const std::size_t copy_len = (hdr.t_size > 0 && hdr.t_size <= avail_payload) ? hdr.t_size : avail_payload;

    out_exe.header = hdr;
    out_exe.payload.assign(bytes.begin() + 2048, bytes.begin() + 2048 + static_cast<std::ptrdiff_t>(copy_len));
    return true;
}

std::vector<u8> build_psx_exe(u32 t_addr, u32 pc0, u32 gp0, u32 sp_base, std::span<const u32> code_words) {
    const u32 raw_code_bytes = static_cast<u32>(code_words.size() * sizeof(u32));
    const u32 aligned_size   = (raw_code_bytes + 2047u) & ~2047u;

    std::vector<u8> exe(2048u + aligned_size, 0);
    std::memcpy(exe.data(), "PS-X EXE", 8);
    write_le32(&exe[0x10], pc0);
    write_le32(&exe[0x14], gp0);
    write_le32(&exe[0x18], t_addr);
    write_le32(&exe[0x1C], aligned_size);
    write_le32(&exe[0x30], sp_base);
    write_le32(&exe[0x34], 0);

    const char* license = "Sony Computer Entertainment Inc. for North America area";
    std::memcpy(&exe[0x4C], license, std::strlen(license));

    for (std::size_t i = 0; i < code_words.size(); ++i) {
        write_le32(&exe[2048u + i * 4u], code_words[i]);
    }
    return exe;
}

std::optional<VsOverlayProfile> lookup_vs_overlay_profile(std::string_view filename) {
    const std::string base = upper_basename(filename);

    if (base == "SLUS_010.40" || base == "SLES_027.54" || base == "SLPS_023.77") {
        return VsOverlayProfile{base, VsOverlaySlot::ResidentCore, 0x80010000u,
                                "Resident core executable (PsyQ, system startup, global state)"};
    }
    if (base == "BATTLE.PRG") {
        return VsOverlayProfile{base, VsOverlaySlot::PrimaryEngine, 0x80068800u,
                                "Main 3D field, actor physics, collision and battle engine"};
    }
    if (base == "TITLE.PRG") {
        return VsOverlayProfile{base, VsOverlaySlot::PrimaryEngine, 0x80068800u,
                                "Title screen, intro cutscenes and new-game setup"};
    }
    if (base == "ENDING.PRG") {
        return VsOverlayProfile{base, VsOverlaySlot::PrimaryEngine, 0x80068800u,
                                "Ending scenes and credits engine"};
    }
    if (base == "MAINMENU.PRG") {
        return VsOverlayProfile{base, VsOverlaySlot::MenuManager, 0x800F9800u,
                                "Main menu dispatcher and sub-menu loader (vs_mainMenu_exec)"};
    }
    if (base.rfind("MENU", 0) == 0 && base.size() >= 9 && base.substr(base.size() - 4) == ".PRG") {
        return VsOverlayProfile{base, VsOverlaySlot::SubMenu, 0x80102800u,
                                "Dynamic sub-menu module (magic, inventory, status, map, workshop, save)"};
    }
    if (base == "INITBTL.PRG" || base == "SCREFF2.PRG") {
        return VsOverlayProfile{base, VsOverlaySlot::BattleAux, 0x800F9800u,
                                "Battle start / screen effects helper overlay"};
    }
    return std::nullopt;
}

PrgAnalysis analyze_vs_prg(std::string_view filename, std::span<const u8> bytes) {
    PrgAnalysis out{};
    out.name      = upper_basename(filename);
    out.byte_size = static_cast<u32>(bytes.size());

    if (const auto prof = lookup_vs_overlay_profile(out.name)) {
        out.slot               = prof->slot;
        out.inferred_base_addr = prof->expected_ram_base;
    }

    const u32 word_count = static_cast<u32>(bytes.size() / 4u);
    if (word_count == 0) {
        return out;
    }

    // Collect stack-frame prologues and JAL targets across the entire overlay to solve the exact
    // sector-aligned KSEG0 load base (works automatically across PAL, NTSC-U, and NTSC-J builds).
    std::vector<u32> prologue_offsets;
    std::vector<u32> unique_jal_targets;
    std::map<u32, u32> jal_target_histogram;

    for (u32 w = 0; w < word_count; ++w) {
        const u32 word = read_le32(&bytes[w * 4u]);
        const DecodedInst inst = decode_mips(word);
        if (inst.op == MipsOp::ADDIU && inst.rs == reg::SP && inst.rt == reg::SP &&
            inst.simm16() < 0 && inst.simm16() >= -8192 && (inst.simm16() & 3) == 0) {
            prologue_offsets.push_back(w * 4u);
        } else if (inst.op == MipsOp::JAL) {
            const u32 target = 0x80000000u | (inst.imm26 << 2);
            if (target >= 0x80060000u && target < 0x80200000u) {
                unique_jal_targets.push_back(target);
                jal_target_histogram[target & 0xFFFFF800u]++;
            }
        }
    }

    std::sort(unique_jal_targets.begin(), unique_jal_targets.end());
    unique_jal_targets.erase(std::unique(unique_jal_targets.begin(), unique_jal_targets.end()), unique_jal_targets.end());

    if (!unique_jal_targets.empty() && !prologue_offsets.empty()) {
        std::map<u32, u32> base_votes;
        for (u32 tgt : unique_jal_targets) {
            for (u32 prol_off : prologue_offsets) {
                if (tgt >= prol_off) {
                    const u32 cand = tgt - prol_off;
                    if (cand >= 0x80060000u && cand < 0x801E0000u && (cand & 0x7FFu) == 0u) {
                        base_votes[cand]++;
                    }
                }
            }
        }
        u32 best_cand  = 0;
        u32 best_votes = 0;
        for (const auto& [cand, votes] : base_votes) {
            if (votes > best_votes) {
                best_votes = votes;
                best_cand  = cand;
            }
        }
        if (best_votes >= 2u) {
            out.inferred_base_addr = best_cand;
        }
    }

    if (out.inferred_base_addr == 0 && !jal_target_histogram.empty()) {
        for (const auto& [page, count] : jal_target_histogram) {
            if (count >= 4u) {
                out.inferred_base_addr = page;
                break;
            }
        }
    }

    // Scan in 16-instruction windows (64 bytes) to identify the contiguous MIPS code partition
    constexpr u32 WINDOW_WORDS = 16;
    bool found_code_start = false;
    u32 last_code_word    = 0;

    if (word_count < WINDOW_WORDS) {
        out.code_start_offset = 0;
        last_code_word        = word_count;
        found_code_start      = true;
    } else {
        for (u32 w = 0; w + WINDOW_WORDS <= word_count; w += WINDOW_WORDS) {
            u32 valid_in_win = 0;
            u32 prologues    = 0;
            for (u32 k = 0; k < WINDOW_WORDS; ++k) {
                const u32 word = read_le32(&bytes[(w + k) * 4u]);
                const DecodedInst inst = decode_mips(word);
                if (inst.op != MipsOp::INVALID) {
                    ++valid_in_win;
                }
                if (inst.op == MipsOp::ADDIU && inst.rs == reg::SP && inst.rt == reg::SP && inst.simm16() < 0) {
                    ++prologues;
                }
            }

            if (valid_in_win >= 15 && (prologues > 0 || found_code_start)) {
                if (!found_code_start) {
                    found_code_start      = true;
                    out.code_start_offset = w * 4u;
                }
                last_code_word = w + WINDOW_WORDS;
            }
        }
    }

    if (!found_code_start) {
        out.code_start_offset = 0;
        last_code_word        = word_count;
    } else if (!prologue_offsets.empty() && prologue_offsets.front() < out.code_start_offset) {
        out.code_start_offset = prologue_offsets.front() & ~0x3Fu;
    }
    out.code_end_offset = std::min<u32>(static_cast<u32>(bytes.size()) & ~3u, last_code_word * 4u);

    for (u32 w = out.code_start_offset / 4u; w < out.code_end_offset / 4u; ++w) {
        const u32 word = read_le32(&bytes[w * 4u]);
        const DecodedInst inst = decode_mips(word);
        if (inst.op != MipsOp::INVALID) {
            ++out.valid_inst_count;
        }
        if (inst.op == MipsOp::JAL) {
            ++out.jal_count;
        }
        if (inst.op == MipsOp::GTE_CMD || inst.op == MipsOp::MTC2 || inst.op == MipsOp::MFC2 ||
            inst.op == MipsOp::CTC2 || inst.op == MipsOp::CFC2 || inst.op == MipsOp::LWC2 || inst.op == MipsOp::SWC2) {
            ++out.gte_inst_count;
        }
        if (inst.op == MipsOp::ADDIU && inst.rs == reg::SP && inst.rt == reg::SP && inst.simm16() < 0) {
            ++out.prologue_count;
        }
    }

    return out;
}

} // namespace vsport
