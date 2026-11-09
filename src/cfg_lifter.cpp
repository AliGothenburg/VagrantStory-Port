#include "vsport/cfg_lifter.hpp"
#include "vsport/mips_decoder.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace vsport {

namespace {

u32 read_word_at(u32 addr, u32 base_kseg0, std::span<const u8> bytes) noexcept {
    const u32 off = addr - base_kseg0;
    if (off + 4u > bytes.size()) return 0;
    return static_cast<u32>(bytes[off + 0]) |
           (static_cast<u32>(bytes[off + 1]) << 8) |
           (static_cast<u32>(bytes[off + 2]) << 16) |
           (static_cast<u32>(bytes[off + 3]) << 24);
}

std::string hex32(u32 v) {
    char b[16]{};
    std::snprintf(b, sizeof(b), "0x%08Xu", v);
    return std::string(b);
}

std::string fn_symbol(u32 addr) {
    char b[32]{};
    std::snprintf(b, sizeof(b), "func_%08X", to_kseg0(addr));
    return std::string(b);
}

std::string loc_symbol(u32 addr) {
    char b[32]{};
    std::snprintf(b, sizeof(b), "loc_%08X", to_kseg0(addr));
    return std::string(b);
}

// Label placed right after a delay-slot instruction that is also a branch target, so the
// fall-through path of the preceding branch/call can skip its second (label) emission.
std::string ds_skip_symbol(u32 ds_addr) {
    char b[32]{};
    std::snprintf(b, sizeof(b), "dsk_%08X", to_kseg0(ds_addr));
    return std::string(b);
}

void emit_non_branch_inst(std::ostringstream& os, u32 pc, const DecodedInst& i, const char* indent) {
    switch (i.op) {
        case MipsOp::NOP:
            break;
        case MipsOp::SLL:
            os << indent << "ctx.set_reg(" << int(i.rd) << ", ctx.get_reg(" << int(i.rt) << ") << " << int(i.shamt) << "u);\n";
            break;
        case MipsOp::SRL:
            os << indent << "ctx.set_reg(" << int(i.rd) << ", ctx.get_reg(" << int(i.rt) << ") >> " << int(i.shamt) << "u);\n";
            break;
        case MipsOp::SRA:
            os << indent << "ctx.set_reg(" << int(i.rd) << ", static_cast<u32>(static_cast<s32>(ctx.get_reg(" << int(i.rt) << ")) >> " << int(i.shamt) << "u));\n";
            break;
        case MipsOp::SLLV:
            os << indent << "ctx.set_reg(" << int(i.rd) << ", ctx.get_reg(" << int(i.rt) << ") << (ctx.get_reg(" << int(i.rs) << ") & 31u));\n";
            break;
        case MipsOp::SRLV:
            os << indent << "ctx.set_reg(" << int(i.rd) << ", ctx.get_reg(" << int(i.rt) << ") >> (ctx.get_reg(" << int(i.rs) << ") & 31u));\n";
            break;
        case MipsOp::SRAV:
            os << indent << "ctx.set_reg(" << int(i.rd) << ", static_cast<u32>(static_cast<s32>(ctx.get_reg(" << int(i.rt) << ")) >> (ctx.get_reg(" << int(i.rs) << ") & 31u)));\n";
            break;
        case MipsOp::ADD:
        case MipsOp::ADDU:
            os << indent << "ctx.set_reg(" << int(i.rd) << ", ctx.get_reg(" << int(i.rs) << ") + ctx.get_reg(" << int(i.rt) << "));\n";
            break;
        case MipsOp::SUB:
        case MipsOp::SUBU:
            os << indent << "ctx.set_reg(" << int(i.rd) << ", ctx.get_reg(" << int(i.rs) << ") - ctx.get_reg(" << int(i.rt) << "));\n";
            break;
        case MipsOp::AND:
            os << indent << "ctx.set_reg(" << int(i.rd) << ", ctx.get_reg(" << int(i.rs) << ") & ctx.get_reg(" << int(i.rt) << "));\n";
            break;
        case MipsOp::OR:
            os << indent << "ctx.set_reg(" << int(i.rd) << ", ctx.get_reg(" << int(i.rs) << ") | ctx.get_reg(" << int(i.rt) << "));\n";
            break;
        case MipsOp::XOR:
            os << indent << "ctx.set_reg(" << int(i.rd) << ", ctx.get_reg(" << int(i.rs) << ") ^ ctx.get_reg(" << int(i.rt) << "));\n";
            break;
        case MipsOp::NOR:
            os << indent << "ctx.set_reg(" << int(i.rd) << ", ~(ctx.get_reg(" << int(i.rs) << ") | ctx.get_reg(" << int(i.rt) << ")));\n";
            break;
        case MipsOp::SLT:
            os << indent << "ctx.set_reg(" << int(i.rd) << ", (static_cast<s32>(ctx.get_reg(" << int(i.rs) << ")) < static_cast<s32>(ctx.get_reg(" << int(i.rt) << "))) ? 1u : 0u);\n";
            break;
        case MipsOp::SLTU:
            os << indent << "ctx.set_reg(" << int(i.rd) << ", (ctx.get_reg(" << int(i.rs) << ") < ctx.get_reg(" << int(i.rt) << ")) ? 1u : 0u);\n";
            break;
        case MipsOp::ADDI:
        case MipsOp::ADDIU:
            os << indent << "ctx.set_reg(" << int(i.rt) << ", ctx.get_reg(" << int(i.rs) << ") + static_cast<u32>(" << i.simm16() << "));\n";
            break;
        case MipsOp::SLTI:
            os << indent << "ctx.set_reg(" << int(i.rt) << ", (static_cast<s32>(ctx.get_reg(" << int(i.rs) << ")) < " << i.simm16() << ") ? 1u : 0u);\n";
            break;
        case MipsOp::SLTIU:
            os << indent << "ctx.set_reg(" << int(i.rt) << ", (ctx.get_reg(" << int(i.rs) << ") < static_cast<u32>(" << i.simm16() << ")) ? 1u : 0u);\n";
            break;
        case MipsOp::ANDI:
            os << indent << "ctx.set_reg(" << int(i.rt) << ", ctx.get_reg(" << int(i.rs) << ") & " << hex32(i.uimm16()) << ");\n";
            break;
        case MipsOp::ORI:
            os << indent << "ctx.set_reg(" << int(i.rt) << ", ctx.get_reg(" << int(i.rs) << ") | " << hex32(i.uimm16()) << ");\n";
            break;
        case MipsOp::XORI:
            os << indent << "ctx.set_reg(" << int(i.rt) << ", ctx.get_reg(" << int(i.rs) << ") ^ " << hex32(i.uimm16()) << ");\n";
            break;
        case MipsOp::LUI:
            os << indent << "ctx.set_reg(" << int(i.rt) << ", " << hex32(i.uimm16() << 16) << ");\n";
            break;
        case MipsOp::MFHI:
            os << indent << "ctx.set_reg(" << int(i.rd) << ", ctx.hi);\n";
            break;
        case MipsOp::MTHI:
            os << indent << "ctx.hi = ctx.get_reg(" << int(i.rs) << ");\n";
            break;
        case MipsOp::MFLO:
            os << indent << "ctx.set_reg(" << int(i.rd) << ", ctx.lo);\n";
            break;
        case MipsOp::MTLO:
            os << indent << "ctx.lo = ctx.get_reg(" << int(i.rs) << ");\n";
            break;
        case MipsOp::MULT:
            os << indent << "{ const s64 p = static_cast<s64>(static_cast<s32>(ctx.get_reg(" << int(i.rs)
               << "))) * static_cast<s64>(static_cast<s32>(ctx.get_reg(" << int(i.rt)
               << "))); ctx.lo = static_cast<u32>(p); ctx.hi = static_cast<u32>(static_cast<u64>(p) >> 32); }\n";
            break;
        case MipsOp::MULTU:
            os << indent << "{ const u64 p = static_cast<u64>(ctx.get_reg(" << int(i.rs)
               << ")) * static_cast<u64>(ctx.get_reg(" << int(i.rt)
               << ")); ctx.lo = static_cast<u32>(p); ctx.hi = static_cast<u32>(p >> 32); }\n";
            break;
        case MipsOp::DIV:
            os << indent << "{ const s32 n = static_cast<s32>(ctx.get_reg(" << int(i.rs)
               << ")), d = static_cast<s32>(ctx.get_reg(" << int(i.rt)
               << ")); if (d == 0) { ctx.lo = (n >= 0) ? 0xFFFFFFFFu : 1u; ctx.hi = static_cast<u32>(n); } "
               << "else if (static_cast<u32>(n) == 0x80000000u && d == -1) { ctx.lo = 0x80000000u; ctx.hi = 0u; } "
               << "else { ctx.lo = static_cast<u32>(n / d); ctx.hi = static_cast<u32>(n % d); } }\n";
            break;
        case MipsOp::DIVU:
            os << indent << "{ const u32 n = ctx.get_reg(" << int(i.rs) << "), d = ctx.get_reg(" << int(i.rt)
               << "); if (d == 0) { ctx.lo = 0xFFFFFFFFu; ctx.hi = n; } else { ctx.lo = n / d; ctx.hi = n % d; } }\n";
            break;
        case MipsOp::LB:
            os << indent << "ctx.set_reg(" << int(i.rt) << ", static_cast<u32>(static_cast<s32>(static_cast<s8>(rt.read8(ctx.get_reg(" << int(i.rs) << ") + static_cast<u32>(" << i.simm16() << "))))));\n";
            break;
        case MipsOp::LBU:
            os << indent << "ctx.set_reg(" << int(i.rt) << ", rt.read8(ctx.get_reg(" << int(i.rs) << ") + static_cast<u32>(" << i.simm16() << ")));\n";
            break;
        case MipsOp::LH:
            os << indent << "ctx.set_reg(" << int(i.rt) << ", static_cast<u32>(static_cast<s32>(static_cast<s16>(rt.read16(ctx.get_reg(" << int(i.rs) << ") + static_cast<u32>(" << i.simm16() << "))))));\n";
            break;
        case MipsOp::LHU:
            os << indent << "ctx.set_reg(" << int(i.rt) << ", rt.read16(ctx.get_reg(" << int(i.rs) << ") + static_cast<u32>(" << i.simm16() << ")));\n";
            break;
        case MipsOp::LW:
            os << indent << "ctx.set_reg(" << int(i.rt) << ", rt.read32(ctx.get_reg(" << int(i.rs) << ") + static_cast<u32>(" << i.simm16() << ")));\n";
            break;
        case MipsOp::LWL:
            os << indent << "ctx.set_reg(" << int(i.rt) << ", rt.exec_lwl(ctx.get_reg(" << int(i.rs) << ") + static_cast<u32>(" << i.simm16() << "), ctx.get_reg(" << int(i.rt) << ")));\n";
            break;
        case MipsOp::LWR:
            os << indent << "ctx.set_reg(" << int(i.rt) << ", rt.exec_lwr(ctx.get_reg(" << int(i.rs) << ") + static_cast<u32>(" << i.simm16() << "), ctx.get_reg(" << int(i.rt) << ")));\n";
            break;
        case MipsOp::SB:
            os << indent << "rt.write8(ctx.get_reg(" << int(i.rs) << ") + static_cast<u32>(" << i.simm16() << "), static_cast<u8>(ctx.get_reg(" << int(i.rt) << ")));\n";
            break;
        case MipsOp::SH:
            os << indent << "rt.write16(ctx.get_reg(" << int(i.rs) << ") + static_cast<u32>(" << i.simm16() << "), static_cast<u16>(ctx.get_reg(" << int(i.rt) << ")));\n";
            break;
        case MipsOp::SW:
            os << indent << "rt.write32(ctx.get_reg(" << int(i.rs) << ") + static_cast<u32>(" << i.simm16() << "), ctx.get_reg(" << int(i.rt) << "));\n";
            break;
        case MipsOp::SWL:
            os << indent << "rt.exec_swl(ctx.get_reg(" << int(i.rs) << ") + static_cast<u32>(" << i.simm16() << "), ctx.get_reg(" << int(i.rt) << "));\n";
            break;
        case MipsOp::SWR:
            os << indent << "rt.exec_swr(ctx.get_reg(" << int(i.rs) << ") + static_cast<u32>(" << i.simm16() << "), ctx.get_reg(" << int(i.rt) << "));\n";
            break;
        case MipsOp::SYSCALL:
            os << indent << "rt.handle_syscall(ctx);\n";
            break;
        case MipsOp::BREAK:
            os << indent << "ctx.halted = true; return;\n";
            break;
        case MipsOp::MFC0:
            os << indent << "ctx.set_reg(" << int(i.rt) << ", ctx.cop0[" << int(i.rd & 31u) << "]);\n";
            break;
        case MipsOp::MTC0:
            os << indent << "ctx.cop0[" << int(i.rd & 31u) << "] = ctx.get_reg(" << int(i.rt) << ");\n";
            break;
        case MipsOp::RFE:
            os << indent << "ctx.cop0[12] = (ctx.cop0[12] & ~0x0Fu) | ((ctx.cop0[12] >> 2) & 0x0Fu);\n";
            break;
        case MipsOp::MFC2:
            os << indent << "ctx.set_reg(" << int(i.rt) << ", rt.gte().read_data(" << int(i.rd) << "));\n";
            break;
        case MipsOp::CFC2:
            os << indent << "ctx.set_reg(" << int(i.rt) << ", rt.gte().read_ctrl(" << int(i.rd) << "));\n";
            break;
        case MipsOp::MTC2:
            os << indent << "rt.gte().write_data(" << int(i.rd) << ", ctx.get_reg(" << int(i.rt) << "));\n";
            break;
        case MipsOp::CTC2:
            os << indent << "rt.gte().write_ctrl(" << int(i.rd) << ", ctx.get_reg(" << int(i.rt) << "));\n";
            break;
        case MipsOp::LWC2:
            os << indent << "rt.gte().write_data(" << int(i.rt) << ", rt.read32(ctx.get_reg(" << int(i.rs) << ") + static_cast<u32>(" << i.simm16() << ")));\n";
            break;
        case MipsOp::SWC2:
            os << indent << "rt.write32(ctx.get_reg(" << int(i.rs) << ") + static_cast<u32>(" << i.simm16() << "), rt.gte().read_data(" << int(i.rt) << "));\n";
            break;
        case MipsOp::GTE_CMD:
            os << indent << "rt.gte().execute(" << hex32(i.raw) << ");\n";
            break;
        default:
            os << indent << "// Unsupported/Data word @ " << hex32(pc) << ": " << hex32(i.raw) << "\n";
            os << indent << "ctx.halted = true; return;\n";
            break;
    }
}

bool looks_like_code_window(u32 pc, u32 base_kseg0, u32 end_kseg0, std::span<const u8> bytes) noexcept {
    u32 checked = 0;
    for (u32 off = 0; off < 16u && (pc + off) < end_kseg0; off += 4u) {
        const u32 w = read_word_at(pc + off, base_kseg0, bytes);
        if (w == 0xFFFFFFFFu) return false;
        const DecodedInst di = decode_mips(w);
        if (di.op == MipsOp::INVALID) return false;
        ++checked;
    }
    return checked >= 2u;
}

} // namespace

