#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace vsport {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using s8  = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// PS1 Memory Constants
constexpr u32 PSX_RAM_SIZE_2MB      = 2 * 1024 * 1024;   // Retail PS1 2MB RAM
constexpr u32 PSX_RAM_SIZE_8MB      = 8 * 1024 * 1024;   // Devkit / Native Port Expanded RAM
constexpr u32 PSX_SCRATCHPAD_SIZE   = 1024;              // 1KB Fast D-Cache Scratchpad (0x1F800000)
constexpr u32 PSX_BIOS_SIZE         = 512 * 1024;        // 512KB BIOS ROM (0x1FC00000)

constexpr u32 PSX_SCRATCHPAD_PHYS   = 0x1F800000u;
constexpr u32 PSX_MMIO_PHYS_START   = 0x1F801000u;
constexpr u32 PSX_MMIO_PHYS_END     = 0x1F802FFFu;
constexpr u32 PSX_BIOS_PHYS_START   = 0x1FC00000u;

// Region mask table for MIPS R3000A KUSEG / KSEG0 / KSEG1 / KSEG2
constexpr std::array<u32, 8> REGION_MASK = {
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, // KUSEG (0x00000000 - 0x7FFFFFFF)
    0x7FFFFFFFu,                                        // KSEG0 (0x80000000 - 0x9FFFFFFF) -> cached
    0x1FFFFFFFu,                                        // KSEG1 (0xA0000000 - 0xBFFFFFFF) -> uncached
    0xFFFFFFFFu, 0xFFFFFFFFu                            // KSEG2 (0xC0000000 - 0xFFFFFFFF)
};

[[nodiscard]] constexpr u32 mask_psx_addr(u32 vaddr) noexcept {
    return vaddr & REGION_MASK[vaddr >> 29];
}

[[nodiscard]] constexpr u32 to_kseg0(u32 addr) noexcept {
    return 0x80000000u | (mask_psx_addr(addr) & 0x1FFFFFFFu);
}

[[nodiscard]] constexpr bool is_ram_phys(u32 paddr, u32 ram_size = PSX_RAM_SIZE_2MB) noexcept {
    return paddr < ram_size;
}

// MIPS R3000A General Purpose Register Indices
namespace reg {
    constexpr u32 ZERO = 0;
    constexpr u32 AT   = 1;
    constexpr u32 V0   = 2;
    constexpr u32 V1   = 3;
    constexpr u32 A0   = 4;
    constexpr u32 A1   = 5;
    constexpr u32 A2   = 6;
    constexpr u32 A3   = 7;
    constexpr u32 T0   = 8;
    constexpr u32 T1   = 9;
    constexpr u32 T2   = 10;
    constexpr u32 T3   = 11;
    constexpr u32 T4   = 12;
    constexpr u32 T5   = 13;
    constexpr u32 T6   = 14;
    constexpr u32 T7   = 15;
    constexpr u32 S0   = 16;
    constexpr u32 S1   = 17;
    constexpr u32 S2   = 18;
    constexpr u32 S3   = 19;
    constexpr u32 S4   = 20;
    constexpr u32 S5   = 21;
    constexpr u32 S6   = 22;
    constexpr u32 S7   = 23;
    constexpr u32 T8   = 24;
    constexpr u32 T9   = 25;
    constexpr u32 K0   = 26;
    constexpr u32 K1   = 27;
    constexpr u32 GP   = 28;
    constexpr u32 SP   = 29;
    constexpr u32 FP   = 30;
    constexpr u32 RA   = 31;
} // namespace reg

[[nodiscard]] constexpr std::string_view gpr_name(u32 index) noexcept {
    constexpr std::array<std::string_view, 32> NAMES = {
        "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3",
        "$t0",   "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7",
        "$s0",   "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
        "$t8",   "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra"
    };
    return (index < 32) ? NAMES[index] : "$invalid";
}

struct CpuContext {
    std::array<u32, 32> r{};
    u32 pc      = 0xBFC00000u;
    u32 next_pc = 0xBFC00004u;
    u32 hi      = 0;
    u32 lo      = 0;

    std::array<u32, 32> cop0{};
    u64 cycles  = 0;
    bool halted = false;

    constexpr void set_reg(u32 idx, u32 val) noexcept {
        if (idx != 0 && idx < 32) {
            r[idx] = val;
        }
        r[0] = 0;
    }

    [[nodiscard]] constexpr u32 get_reg(u32 idx) const noexcept {
        return (idx == 0 || idx >= 32) ? 0u : r[idx];
    }
};

[[nodiscard]] constexpr u64 fnv1a_64(const u8* data, std::size_t len) noexcept {
    u64 h = 14695981039346656037ull;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<u64>(data[i]);
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace vsport
