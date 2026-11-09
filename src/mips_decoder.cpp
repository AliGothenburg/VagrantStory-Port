#include "vsport/mips_decoder.hpp"

#include <cstdio>

namespace vsport {

const char* gte_func_name(u8 fn) noexcept {
    switch (fn) {
        case gte_fn::RTPS:  return "RTPS";
        case gte_fn::NCLIP: return "NCLIP";
        case gte_fn::OP:    return "OP";
        case gte_fn::DPCS:  return "DPCS";
        case gte_fn::INTPL: return "INTPL";
        case gte_fn::MVMVA: return "MVMVA";
        case gte_fn::NCDS:  return "NCDS";
        case gte_fn::CDP:   return "CDP";
        case gte_fn::NCDT:  return "NCDT";
        case gte_fn::NCCS:  return "NCCS";
        case gte_fn::CC:    return "CC";
        case gte_fn::NCS:   return "NCS";
        case gte_fn::NCT:   return "NCT";
        case gte_fn::SQR:   return "SQR";
        case gte_fn::DCPL:  return "DCPL";
        case gte_fn::DPCT:  return "DPCT";
        case gte_fn::AVSZ3: return "AVSZ3";
        case gte_fn::AVSZ4: return "AVSZ4";
        case gte_fn::RTPT:  return "RTPT";
        case gte_fn::GPF:   return "GPF";
        case gte_fn::GPL:   return "GPL";
        case gte_fn::NCCT:  return "NCCT";
        default:            return "GTE_UNK";
    }
}

DecodedInst decode_mips(u32 word) noexcept {
    DecodedInst d{};
    d.raw   = word;
    d.rs    = static_cast<u8>((word >> 21) & 0x1Fu);
    d.rt    = static_cast<u8>((word >> 16) & 0x1Fu);
    d.rd    = static_cast<u8>((word >> 11) & 0x1Fu);
    d.shamt = static_cast<u8>((word >> 6)  & 0x1Fu);
    d.funct = static_cast<u8>(word & 0x3Fu);
    d.imm16 = static_cast<u16>(word & 0xFFFFu);
    d.imm26 = word & 0x03FFFFFFu;

    if (word == 0) {
        d.op = MipsOp::NOP;
        return d;
    }

    const u8 primary = static_cast<u8>((word >> 26) & 0x3Fu);
    switch (primary) {
        case 0x00: { // SPECIAL
            switch (d.funct) {
                case 0x00: d.op = MipsOp::SLL; break;
                case 0x02: d.op = MipsOp::SRL; break;
                case 0x03: d.op = MipsOp::SRA; break;
                case 0x04: d.op = MipsOp::SLLV; break;
                case 0x06: d.op = MipsOp::SRLV; break;
                case 0x07: d.op = MipsOp::SRAV; break;
                case 0x08: d.op = MipsOp::JR; break;
                case 0x09: d.op = MipsOp::JALR; break;
                case 0x0C: d.op = MipsOp::SYSCALL; break;
                case 0x0D: d.op = MipsOp::BREAK; break;
                case 0x10: d.op = MipsOp::MFHI; break;
                case 0x11: d.op = MipsOp::MTHI; break;
                case 0x12: d.op = MipsOp::MFLO; break;
                case 0x13: d.op = MipsOp::MTLO; break;
                case 0x18: d.op = MipsOp::MULT; break;
                case 0x19: d.op = MipsOp::MULTU; break;
                case 0x1A: d.op = MipsOp::DIV; break;
                case 0x1B: d.op = MipsOp::DIVU; break;
                case 0x20: d.op = MipsOp::ADD; break;
                case 0x21: d.op = MipsOp::ADDU; break;
                case 0x22: d.op = MipsOp::SUB; break;
                case 0x23: d.op = MipsOp::SUBU; break;
                case 0x24: d.op = MipsOp::AND; break;
                case 0x25: d.op = MipsOp::OR; break;
                case 0x26: d.op = MipsOp::XOR; break;
                case 0x27: d.op = MipsOp::NOR; break;
                case 0x2A: d.op = MipsOp::SLT; break;
                case 0x2B: d.op = MipsOp::SLTU; break;
                default:   d.op = MipsOp::INVALID; break;
            }
            break;
        }
        case 0x01: { // REGIMM (BcondZ)
            // Note: PSX R3000A decodes bit 16 for BLTZ(0) vs BGEZ(1), and bits 20:17 == 0b1000 for link!
            const bool is_bgez = (d.rt & 0x01u) != 0;
            const bool is_link = (d.rt & 0x1Eu) == 0x10u;
            if (is_link) {
                d.op = is_bgez ? MipsOp::BGEZAL : MipsOp::BLTZAL;
            } else {
                d.op = is_bgez ? MipsOp::BGEZ : MipsOp::BLTZ;
            }
            break;
        }
        case 0x02: d.op = MipsOp::J; break;
        case 0x03: d.op = MipsOp::JAL; break;
        case 0x04: d.op = MipsOp::BEQ; break;
        case 0x05: d.op = MipsOp::BNE; break;
        case 0x06: d.op = MipsOp::BLEZ; break;
        case 0x07: d.op = MipsOp::BGTZ; break;
        case 0x08: d.op = MipsOp::ADDI; break;
        case 0x09: d.op = MipsOp::ADDIU; break;
        case 0x0A: d.op = MipsOp::SLTI; break;
        case 0x0B: d.op = MipsOp::SLTIU; break;
        case 0x0C: d.op = MipsOp::ANDI; break;
        case 0x0D: d.op = MipsOp::ORI; break;
        case 0x0E: d.op = MipsOp::XORI; break;
        case 0x0F: d.op = MipsOp::LUI; break;
        case 0x10: { // COP0
            if ((d.rs & 0x10u) != 0) {
                if (d.funct == 0x10) d.op = MipsOp::RFE;
            } else {
                switch (d.rs) {
                    case 0x00: d.op = MipsOp::MFC0; break;
                    case 0x04: d.op = MipsOp::MTC0; break;
                    default:   d.op = MipsOp::INVALID; break;
                }
            }
            break;
        }
        case 0x12: { // COP2 (PlayStation GTE)
            if ((d.rs & 0x10u) != 0) {
                d.op       = MipsOp::GTE_CMD;
                d.gte_func = static_cast<u8>(word & 0x3Fu);
                d.gte_sf   = ((word >> 19) & 1u) != 0;
                d.gte_mx   = static_cast<u8>((word >> 17) & 0x03u);
                d.gte_v    = static_cast<u8>((word >> 15) & 0x03u);
                d.gte_cv   = static_cast<u8>((word >> 13) & 0x03u);
                d.gte_lm   = ((word >> 10) & 1u) != 0;
            } else {
                switch (d.rs) {
                    case 0x00: d.op = MipsOp::MFC2; break;
                    case 0x02: d.op = MipsOp::CFC2; break;
                    case 0x04: d.op = MipsOp::MTC2; break;
                    case 0x06: d.op = MipsOp::CTC2; break;
                    default:   d.op = MipsOp::INVALID; break;
                }
            }
            break;
        }
        case 0x20: d.op = MipsOp::LB; break;
        case 0x21: d.op = MipsOp::LH; break;
        case 0x22: d.op = MipsOp::LWL; break;
        case 0x23: d.op = MipsOp::LW; break;
        case 0x24: d.op = MipsOp::LBU; break;
        case 0x25: d.op = MipsOp::LHU; break;
        case 0x26: d.op = MipsOp::LWR; break;
        case 0x28: d.op = MipsOp::SB; break;
        case 0x29: d.op = MipsOp::SH; break;
        case 0x2A: d.op = MipsOp::SWL; break;
        case 0x2B: d.op = MipsOp::SW; break;
        case 0x2E: d.op = MipsOp::SWR; break;
        case 0x32: d.op = MipsOp::LWC2; break;
        case 0x3A: d.op = MipsOp::SWC2; break;
        default:   d.op = MipsOp::INVALID; break;
    }

    return d;
}

std::string format_mips(u32 pc, const DecodedInst& i) {
    char buf[128]{};
    const auto rn = [](u8 r) { return gpr_name(r).data(); };

    switch (i.op) {
        case MipsOp::NOP:     std::snprintf(buf, sizeof(buf), "nop"); break;
        case MipsOp::SLL:     std::snprintf(buf, sizeof(buf), "sll     %s, %s, %u", rn(i.rd), rn(i.rt), i.shamt); break;
        case MipsOp::SRL:     std::snprintf(buf, sizeof(buf), "srl     %s, %s, %u", rn(i.rd), rn(i.rt), i.shamt); break;
        case MipsOp::SRA:     std::snprintf(buf, sizeof(buf), "sra     %s, %s, %u", rn(i.rd), rn(i.rt), i.shamt); break;
        case MipsOp::SLLV:    std::snprintf(buf, sizeof(buf), "sllv    %s, %s, %s", rn(i.rd), rn(i.rt), rn(i.rs)); break;
        case MipsOp::SRLV:    std::snprintf(buf, sizeof(buf), "srlv    %s, %s, %s", rn(i.rd), rn(i.rt), rn(i.rs)); break;
        case MipsOp::SRAV:    std::snprintf(buf, sizeof(buf), "srav    %s, %s, %s", rn(i.rd), rn(i.rt), rn(i.rs)); break;
        case MipsOp::JR:      std::snprintf(buf, sizeof(buf), "jr      %s", rn(i.rs)); break;
        case MipsOp::JALR:    std::snprintf(buf, sizeof(buf), "jalr    %s, %s", rn(i.rd), rn(i.rs)); break;
        case MipsOp::J:       std::snprintf(buf, sizeof(buf), "j       0x%08X", i.jump_target(pc)); break;
        case MipsOp::JAL:     std::snprintf(buf, sizeof(buf), "jal     0x%08X", i.jump_target(pc)); break;
        case MipsOp::BEQ:     std::snprintf(buf, sizeof(buf), "beq     %s, %s, 0x%08X", rn(i.rs), rn(i.rt), i.branch_target(pc)); break;
        case MipsOp::BNE:     std::snprintf(buf, sizeof(buf), "bne     %s, %s, 0x%08X", rn(i.rs), rn(i.rt), i.branch_target(pc)); break;
        case MipsOp::BLEZ:    std::snprintf(buf, sizeof(buf), "blez    %s, 0x%08X", rn(i.rs), i.branch_target(pc)); break;
        case MipsOp::BGTZ:    std::snprintf(buf, sizeof(buf), "bgtz    %s, 0x%08X", rn(i.rs), i.branch_target(pc)); break;
        case MipsOp::BLTZ:    std::snprintf(buf, sizeof(buf), "bltz    %s, 0x%08X", rn(i.rs), i.branch_target(pc)); break;
        case MipsOp::BGEZ:    std::snprintf(buf, sizeof(buf), "bgez    %s, 0x%08X", rn(i.rs), i.branch_target(pc)); break;
        case MipsOp::BLTZAL:  std::snprintf(buf, sizeof(buf), "bltzal  %s, 0x%08X", rn(i.rs), i.branch_target(pc)); break;
        case MipsOp::BGEZAL:  std::snprintf(buf, sizeof(buf), "bgezal  %s, 0x%08X", rn(i.rs), i.branch_target(pc)); break;
        case MipsOp::SYSCALL: std::snprintf(buf, sizeof(buf), "syscall"); break;
        case MipsOp::BREAK:   std::snprintf(buf, sizeof(buf), "break"); break;
        case MipsOp::MFHI:    std::snprintf(buf, sizeof(buf), "mfhi    %s", rn(i.rd)); break;
        case MipsOp::MTHI:    std::snprintf(buf, sizeof(buf), "mthi    %s", rn(i.rs)); break;
        case MipsOp::MFLO:    std::snprintf(buf, sizeof(buf), "mflo    %s", rn(i.rd)); break;
        case MipsOp::MTLO:    std::snprintf(buf, sizeof(buf), "mtlo    %s", rn(i.rs)); break;
        case MipsOp::MULT:    std::snprintf(buf, sizeof(buf), "mult    %s, %s", rn(i.rs), rn(i.rt)); break;
        case MipsOp::MULTU:   std::snprintf(buf, sizeof(buf), "multu   %s, %s", rn(i.rs), rn(i.rt)); break;
        case MipsOp::DIV:     std::snprintf(buf, sizeof(buf), "div     %s, %s", rn(i.rs), rn(i.rt)); break;
        case MipsOp::DIVU:    std::snprintf(buf, sizeof(buf), "divu    %s, %s", rn(i.rs), rn(i.rt)); break;
        case MipsOp::ADD:     std::snprintf(buf, sizeof(buf), "add     %s, %s, %s", rn(i.rd), rn(i.rs), rn(i.rt)); break;
        case MipsOp::ADDU:    std::snprintf(buf, sizeof(buf), "addu    %s, %s, %s", rn(i.rd), rn(i.rs), rn(i.rt)); break;
        case MipsOp::SUB:     std::snprintf(buf, sizeof(buf), "sub     %s, %s, %s", rn(i.rd), rn(i.rs), rn(i.rt)); break;
        case MipsOp::SUBU:    std::snprintf(buf, sizeof(buf), "subu    %s, %s, %s", rn(i.rd), rn(i.rs), rn(i.rt)); break;
        case MipsOp::AND:     std::snprintf(buf, sizeof(buf), "and     %s, %s, %s", rn(i.rd), rn(i.rs), rn(i.rt)); break;
        case MipsOp::OR:      std::snprintf(buf, sizeof(buf), "or      %s, %s, %s", rn(i.rd), rn(i.rs), rn(i.rt)); break;
        case MipsOp::XOR:     std::snprintf(buf, sizeof(buf), "xor     %s, %s, %s", rn(i.rd), rn(i.rs), rn(i.rt)); break;
        case MipsOp::NOR:     std::snprintf(buf, sizeof(buf), "nor     %s, %s, %s", rn(i.rd), rn(i.rs), rn(i.rt)); break;
        case MipsOp::SLT:     std::snprintf(buf, sizeof(buf), "slt     %s, %s, %s", rn(i.rd), rn(i.rs), rn(i.rt)); break;
        case MipsOp::SLTU:    std::snprintf(buf, sizeof(buf), "sltu    %s, %s, %s", rn(i.rd), rn(i.rs), rn(i.rt)); break;
        case MipsOp::ADDI:    std::snprintf(buf, sizeof(buf), "addi    %s, %s, %d", rn(i.rt), rn(i.rs), i.simm16()); break;
        case MipsOp::ADDIU:   std::snprintf(buf, sizeof(buf), "addiu   %s, %s, %d", rn(i.rt), rn(i.rs), i.simm16()); break;
        case MipsOp::SLTI:    std::snprintf(buf, sizeof(buf), "slti    %s, %s, %d", rn(i.rt), rn(i.rs), i.simm16()); break;
        case MipsOp::SLTIU:   std::snprintf(buf, sizeof(buf), "sltiu   %s, %s, %d", rn(i.rt), rn(i.rs), i.simm16()); break;
        case MipsOp::ANDI:    std::snprintf(buf, sizeof(buf), "andi    %s, %s, 0x%04X", rn(i.rt), rn(i.rs), i.uimm16()); break;
        case MipsOp::ORI:     std::snprintf(buf, sizeof(buf), "ori     %s, %s, 0x%04X", rn(i.rt), rn(i.rs), i.uimm16()); break;
        case MipsOp::XORI:    std::snprintf(buf, sizeof(buf), "xori    %s, %s, 0x%04X", rn(i.rt), rn(i.rs), i.uimm16()); break;
        case MipsOp::LUI:     std::snprintf(buf, sizeof(buf), "lui     %s, 0x%04X", rn(i.rt), i.uimm16()); break;
        case MipsOp::LB:      std::snprintf(buf, sizeof(buf), "lb      %s, %d(%s)", rn(i.rt), i.simm16(), rn(i.rs)); break;
        case MipsOp::LH:      std::snprintf(buf, sizeof(buf), "lh      %s, %d(%s)", rn(i.rt), i.simm16(), rn(i.rs)); break;
        case MipsOp::LWL:     std::snprintf(buf, sizeof(buf), "lwl     %s, %d(%s)", rn(i.rt), i.simm16(), rn(i.rs)); break;
        case MipsOp::LW:      std::snprintf(buf, sizeof(buf), "lw      %s, %d(%s)", rn(i.rt), i.simm16(), rn(i.rs)); break;
        case MipsOp::LBU:     std::snprintf(buf, sizeof(buf), "lbu     %s, %d(%s)", rn(i.rt), i.simm16(), rn(i.rs)); break;
        case MipsOp::LHU:     std::snprintf(buf, sizeof(buf), "lhu     %s, %d(%s)", rn(i.rt), i.simm16(), rn(i.rs)); break;
        case MipsOp::LWR:     std::snprintf(buf, sizeof(buf), "lwr     %s, %d(%s)", rn(i.rt), i.simm16(), rn(i.rs)); break;
        case MipsOp::SB:      std::snprintf(buf, sizeof(buf), "sb      %s, %d(%s)", rn(i.rt), i.simm16(), rn(i.rs)); break;
        case MipsOp::SH:      std::snprintf(buf, sizeof(buf), "sh      %s, %d(%s)", rn(i.rt), i.simm16(), rn(i.rs)); break;
        case MipsOp::SWL:     std::snprintf(buf, sizeof(buf), "swl     %s, %d(%s)", rn(i.rt), i.simm16(), rn(i.rs)); break;
        case MipsOp::SW:      std::snprintf(buf, sizeof(buf), "sw      %s, %d(%s)", rn(i.rt), i.simm16(), rn(i.rs)); break;
        case MipsOp::SWR:     std::snprintf(buf, sizeof(buf), "swr     %s, %d(%s)", rn(i.rt), i.simm16(), rn(i.rs)); break;
        case MipsOp::MFC0:    std::snprintf(buf, sizeof(buf), "mfc0    %s, $cop0_%u", rn(i.rt), i.rd); break;
        case MipsOp::MTC0:    std::snprintf(buf, sizeof(buf), "mtc0    %s, $cop0_%u", rn(i.rt), i.rd); break;
        case MipsOp::RFE:     std::snprintf(buf, sizeof(buf), "rfe"); break;
        case MipsOp::MFC2:    std::snprintf(buf, sizeof(buf), "mfc2    %s, $gte_d%u", rn(i.rt), i.rd); break;
        case MipsOp::CFC2:    std::snprintf(buf, sizeof(buf), "cfc2    %s, $gte_c%u", rn(i.rt), i.rd); break;
        case MipsOp::MTC2:    std::snprintf(buf, sizeof(buf), "mtc2    %s, $gte_d%u", rn(i.rt), i.rd); break;
        case MipsOp::CTC2:    std::snprintf(buf, sizeof(buf), "ctc2    %s, $gte_c%u", rn(i.rt), i.rd); break;
        case MipsOp::LWC2:    std::snprintf(buf, sizeof(buf), "lwc2    $gte_d%u, %d(%s)", i.rt, i.simm16(), rn(i.rs)); break;
        case MipsOp::SWC2:    std::snprintf(buf, sizeof(buf), "swc2    $gte_d%u, %d(%s)", i.rt, i.simm16(), rn(i.rs)); break;
        case MipsOp::GTE_CMD:
            std::snprintf(buf, sizeof(buf), "gte.%s (sf=%u, lm=%u)",
                          gte_func_name(i.gte_func), i.gte_sf ? 1u : 0u, i.gte_lm ? 1u : 0u);
            break;
        case MipsOp::INVALID:
        default:
            std::snprintf(buf, sizeof(buf), ".word   0x%08X", i.raw);
            break;
    }
    return std::string(buf);
}

} // namespace vsport