std::vector<DiscoveredFunction> discover_functions(
    u32 base_kseg0,
    std::span<const u8> code_bytes,
    u32 entry_pc) {
    base_kseg0 = to_kseg0(base_kseg0);
    const u32 end_kseg0 = base_kseg0 + static_cast<u32>(code_bytes.size() & ~3ull);

    std::set<u32> starts;
    if (entry_pc != 0) {
        const u32 ep = to_kseg0(entry_pc);
        if (ep >= base_kseg0 && ep < end_kseg0) {
            starts.insert(ep);
        }
    } else if (base_kseg0 < end_kseg0) {
        const u32 w0 = read_word_at(base_kseg0, base_kseg0, code_bytes);
        const DecodedInst d0 = decode_mips(w0);
        if (w0 != 0 && d0.op != MipsOp::INVALID && d0.op != MipsOp::BREAK &&
            (w0 & 0xFFE00000u) != 0x80000000u &&
            looks_like_code_window(base_kseg0, base_kseg0, end_kseg0, code_bytes)) {
            starts.insert(base_kseg0);
        }
    }

    // Pass 1: Collect verified JAL targets and stack frame prologues (excluding .data tables)
    for (u32 pc = base_kseg0; pc < end_kseg0; pc += 4u) {
        const u32 raw = read_word_at(pc, base_kseg0, code_bytes);
        const DecodedInst inst = decode_mips(raw);
        if (inst.op == MipsOp::JAL) {
            const u32 tgt = to_kseg0(inst.jump_target(pc));
            if (tgt >= base_kseg0 && tgt < end_kseg0 && (tgt & 3u) == 0u &&
                looks_like_code_window(pc, base_kseg0, end_kseg0, code_bytes) &&
                looks_like_code_window(tgt, base_kseg0, end_kseg0, code_bytes)) {
                starts.insert(tgt);
            }
        } else if (inst.op == MipsOp::ADDIU && inst.rs == reg::SP && inst.rt == reg::SP &&
                   inst.simm16() < 0 && inst.simm16() >= -8192 && (inst.simm16() & 3) == 0) {
            if (looks_like_code_window(pc, base_kseg0, end_kseg0, code_bytes)) {
                bool has_sp_store_or_ret = false;
                for (u32 k = 4u; k <= 24u && (pc + k) < end_kseg0; k += 4u) {
                    const DecodedInst next_i = decode_mips(read_word_at(pc + k, base_kseg0, code_bytes));
                    if ((next_i.op == MipsOp::SW || next_i.op == MipsOp::SH || next_i.op == MipsOp::SB) &&
                        next_i.rs == reg::SP && next_i.simm16() >= 0) {
                        has_sp_store_or_ret = true;
                        break;
                    }
                    if (next_i.is_return()) {
                        has_sp_store_or_ret = true;
                        break;
                    }
                }
                if (has_sp_store_or_ret) {
                    starts.insert(pc);
                }
            }
        }
    }

    std::vector<u32> sorted_starts(starts.begin(), starts.end());
    std::vector<DiscoveredFunction> funcs;
    funcs.reserve(sorted_starts.size());

    for (std::size_t idx = 0; idx < sorted_starts.size(); ++idx) {
        DiscoveredFunction fn{};
        fn.start_addr = sorted_starts[idx];
        const u32 next_start = (idx + 1 < sorted_starts.size()) ? sorted_starts[idx + 1] : end_kseg0;
        const u32 hard_cap   = std::min<u32>(end_kseg0, fn.start_addr + 32768u);

        u32 max_local_branch       = fn.start_addr;
        bool invalid_or_switch     = false;
        bool modifies_ra_or_sp     = false;
        bool ended_with_terminator = false;
        u32 chosen_end             = 0;

        for (u32 pc = fn.start_addr; pc < hard_cap; pc += 4u) {
            const DecodedInst inst = decode_mips(read_word_at(pc, base_kseg0, code_bytes));
            if (inst.op == MipsOp::INVALID) {
                invalid_or_switch = true;
                break;
            }

            if (inst.op == MipsOp::JAL || inst.op == MipsOp::JALR ||
                inst.op == MipsOp::BLTZAL || inst.op == MipsOp::BGEZAL ||
                (inst.op == MipsOp::ADDIU && inst.rt == reg::SP)) {
                modifies_ra_or_sp = true;
            }

            // Detect switch-table indirect jump (jr $reg where $reg != $ra inside a non-thunk function)
            if (inst.op == MipsOp::JR && inst.rs != reg::RA) {
                if (modifies_ra_or_sp || (pc - fn.start_addr) > 24u) {
                    invalid_or_switch = true;
                    break;
                }
            }

            if (inst.is_branch() && inst.op != MipsOp::BLTZAL && inst.op != MipsOp::BGEZAL) {
                const u32 bt = to_kseg0(inst.branch_target(pc));
                if (bt < fn.start_addr || bt >= hard_cap || (bt - fn.start_addr) > 16384u) {
                    invalid_or_switch = true;
                    break;
                }
                fn.local_labels.insert(bt);
                max_local_branch = std::max(max_local_branch, bt);
            } else if (inst.op == MipsOp::J) {
                const u32 jt = to_kseg0(inst.jump_target(pc));
                if (jt >= fn.start_addr && jt <= pc) {
                    fn.local_labels.insert(jt);
                } else if (jt > pc && jt < next_start) {
                    fn.local_labels.insert(jt);
                    max_local_branch = std::max(max_local_branch, jt);
                } else if (modifies_ra_or_sp) {
                    // External J after modifying $ra/$sp: exclude unless $ra untouched
                    invalid_or_switch = true;
                    break;
                }
            }

            const bool uncond_term = (inst.op == MipsOp::JR || inst.op == MipsOp::J || inst.op == MipsOp::BREAK);
            if (uncond_term) {
                if (inst.has_delay_slot()) {
                    if ((pc + 8u) > end_kseg0) {
                        invalid_or_switch = true;
                        break;
                    }
                    const DecodedInst ds = decode_mips(read_word_at(pc + 4u, base_kseg0, code_bytes));
                    if (ds.op == MipsOp::INVALID) {
                        invalid_or_switch = true;
                        break;
                    }
                }
                if (pc >= max_local_branch) {
                    chosen_end            = inst.has_delay_slot() ? (pc + 8u) : (pc + 4u);
                    ended_with_terminator = true;
                    break;
                }
                // Skip delay slot instruction so we don't double-process it
                if (inst.has_delay_slot() && !fn.local_labels.contains(pc + 4u)) {
                    pc += 4u;
                }
                continue;
            }

            if (inst.has_delay_slot() && (pc + 4u) < hard_cap && !fn.local_labels.contains(pc + 4u)) {
                const DecodedInst ds = decode_mips(read_word_at(pc + 4u, base_kseg0, code_bytes));
                if (ds.op == MipsOp::INVALID) {
                    invalid_or_switch = true;
                    break;
                }
                if (ds.op == MipsOp::ADDIU && ds.rt == reg::SP) {
                    modifies_ra_or_sp = true;
                }
                pc += 4u;
            }
        }

        if (invalid_or_switch || !ended_with_terminator || chosen_end <= fn.start_addr) {
            continue;
        }
        fn.end_addr = chosen_end;

        // Verify all conditional branches strictly stay inside [fn.start_addr, fn.end_addr)
        bool branches_sound = true;
        for (u32 lbl : fn.local_labels) {
            if (lbl < fn.start_addr || lbl >= fn.end_addr || ((lbl - fn.start_addr) & 3u) != 0u) {
                branches_sound = false;
                break;
            }
        }
        if (!branches_sound) {
            continue;
        }

        funcs.push_back(std::move(fn));
    }

    return funcs;
}

