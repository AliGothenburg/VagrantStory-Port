#pragma once

#include "vsport/types.hpp"

#include <string>

namespace vsport {

enum class MipsOp : u8 {
    INVALID = 0,
    NOP,
    // Shifts
    SLL, SRL, SRA, SLLV, SRLV, SRAV,
    // Jumps & Branches
    JR, JALR, J, JAL,
    BEQ, BNE, BLEZ, BGTZ,
    BLTZ, BGEZ, BLTZAL, BGEZAL,
    // Exceptions
    SYSCALL, BREAK,
    // Multiply / Divide
    MFHI, MTHI, MFLO, MTLO,
    MULT, MULTU, DIV, DIVU,
    // ALU Register
    ADD, ADDU, SUB, SUBU,
    AND, OR, XOR, NOR,
    SLT, SLTU,
    // ALU Immediate
    ADDI, ADDIU, SLTI, SLTIU,
    ANDI, ORI, XORI, LUI,
    // Loads & Stores
    LB, LH, LWL, LW, LBU, LHU, LWR,
    SB, SH, SWL, SW, SWR,
    // Coprocessor 0 (System Control)
    MFC0, MTC0, RFE,
    // Coprocessor 2 (PlayStation GTE)
    MFC2, CFC2, MTC2, CTC2,
    LWC2, SWC2, GTE_CMD
};

// PlayStation COP2 GTE 6-bit function codes (bits 0..5 of COP2 command)
namespace gte_fn {
    constexpr u8 RTPS  = 0x01;
    constexpr u8 NCLIP = 0x06;
    constexpr u8 OP    = 0x0C;
    constexpr u8 DPCS  = 0x10;
    constexpr u8 INTPL = 0x11;
    constexpr u8 MVMVA = 0x12;
    constexpr u8 NCDS  = 0x13;
    constexpr u8 CDP   = 0x14;
    constexpr u8 NCDT  = 0x16;
    constexpr u8 NCCS  = 0x1B;
    constexpr u8 CC    = 0x1C;
    constexpr u8 NCS   = 0x1E;
    constexpr u8 NCT   = 0x20;
    constexpr u8 SQR   = 0x28;
    constexpr u8 DCPL  = 0x29;
    constexpr u8 DPCT  = 0x2A;
    constexpr u8 AVSZ3 = 0x2D;
    constexpr u8 AVSZ4 = 0x2E;
    constexpr u8 RTPT  = 0x30;
    constexpr u8 GPF   = 0x3D;
    constexpr u8 GPL   = 0x3E;
    constexpr u8 NCCT  = 0x3F;
} // namespace gte_fn

struct DecodedInst {
    u32 raw   = 0;
    MipsOp op = MipsOp::INVALID;

    u8 rs     = 0;
    u8 rt     = 0;
    u8 rd     = 0;
    u8 shamt  = 0;
    u8 funct  = 0;
    u16 imm16 = 0;
    u32 imm26 = 0;

    // GTE command fields (valid when op == MipsOp::GTE_CMD)
    u8 gte_func = 0;
    bool gte_sf = false; // bit 19: shift fraction by 12 bits
    u8 gte_mx   = 0;     // bits 17..18: matrix selector
    u8 gte_v    = 0;     // bits 15..16: vector selector
    u8 gte_cv   = 0;     // bits 13..14: translation/color vector selector
    bool gte_lm = false; // bit 10: clamp negative results to 0

    [[nodiscard]] constexpr s32 simm16() const noexcept {
        return static_cast<s32>(static_cast<s16>(imm16));
    }

    [[nodiscard]] constexpr u32 uimm16() const noexcept {
        return static_cast<u32>(imm16);
    }

    [[nodiscard]] constexpr u32 branch_target(u32 pc) const noexcept {
        return pc + 4u + static_cast<u32>(simm16() << 2);
    }

    [[nodiscard]] constexpr u32 jump_target(u32 pc) const noexcept {
        return ((pc + 4u) & 0xF0000000u) | (imm26 << 2);
    }

    [[nodiscard]] constexpr bool is_branch() const noexcept {
        return op == MipsOp::BEQ || op == MipsOp::BNE ||
               op == MipsOp::BLEZ || op == MipsOp::BGTZ ||
               op == MipsOp::BLTZ || op == MipsOp::BGEZ ||
               op == MipsOp::BLTZAL || op == MipsOp::BGEZAL;
    }

    [[nodiscard]] constexpr bool is_jump() const noexcept {
        return op == MipsOp::J || op == MipsOp::JAL ||
               op == MipsOp::JR || op == MipsOp::JALR;
    }

    [[nodiscard]] constexpr bool has_delay_slot() const noexcept {
        return is_branch() || is_jump();
    }

    [[nodiscard]] constexpr bool is_call() const noexcept {
        return op == MipsOp::JAL || op == MipsOp::JALR ||
               op == MipsOp::BLTZAL || op == MipsOp::BGEZAL;
    }

    [[nodiscard]] constexpr bool is_return() const noexcept {
        return op == MipsOp::JR && rs == reg::RA;
    }
};

[[nodiscard]] DecodedInst decode_mips(u32 word) noexcept;
[[nodiscard]] std::string format_mips(u32 pc, const DecodedInst& inst);
[[nodiscard]] const char* gte_func_name(u8 gte_func) noexcept;

} // namespace vsport