std::string lift_function_to_cpp(
    const DiscoveredFunction& fn,
    u32 base_kseg0,
    std::span<const u8> code_bytes,
    bool emit_comments) {
    base_kseg0 = to_kseg0(base_kseg0);

    // Pre-pass: determine the exact set of local labels actually targeted by emitted goto statements
    std::set<u32> candidate_labels;
    for (u32 lbl : fn.local_labels) {
        if (lbl >= fn.start_addr && lbl < fn.end_addr && ((lbl - fn.start_addr) & 3u) == 0u) {
            candidate_labels.insert(lbl);
        }
    }

    std::set<u32> emitted_pcs;
    std::set<u32> used_gotos;
    for (u32 pc = fn.start_addr; pc < fn.end_addr; pc += 4u) {
        emitted_pcs.insert(pc);
        const DecodedInst inst = decode_mips(read_word_at(pc, base_kseg0, code_bytes));
        if (!inst.has_delay_slot()) continue;

        if (inst.op == MipsOp::J) {
            const u32 k_tgt = to_kseg0(inst.jump_target(pc));
            if (candidate_labels.contains(k_tgt)) used_gotos.insert(k_tgt);
        } else if (inst.is_branch()) {
            const u32 k_tgt = to_kseg0(inst.branch_target(pc));
            if (candidate_labels.contains(k_tgt)) used_gotos.insert(k_tgt);
        }

        const u32 ds_pc = pc + 4u;
        if (!candidate_labels.contains(ds_pc)) {
            pc += 4u;
        }
    }

    std::set<u32> active_labels;
    for (u32 lbl : used_gotos) {
        if (emitted_pcs.contains(lbl)) {
            active_labels.insert(lbl);
        }
    }

    std::ostringstream os;
    os << "void " << fn_symbol(fn.start_addr) << "(CpuContext& ctx, PsxRuntime& rt) {\n";
    os << "    const bool entered_in_irq = rt.in_interrupt();\n";
    os << "    const u64 entry_lj = rt.longjmp_token();\n";
    os << "    (void)ctx; (void)rt; (void)entered_in_irq; (void)entry_lj;\n";

    // Delay slots that are also branch targets get emitted twice: once inside the branch block and
    // once in sequence (for the branch that targets them). The fall-through path of the first
    // branch must jump over the second copy, otherwise the instruction executes twice.
    std::set<u32> pending_ds_skips;

    u32 pc = fn.start_addr;
    for (; pc < fn.end_addr; pc += 4u) {
        if (active_labels.contains(pc)) {
            os << loc_symbol(pc) << ":\n";
        }

        const u32 word = read_word_at(pc, base_kseg0, code_bytes);
        const DecodedInst inst = decode_mips(word);

        if (emit_comments) {
            os << "    // " << hex32(pc) << ": " << format_mips(pc, inst) << "\n";
        }
        os << "    ctx.cycles += 3;\n";

        if (!inst.has_delay_slot()) {
            emit_non_branch_inst(os, pc, inst, "    ");
            if (pending_ds_skips.erase(pc) != 0) {
                os << ds_skip_symbol(pc) << ":;\n";
            }
            continue;
        }

        const u32 ds_pc = pc + 4u;
        const DecodedInst ds_inst = (ds_pc < base_kseg0 + code_bytes.size())
                                        ? decode_mips(read_word_at(ds_pc, base_kseg0, code_bytes))
                                        : decode_mips(0);

        os << "    {\n";
        if (emit_comments) {
            os << "        // [delay slot] " << hex32(ds_pc) << ": " << format_mips(ds_pc, ds_inst) << "\n";
        }
        os << "        ctx.cycles += 3;\n";

        auto emit_target_jump = [&](u32 target_addr, const char* ind) {
            const u32 k_tgt = to_kseg0(target_addr);
            if (k_tgt <= pc) {
                os << ind << "rt.poll_hardware(ctx);\n";
                os << ind << "if (rt.should_unwind(ctx, entered_in_irq, entry_lj)) {\n";
                os << ind << "    if (!ctx.halted && !entered_in_irq && rt.longjmp_token() == entry_lj) { ctx.pc = "
                   << hex32(k_tgt) << "; ctx.next_pc = " << hex32(k_tgt + 4u) << "; }\n";
                os << ind << "    return;\n";
                os << ind << "}\n";
            }
            if (active_labels.contains(k_tgt)) {
                os << ind << "goto " << loc_symbol(k_tgt) << ";\n";
            } else if (k_tgt == fn.start_addr) {
                os << ind << fn_symbol(fn.start_addr) << "(ctx, rt); return;\n";
            } else {
                os << ind << "rt.call_function(ctx, " << hex32(target_addr) << "); return;\n";
            }
        };

        if (inst.op == MipsOp::JR) {
            os << "        const u32 jmp_tgt = ctx.get_reg(" << int(inst.rs) << ");\n";
            emit_non_branch_inst(os, ds_pc, ds_inst, "        ");
            if (inst.rs == reg::RA) {
                os << "        ctx.pc = jmp_tgt; ctx.next_pc = jmp_tgt + 4u; return;\n";
            } else {
                os << "        ctx.pc = jmp_tgt; ctx.next_pc = jmp_tgt + 4u;\n";
                os << "        rt.call_function(ctx, jmp_tgt); return;\n";
            }
        } else if (inst.op == MipsOp::JALR) {
            os << "        const u32 call_tgt = ctx.get_reg(" << int(inst.rs) << ");\n";
            os << "        ctx.set_reg(" << int(inst.rd) << ", " << hex32(pc + 8u) << ");\n";
            emit_non_branch_inst(os, ds_pc, ds_inst, "        ");
            os << "        rt.poll_hardware(ctx);\n";
            os << "        rt.call_function(ctx, call_tgt);\n";
            os << "        if (rt.should_unwind(ctx, entered_in_irq, entry_lj)) return;\n";
        } else if (inst.op == MipsOp::J) {
            const u32 tgt = inst.jump_target(pc);
            emit_non_branch_inst(os, ds_pc, ds_inst, "        ");
            emit_target_jump(tgt, "        ");
        } else if (inst.op == MipsOp::JAL) {
            const u32 tgt = inst.jump_target(pc);
            os << "        ctx.set_reg(" << int(reg::RA) << ", " << hex32(pc + 8u) << ");\n";
            emit_non_branch_inst(os, ds_pc, ds_inst, "        ");
            os << "        rt.poll_hardware(ctx);\n";
            os << "        rt.call_function(ctx, " << hex32(tgt) << ");\n";
            os << "        if (rt.should_unwind(ctx, entered_in_irq, entry_lj)) return;\n";
        } else if (inst.is_branch()) {
            os << "        const bool cond = ";
            switch (inst.op) {
                case MipsOp::BEQ:
                    os << "(ctx.get_reg(" << int(inst.rs) << ") == ctx.get_reg(" << int(inst.rt) << "));\n";
                    break;
                case MipsOp::BNE:
                    os << "(ctx.get_reg(" << int(inst.rs) << ") != ctx.get_reg(" << int(inst.rt) << "));\n";
                    break;
                case MipsOp::BLEZ:
                    os << "(static_cast<s32>(ctx.get_reg(" << int(inst.rs) << ")) <= 0);\n";
                    break;
                case MipsOp::BGTZ:
                    os << "(static_cast<s32>(ctx.get_reg(" << int(inst.rs) << ")) > 0);\n";
                    break;
                case MipsOp::BLTZ:
                case MipsOp::BLTZAL:
                    os << "(static_cast<s32>(ctx.get_reg(" << int(inst.rs) << ")) < 0);\n";
                    break;
                case MipsOp::BGEZ:
                case MipsOp::BGEZAL:
                default:
                    os << "(static_cast<s32>(ctx.get_reg(" << int(inst.rs) << ")) >= 0);\n";
                    break;
            }
            if (inst.op == MipsOp::BLTZAL || inst.op == MipsOp::BGEZAL) {
                os << "        ctx.set_reg(" << int(reg::RA) << ", " << hex32(pc + 8u) << ");\n";
            }
            emit_non_branch_inst(os, ds_pc, ds_inst, "        ");
            os << "        if (cond) {\n";
            const u32 b_tgt = inst.branch_target(pc);
            if ((inst.op == MipsOp::BLTZAL || inst.op == MipsOp::BGEZAL) && !active_labels.contains(to_kseg0(b_tgt))) {
                os << "            rt.poll_hardware(ctx);\n";
                os << "            rt.call_function(ctx, " << hex32(b_tgt) << ");\n";
                os << "            if (rt.should_unwind(ctx, entered_in_irq, entry_lj)) return;\n";
            } else {
                emit_target_jump(b_tgt, "            ");
            }
            os << "        }\n";
        }

        // Paths that fall out of the block (branch not taken, or return from a call) continue at
        // pc + 8. When the delay slot is re-emitted below as a label target, skip that copy.
        const bool falls_through = inst.is_branch() || inst.op == MipsOp::JAL || inst.op == MipsOp::JALR;
        if (falls_through && candidate_labels.contains(ds_pc) && ds_pc < fn.end_addr &&
            !ds_inst.has_delay_slot()) {
            os << "        goto " << ds_skip_symbol(ds_pc) << ";\n";
            pending_ds_skips.insert(ds_pc);
        }

        os << "    }\n";

        if (!candidate_labels.contains(ds_pc)) {
            pc += 4u;
        }
    }

    // Fallthrough into next contiguous routine (e.g. multi-entry PsyQ assembly functions)
    os << "    ctx.pc = " << hex32(pc) << ";\n";
    os << "    ctx.next_pc = " << hex32(pc + 4u) << ";\n";
    os << "    rt.call_function(ctx, " << hex32(pc) << ");\n";
    os << "}\n";
    return os.str();
}

std::string emit_recompiled_module_cpp(
    std::span<const DiscoveredFunction> funcs,
    u32 base_kseg0,
    std::span<const u8> code_bytes,
    const LifterOptions& opts) {
    base_kseg0 = to_kseg0(base_kseg0);
    std::ostringstream os;
    os << "// Auto-generated by vs_recomp (Vagrant Story Native Port Toolchain)\n";
    os << "#include \"vsport/runtime.hpp\"\n\n";
    os << "#ifdef _MSC_VER\n";
    os << "#pragma warning(push)\n";
    os << "#pragma warning(disable : 4702) // Unreachable code after MIPS unconditional jump/return\n";
    os << "#endif\n\n";
    os << "namespace vsport::generated_" << opts.module_id << " {\n\n";

    const std::size_t limit = (opts.max_functions > 0 && opts.max_functions < funcs.size())
                                  ? opts.max_functions
                                  : funcs.size();

    // Forward declarations
    for (std::size_t i = 0; i < limit; ++i) {
        os << "void " << fn_symbol(funcs[i].start_addr) << "(CpuContext& ctx, PsxRuntime& rt);\n";
    }
    os << "\n";

    // Function definitions
    for (std::size_t i = 0; i < limit; ++i) {
        os << lift_function_to_cpp(funcs[i], base_kseg0, code_bytes, opts.emit_comments) << "\n";
    }

    // Overlay-aware 64-bit FNV-1a signature registration
    os << "void register_all(PsxRuntime& rt) {\n";
    for (std::size_t i = 0; i < limit; ++i) {
        const u32 start_off = funcs[i].start_addr - base_kseg0;
        const u32 byte_len  = funcs[i].end_addr - funcs[i].start_addr;
        if (start_off + byte_len <= code_bytes.size() && byte_len > 0) {
            const u64 sig = fnv1a_64(&code_bytes[start_off], byte_len);
            char sig_hex[32]{};
            std::snprintf(sig_hex, sizeof(sig_hex), "0x%016llXull", static_cast<unsigned long long>(sig));
            os << "    rt.register_func(" << hex32(funcs[i].start_addr) << ", "
               << byte_len << "u, " << sig_hex << ", &" << fn_symbol(funcs[i].start_addr) << ");\n";
        }
    }
    os << "}\n\n";
    os << "} // namespace vsport::generated_" << opts.module_id << "\n\n";
    os << "#ifdef _MSC_VER\n";
    os << "#pragma warning(pop)\n";
    os << "#endif\n";
    return os.str();
}

} // namespace vsport
