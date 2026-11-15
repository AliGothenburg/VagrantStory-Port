#include "vsport/cdrom_iso.hpp"
#include "vsport/cfg_lifter.hpp"
#include "vsport/gte.hpp"
#include "vsport/memcard.hpp"
#include "vsport/mips_decoder.hpp"
#include "vsport/pad_mapping.hpp"
#include "vsport/psx_exe.hpp"
#include "vsport/psx_gpu.hpp"
#include "vsport/runtime.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace vsport;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                                   \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " -> " << msg << "\n"; \
            ++g_failures;                                                                  \
        }                                                                                  \
    } while (0)

void test_address_mapping_and_scratchpad() {
    CHECK(mask_psx_addr(0x00010000u) == 0x00010000u, "KUSEG mask");
    CHECK(mask_psx_addr(0x80068800u) == 0x00068800u, "KSEG0 mask for BATTLE.PRG");
    CHECK(mask_psx_addr(0xA0068800u) == 0x00068800u, "KSEG1 mask for BATTLE.PRG");
    CHECK(to_kseg0(0x00068800u) == 0x80068800u, "to_kseg0 canonicalization");

    PsxRuntime rt;
    // Write via KSEG0, read back via KUSEG and KSEG1
    rt.write32(0x80068800u, 0xCAFEBABEu);
    CHECK(rt.read32(0x00068800u) == 0xCAFEBABEu, "KUSEG mirror read");
    CHECK(rt.read32(0xA0068800u) == 0xCAFEBABEu, "KSEG1 mirror read");

    // 1KB Scratchpad @ 0x1F800000 / 0x9F800000
    rt.write32(0x1F800100u, 0x12345678u);
    CHECK(rt.read32(0x9F800100u) == 0x12345678u, "Scratchpad KSEG0 mirror");
}

void test_unaligned_lwl_lwr_swl_swr() {
    PsxRuntime rt;
    rt.write32(0x80020000u, 0x44332211u);
    rt.write32(0x80020004u, 0x88776655u);

    // Load unaligned 32-bit little-endian integer starting at 0x80020001 -> expected 0x55443322
    // Standard MIPS little-endian unaligned load sequence:
    //   lwl $t0, 4($base)   ; addr = 0x80020001 + 3 = 0x80020004
    //   lwr $t0, 1($base)   ; addr = 0x80020001
    u32 reg_val = 0xAAAAAAAAu;
    reg_val = rt.exec_lwl(0x80020004u, reg_val);
    reg_val = rt.exec_lwr(0x80020001u, reg_val);
    CHECK(reg_val == 0x55443322u, "Unaligned LWL+LWR reconstruction");

    // Store unaligned 0xDEADBEEFu at 0x80020011 via SWL(addr+3) + SWR(addr)
    rt.write32(0x80020010u, 0x11111111u);
    rt.write32(0x80020014u, 0x22222222u);
    rt.exec_swl(0x80020014u, 0xDEADBEEFu);
    rt.exec_swr(0x80020011u, 0xDEADBEEFu);

    u32 verify = 0;
    verify = rt.exec_lwl(0x80020014u, verify);
    verify = rt.exec_lwr(0x80020011u, verify);
    CHECK(verify == 0xDEADBEEFu, "Unaligned SWL+SWR roundtrip");
}

void test_mips_and_gte_decoder() {
    // addiu $sp, $sp, -32 -> 0x27BDFFE0
    const auto d1 = decode_mips(0x27BDFFE0u);
    CHECK(d1.op == MipsOp::ADDIU && d1.rs == reg::SP && d1.rt == reg::SP && d1.simm16() == -32,
          "Decode addiu $sp, $sp, -32");

    // GTE RTPS (sf=1, lm=0): 0x4A180001
    const auto d2 = decode_mips(0x4A180001u);
    CHECK(d2.op == MipsOp::GTE_CMD && d2.gte_func == gte_fn::RTPS && d2.gte_sf && !d2.gte_lm,
          "Decode COP2 GTE RTPS");

    // BGEZAL $a0, +16
    const auto d3 = decode_mips(0x04910004u);
    CHECK(d3.op == MipsOp::BGEZAL && d3.is_call() && d3.has_delay_slot(),
          "Decode REGIMM BGEZAL");
}

void test_gte_rtps_widescreen_and_nclip() {
    GteEngine gte;
    // Identity rotation matrix (4096 = 1.0 in 4.12 fixed point)
    gte.write_ctrl(0, 4096u); // R11=1, R12=0
    gte.write_ctrl(2, 4096u); // R22=1, R23=0
    gte.write_ctrl(4, 4096u); // R33=1

    // Camera translation Z = 200, H = 200 -> perspective scale = 1.0!
    gte.write_ctrl(5, 0);
    gte.write_ctrl(6, 0);
    gte.write_ctrl(7, 200u);
    gte.write_ctrl(24, 160u << 16); // OFX = 160
    gte.write_ctrl(25, 120u << 16); // OFY = 120
    gte.write_ctrl(26, 200u);       // H   = 200

    // Vertex V0 = (40, -30, 0) -> Expected screen XY = (160 + 40, 120 - 30) = (200, 90)
    const u32 vxy0 = static_cast<u32>(static_cast<u16>(40)) | (static_cast<u32>(static_cast<u16>(static_cast<s16>(-30))) << 16);
    gte.write_data(0, vxy0);
    gte.write_data(1, 0);
    gte.execute(0x4A180001u); // RTPS (sf=1)

    const u32 sxy2 = gte.read_data(14);
    const s16 sx = static_cast<s16>(sxy2 & 0xFFFFu);
    const s16 sy = static_cast<s16>((sxy2 >> 16) & 0xFFFFu);
    CHECK(sx == 200 && sy == 90, "GTE RTPS 4:3 projection accuracy");

    // Enable 16:9 Widescreen X scale (0.75) -> Expected SX = 160 + 40 * 0.75 = 190, SY unchanged = 90
    gte.set_widescreen_scale(0.75f);
    gte.execute(0x4A180001u);
    const u32 ws_sxy2 = gte.read_data(14);
    const s16 ws_sx = static_cast<s16>(ws_sxy2 & 0xFFFFu);
    const s16 ws_sy = static_cast<s16>((ws_sxy2 >> 16) & 0xFFFFu);
    CHECK(ws_sx == 190 && ws_sy == 90, "GTE RTPS 16:9 widescreen hor+ projection");

    // NCLIP clockwise triangle test via SXYP FIFO push: (10,10), (20,10), (10,20) -> area = +100
    gte.write_data(15, (10u << 16) | 10u);
    gte.write_data(15, (10u << 16) | 20u);
    gte.write_data(15, (20u << 16) | 10u);
    gte.execute(0x4A000006u); // NCLIP
    CHECK(static_cast<s32>(gte.read_data(24)) == 100, "GTE NCLIP determinant");

    // LZCS / LZCR count leading bits
    gte.write_data(30, 0x00100000u);
    CHECK(gte.read_data(31) == 11u, "GTE LZCR leading zero count");
}

void test_gpu_otc_and_dma_linked_list() {
    PsxRuntime rt;
    // Clear 8-entry OT at 0x80090000..0x8009001C via DMA6 OTC
    rt.write32(0x1F8010E0u, 0x0009001Cu);
    rt.write32(0x1F8010E4u, 8u);
    rt.write32(0x1F8010E8u, 0x11000002u);

    CHECK(rt.read32(0x8009001Cu) == 0x00090018u, "OTC head points to prev");
    CHECK(rt.read32(0x80090000u) == 0x00FFFFFFu, "OTC tail sentinel 0x00FFFFFF");

    // Insert a flat red 16x16 rectangle GP0(0x78) packet at 0x80091000 into OT[4]
    const u32 prev = rt.read32(0x80090010u) & 0x00FFFFFFu;
    rt.write32(0x80091000u, (2u << 24) | prev);
    rt.write32(0x80091004u, 0x780000FFu); // GP0 16x16 opaque monochrome rect, Red=255
    rt.write32(0x80091008u, (20u << 16) | 30u); // X=30, Y=20
    rt.write32(0x80090010u, 0x00091000u);

    // Trigger DMA2 Linked-List GPU render
    rt.write32(0x1F8010A0u, 0x0009001Cu);
    rt.write32(0x1F8010A4u, 0u);
    rt.write32(0x1F8010A8u, 0x01000401u);

    CHECK(rt.gpu().primitives_drawn() == 1u, "DMA2 Linked-List dispatched GP0 primitive");
    CHECK((rt.gpu().read_vram_pixel(35, 25) & 0x1Fu) == 31u, "Red pixel rasterized in VRAM");
}

// Recompiled equivalent test function compiled directly in C++ via our lifter output format
void test_interpreter_and_lifter_pipeline() {
    // Small MIPS program at 0x80010000:
    //   0x80010000: addiu $t0, $zero, 5      (0x24080005)
    //   0x80010004: addiu $v0, $zero, 0      (0x24020000)
    // loop (0x80010008):
    //   0x80010008: addu  $v0, $v0, $t0      (0x00481021)
    //   0x8001000C: bne   $t0, $zero, loop   (0x1500FFFE)
    //   0x80010010: addiu $t0, $t0, -1       (0x2508FFFF)  <-- branch delay slot! (executes BEFORE next iter)
    //   0x80010014: jr    $ra                (0x03E00008)
    //   0x80010018: nop                      (0x00000000)
    // Trace:
    // iter 1: v0=0+5=5, cond(5!=0)=true, delay slot t0=4 -> goto loop
    // iter 2: v0=5+4=9, cond(4!=0)=true, delay slot t0=3 -> goto loop
    // iter 3: v0=9+3=12, cond(3!=0)=true, delay slot t0=2 -> goto loop
    // iter 4: v0=12+2=14, cond(2!=0)=true, delay slot t0=1 -> goto loop
    // iter 5: v0=14+1=15, cond(1!=0)=true, delay slot t0=0 -> goto loop
    // iter 6: v0=15+0=15, cond(0!=0)=false, delay slot t0=-1 -> fallthrough -> jr $ra!
    const std::vector<u32> words = {
        0x24080005u,
        0x24020000u,
        0x00481021u,
        0x1500FFFEu,
        0x2508FFFFu,
        0x03E00008u,
        0x00000000u
    };

    const auto exe_bytes = build_psx_exe(0x80010000u, 0x80010000u, 0x80050000u, 0x801FFF00u, words);
    PsxExeImage exe{};
    CHECK(parse_psx_exe(exe_bytes, exe), "Parse synthesized PS-X EXE");

    PsxRuntime rt;
    CpuContext ctx{};
    rt.load_exe(exe, ctx);
    rt.run(ctx, 1000);

    CHECK(ctx.halted, "Program returned cleanly to sentinel $ra");
    CHECK(ctx.get_reg(reg::V0) == 15u, "Delay-slot loop sum $v0 == 15");
    CHECK(ctx.get_reg(reg::T0) == 0xFFFFFFFFu, "Delay-slot final decrement $t0 == -1");

    // Verify CFG function discovery and C++ lifter code generation
    const auto funcs = discover_functions(exe.header.t_addr, exe.payload, exe.header.pc0);
    CHECK(!funcs.empty() && funcs[0].start_addr == 0x80010000u, "CFG discovered entry function");
    CHECK(funcs[0].local_labels.contains(0x80010008u), "CFG discovered loop label loc_80010008");

    const std::string cpp = lift_function_to_cpp(funcs[0], exe.header.t_addr, exe.payload, true);
    CHECK(cpp.find("loc_80010008:") != std::string::npos, "Lifter emitted local branch label");
    CHECK(cpp.find("goto loc_80010008;") != std::string::npos, "Lifter emitted delay-slot-aware goto");
}

// Regression: a branch delay slot that is itself a branch target must run only once on the
// not-taken path (the lifter emits it inside the branch block AND again under its label).
void test_lifter_labeled_delay_slot_runs_once() {
    //   0x80010000: addiu $t0, $zero, 3     (0x24080003)
    //   0x80010004: bne   $t0, $t0, 0x80010008  (0x15080000)  never taken, targets its own delay slot
    //   0x80010008: addiu $v0, $v0, 1       (0x24420001)  <-- delay slot + branch label
    //   0x8001000C: jr    $ra               (0x03E00008)
    //   0x80010010: nop
    const std::vector<u32> words = {0x24080003u, 0x15080000u, 0x24420001u, 0x03E00008u, 0x00000000u};

    const auto exe_bytes = build_psx_exe(0x80010000u, 0x80010000u, 0x80050000u, 0x801FFF00u, words);
    PsxExeImage exe{};
    CHECK(parse_psx_exe(exe_bytes, exe), "Parse synthesized PS-X EXE (labeled delay slot)");

    PsxRuntime rt;
    CpuContext ctx{};
    rt.load_exe(exe, ctx);
    rt.run(ctx, 1000);
    CHECK(ctx.get_reg(reg::V0) == 1u, "Interpreter runs labeled delay slot exactly once");

    const auto funcs = discover_functions(exe.header.t_addr, exe.payload, exe.header.pc0);
    CHECK(!funcs.empty() && funcs[0].local_labels.contains(0x80010008u), "CFG labels the delay slot");
    if (funcs.empty()) return;

    const std::string cpp = lift_function_to_cpp(funcs[0], exe.header.t_addr, exe.payload, false);
    const std::string ds_stmt = "ctx.set_reg(2, ctx.get_reg(2) + static_cast<u32>(1));";
    const auto first  = cpp.find(ds_stmt);
    const auto skip   = cpp.find("goto dsk_80010008;");
    const auto label  = cpp.find("loc_80010008:");
    const auto second = (label != std::string::npos) ? cpp.find(ds_stmt, label) : std::string::npos;
    const auto target = cpp.find("dsk_80010008:;");

    CHECK(first != std::string::npos && second != std::string::npos && first < label,
          "Delay slot emitted inside the branch block and again under its label");
    CHECK(skip != std::string::npos && first < skip && skip < label,
          "Fall-through path jumps over the labeled delay-slot copy");
    CHECK(target != std::string::npos && second < target,
          "Skip label placed right after the labeled delay-slot copy");
}

void test_vs_overlay_catalog() {
    const auto battle = lookup_vs_overlay_profile("CDROM:\\MAP\\BATTLE.PRG;1");
    CHECK(battle.has_value() && battle->expected_ram_base == 0x80068800u,
          "Vagrant Story BATTLE.PRG RAM slot 0x80068800");

    const auto mainmenu = lookup_vs_overlay_profile("MENU/MAINMENU.PRG");
    CHECK(mainmenu.has_value() && mainmenu->slot == VsOverlaySlot::MenuManager &&
              mainmenu->expected_ram_base == 0x800F9800u,
          "Vagrant Story MAINMENU.PRG slot 0x800F9800");

    const auto screff = lookup_vs_overlay_profile("GIM/SCREFF2.PRG");
    CHECK(screff.has_value() && screff->slot == VsOverlaySlot::BattleAux &&
              screff->expected_ram_base == 0x800F9800u,
          "Vagrant Story SCREFF2.PRG slot 0x800F9800");

    for (const char* sub : {"MENU/MENU0.PRG", "MENU/MENU7.PRG", "MENU/MENUF.PRG"}) {
        const auto menu = lookup_vs_overlay_profile(sub);
        CHECK(menu.has_value() && menu->slot == VsOverlaySlot::SubMenu &&
                  menu->expected_ram_base == 0x80102800u,
              "Vagrant Story MENU0..MENUF submenu slot 0x80102800");
    }

    // Verify JAL-to-prologue exact KSEG0 base solver on a synthetic SubMenu .PRG @ 0x80102800:
    // +0x00: addiu $sp, $sp, -16 ; sw $ra, 0($sp) ; jal 0x80102814 ; nop ; jr $ra
    // +0x14: addiu $sp, $sp, -8  ; sw $ra, 0($sp) ; jal 0x80102800 ; nop ; jr $ra
    const u32 synth_words[] = {
        0x27BDFFF0u, 0xAFBF0000u, 0x0C040A05u, 0x00000000u, 0x03E00008u,
        0x27BDFFF8u, 0xAFBF0000u, 0x0C040A00u, 0x00000000u, 0x03E00008u
    };
    std::vector<u8> synth_bytes(sizeof(synth_words));
    std::memcpy(synth_bytes.data(), synth_words, sizeof(synth_words));
    const PrgAnalysis solved = analyze_vs_prg("CUSTOM_SUB.PRG", synth_bytes);
    CHECK(solved.inferred_base_addr == 0x80102800u,
          "analyze_vs_prg JAL-to-prologue solver inferred exact 0x80102800 base");
}

void aot_stub_menu0(CpuContext& ctx, PsxRuntime&) {
    ctx.set_reg(reg::V0, 100u);
    ctx.pc      = ctx.get_reg(reg::RA);
    ctx.next_pc = ctx.pc + 4u;
}

void aot_stub_menu7(CpuContext& ctx, PsxRuntime&) {
    ctx.set_reg(reg::V0, 700u);
    ctx.pc      = ctx.get_reg(reg::RA);
    ctx.next_pc = ctx.pc + 4u;
}

void test_submenu_overlay_signature_switching() {
    PsxRuntime rt;

    // Two distinct SubMenu overlays sharing KSEG0 slot 0x80102800:
    // MENU0 bytes: addiu $v0, $zero, 100 ; jr $ra ; nop
    const u32 menu0_words[] = {0x24020064u, 0x03E00008u, 0x00000000u};
    // MENU7 bytes: addiu $v0, $zero, 700 ; jr $ra ; nop
    const u32 menu7_words[] = {0x240202BCu, 0x03E00008u, 0x00000000u};
    // Unregistered custom bytes: addiu $v0, $zero, 999 ; jr $ra ; nop
    const u32 other_words[] = {0x240203E7u, 0x03E00008u, 0x00000000u};

    std::vector<u8> b0(sizeof(menu0_words)), b7(sizeof(menu7_words)), bo(sizeof(other_words));
    std::memcpy(b0.data(), menu0_words, sizeof(menu0_words));
    std::memcpy(b7.data(), menu7_words, sizeof(menu7_words));
    std::memcpy(bo.data(), other_words, sizeof(other_words));

    rt.register_func(0x80102800u, static_cast<u32>(b0.size()), fnv1a_64(b0.data(), b0.size()), &aot_stub_menu0);
    rt.register_func(0x80102800u, static_cast<u32>(b7.size()), fnv1a_64(b7.data(), b7.size()), &aot_stub_menu7);

    CpuContext ctx{};
    ctx.set_reg(reg::RA, 0x80000000u);

    // 1. Load MENU0 -> dispatches aot_stub_menu0
    rt.load_overlay(0x80102800u, b0);
    rt.call_function(ctx, 0x80102800u);
    CHECK(ctx.get_reg(reg::V0) == 100u && rt.aot_dispatch_count() == 1u,
          "SubMenu slot 0x80102800 dispatched MENU0 AOT function");

    // 2. Hot-swap MENU7 into same 0x80102800 slot -> page epoch invalidates MENU0 & dispatches aot_stub_menu7!
    rt.load_overlay(0x80102800u, b7);
    rt.call_function(ctx, 0x80102800u);
    CHECK(ctx.get_reg(reg::V0) == 700u && rt.aot_dispatch_count() == 2u,
          "SubMenu slot 0x80102800 hot-swapped to MENU7 AOT function");

    // 3. Hot-swap unregistered overlay -> falls back cleanly to MIPS interpreter!
    rt.load_overlay(0x80102800u, bo);
    rt.call_function(ctx, 0x80102800u);
    CHECK(ctx.get_reg(reg::V0) == 999u && rt.aot_dispatch_count() == 2u,
          "SubMenu slot 0x80102800 fell back to interpreter on unknown overlay");
}

void test_spu_dma4_adpcm_and_adsr() {
    PsxRuntime rt;

    // Synthesize a 16-byte Sony ADPCM block in PSX RAM at 0x80080000:
    // byte 0: shift=2, filter=0 (0x02), byte 1: flags=0x05 (Loop Start + Loop End, one-shot)
    // bytes 2..15: 0x33 (+3 nibble -> (3 << 12) >> 2 = +3072 PCM amplitude)
    rt.write32(0x80080000u, 0x33330502u);
    rt.write32(0x80080004u, 0x33333333u);
    rt.write32(0x80080008u, 0x33333333u);
    rt.write32(0x8008000Cu, 0x33333333u);

    // Set SPU transfer address to 0x1000 (reg = 0x1000 / 8 = 0x0200) and transfer 4 words via DMA4!
    rt.write16(0x1F801DA6u, 0x0200u);
    rt.write32(0x1F8010C0u, 0x00080000u); // D4_MADR
    rt.write32(0x1F8010C4u, 0x00010004u); // D4_BCR = 4 words
    rt.write32(0x1F8010C8u, 0x01000201u); // D4_CHCR = RAM -> SPU, SyncMode=1, Start

    CHECK(rt.spu().bytes_uploaded() == 16u, "DMA4 uploaded 16-byte ADPCM block to SPU RAM");

    // Configure SPU Main Volume, SPUCNT (Enable + Unmute), Voice 0 registers, and KON!
    rt.write16(0x1F801D80u, 0x3FFFu); // Main Vol L
    rt.write16(0x1F801D82u, 0x3FFFu); // Main Vol R
    rt.write16(0x1F801DAAu, 0xC000u); // SPUCNT: Enable (15) + Unmute (14)

    rt.write16(0x1F801C00u, 0x3FFFu); // Voice 0 Vol L
    rt.write16(0x1F801C02u, 0x3FFFu); // Voice 0 Vol R
    rt.write16(0x1F801C04u, 0x1000u); // Voice 0 Pitch = 44.1 kHz (1.0x)
    rt.write16(0x1F801C06u, 0x0200u); // Voice 0 Start Addr = 0x1000
    rt.write16(0x1F801C08u, 0x000Fu); // Fast Attack (AR=0) + Max Sustain Level (SL=0xF)
    rt.write16(0x1F801C0Au, 0x0000u);
    rt.write16(0x1F801D88u, 0x0001u); // KON Voice 0!

    CHECK(rt.spu().key_on_count() == 1u, "SPU Voice 0 Key-On latched");
    CHECK((rt.read16(0x1F801D9Cu) & 1u) == 0u, "ENDX bit 0 cleared on KON");

    // Tick 32 stereo samples (32 * 768 cycles) -> crosses 28-sample block end -> sets ENDX bit 0!
    CpuContext dummy{};
    rt.tick_hardware(dummy, 32ull * PsxSpu::CYCLES_PER_SAMPLE);

    CHECK((rt.read16(0x1F801D9Cu) & 1u) == 1u, "ENDX bit 0 set after 28-sample ADPCM Loop End");

    std::vector<s16> pcm;
    const std::size_t got = rt.spu().pop_samples(pcm, 32);
    bool non_zero_audio = false;
    for (s16 s : pcm) {
        if (s > 500) {
            non_zero_audio = true;
            break;
        }
    }
    CHECK(got == 32u && non_zero_audio, "SPU synthesized non-zero 44.1 kHz stereo PCM");
}

void test_mdec_dma0_dma1_rgb24_and_rgb15() {
    PsxRuntime rt;

    // Reset MDEC & enable DMA0/DMA1 requests
    rt.write32(0x1F801824u, 0x80000000u);
    CHECK(rt.read32(0x1F801824u) == 0x80040000u, "MDEC1 reset status 0x80040000");
    rt.write32(0x1F801824u, 0x60000000u);

    // Enable DMA1 channel interrupt in DICR (Master Enable bit 23 + Ch1 Enable bit 17)
    rt.write32(0x1F8010F4u, (1u << 23) | (1u << 17));

    // Synthesize 1 color 16x16 macroblock in RAM @ 0x80070000 (24-bit RGB888, depth=2):
    // 6 blocks (Cr, Cb, Y1, Y2, Y3, Y4), each 2 halfwords (DC + 0xFE00 EOB) = 12 halfwords = 6 parameter words!
    // Cr=0 (q=1, dc=0 -> 0x0400), Cb=0 (0x0400), Y1..Y4: q=1, dc=+240 (0x04F0 -> dct[0]=480 -> Y=+60 -> RGB = 188)
    rt.write32(0x80070000u, (1u << 29) | (2u << 27) | 6u); // Cmd 1, depth=2 (24-bit), 6 param words
    rt.write32(0x80070004u, 0xFE000400u); // Cr: DC=0, EOB
    rt.write32(0x80070008u, 0xFE000400u); // Cb: DC=0, EOB
    rt.write32(0x8007000Cu, 0xFE0004F0u); // Y1: DC=+240, EOB
    rt.write32(0x80070010u, 0xFE0004F0u); // Y2: DC=+240, EOB
    rt.write32(0x80070014u, 0xFE0004F0u); // Y3: DC=+240, EOB
    rt.write32(0x80070018u, 0xFE0004F0u); // Y4: DC=+240, EOB

    // Arm DMA1 (DecDCTout -> 0x80071000, 192 words = 6 blocks of 32 words) BEFORE DMA0 (DecDCTin)!
    rt.write32(0x1F801090u, 0x00071000u);
    rt.write32(0x1F801094u, (6u << 16) | 32u);
    rt.write32(0x1F801098u, 0x01000200u);

    // Trigger DMA0 (DecDCTin from 0x80070000, 7 words)
    rt.write32(0x1F801080u, 0x00070000u);
    rt.write32(0x1F801084u, (1u << 16) | 7u);
    rt.write32(0x1F801088u, 0x01000201u);

    CHECK(rt.mdec().macroblocks_decoded() == 1u, "MDEC decoded 16x16 24-bit macroblock via DMA0");

    // Advance hardware 2048 cycles so DMA1 completes & asserts IRQ3 (DMA)
    CpuContext dummy{};
    rt.tick_hardware(dummy, 2048ull);
    CHECK((rt.read32(0x1F801098u) & (1u << 24)) == 0u, "DMA1 CHCR busy bit cleared on completion");
    CHECK((rt.read32(0x1F801070u) & (1u << 3)) != 0u, "DMA1 completion raised IRQ3 in I_STAT");

    const u32 rgb_word0 = rt.read32(0x80071000u);
    const u32 r0 = rgb_word0 & 0xFFu;
    const u32 g0 = (rgb_word0 >> 8) & 0xFFu;
    const u32 b0 = (rgb_word0 >> 16) & 0xFFu;
    CHECK(r0 >= 186u && r0 <= 190u && r0 == g0 && g0 == b0, "MDEC 24-bit IDCT + YUV->RGB888 gray level ~188");

    // Also test 15-bit RGB555 output with STP bit 15 enabled (bit 25)
    rt.write32(0x80070000u, (1u << 29) | (3u << 27) | (1u << 25) | 6u);
    rt.write32(0x1F801080u, 0x00070000u);
    rt.write32(0x1F801084u, (1u << 16) | 7u);
    rt.write32(0x1F801088u, 0x01000201u);

    rt.write32(0x1F801090u, 0x00072000u);
    rt.write32(0x1F801094u, (4u << 16) | 32u); // 128 words = 256 RGB555 pixels
    rt.write32(0x1F801098u, 0x01000200u);
    rt.tick_hardware(dummy, 2048ull);

    const u16 px555 = static_cast<u16>(rt.read32(0x80072000u) & 0xFFFFu);
    CHECK((px555 & 0x8000u) != 0u && (px555 & 0x1Fu) >= 23u, "MDEC 15-bit RGB555 + STP bit15 output verified");
}

void test_dicr_byte_mmio_masking_and_ack() {
    PsxRuntime rt;

    // Mirror PsyQ StartDMA3 (StCdInterrupt) byte-level DICR manipulation:
    // Master Enable (bit 23) + DMA1 Enable (bit 17) + DMA3 Enable (bit 19) -> byte 2 = 0x8A
    rt.write32(0x1F8010F4u, 0x008A0000u);
    rt.write32(0x1F801070u, ~0x08u); // Clear IRQ3

    // Intermediate .STR sector (sectorNo != nSectors - 1): disable DMA3 IRQ via sb to 0x1F8010F6
    const u8 en_mid = static_cast<u8>(rt.read8(0x1F8010F6u) & ~0x08u);
    rt.write8(0x1F8010F6u, en_mid);

    rt.write32(0x1F8010B0u, 0x00074000u);
    rt.write32(0x1F8010B4u, 8u);
    rt.write32(0x1F8010B8u, 0x11000000u);
    CHECK((rt.read32(0x1F801070u) & (1u << 3)) == 0u, "DMA3 does not raise IRQ3 when masked via 8-bit DICR+2");

    // Final .STR sector (sectorNo == nSectors - 1): re-enable DMA3 IRQ via sb to 0x1F8010F6
    rt.write8(0x1F8010F6u, static_cast<u8>(rt.read8(0x1F8010F6u) | 0x08u));
    rt.write32(0x1F8010B0u, 0x00074020u);
    rt.write32(0x1F8010B4u, 8u);
    rt.write32(0x1F8010B8u, 0x11000000u);
    CHECK((rt.read32(0x1F801070u) & (1u << 3)) != 0u, "DMA3 raises IRQ3 on final sector when unmasked via 8-bit DICR+2");

    // Acknowledge IRQ3 in I_STAT and DMA3 flag via sb to 0x1F8010F7
    rt.write32(0x1F801070u, ~(1u << 3));
    rt.write8(0x1F8010F7u, 0x08u);
    CHECK((rt.read32(0x1F8010F4u) & 0x88000000u) == 0u, "8-bit write to DICR+3 clears DMA3 flag and master bit 31");
}

// The upscaled picture is drawn from a queue later than the native one. A texture that the game
// replaces before the queue is drawn (memory card screen icons, animated textures) must still be
// sampled as it was when the primitive was sent.
void test_gpu_upscaled_queue_sees_old_textures() {
    PsxGpu gpu;
    gpu.set_internal_scale(2);
    const auto upload_pixel = [&](u32 x, u32 y, u16 color) {
        gpu.write_gp0(0xA0000000u);
        gpu.write_gp0((y << 16) | x);
        gpu.write_gp0((1u << 16) | 1u);
        gpu.write_gp0(color);
    };
    gpu.write_gp0(0xE100010Au);       // Texpage x=640, 15bpp direct
    upload_pixel(640, 0, 0x001Fu);    // Red texel
    gpu.write_gp0(0x6D000000u);       // 1x1 raw textured sprite at (20,20) sampling (0,0)
    gpu.write_gp0((20u << 16) | 20u);
    gpu.write_gp0(0u);
    upload_pixel(640, 0, 0x7C00u);    // Texture replaced by blue before the queue is drawn
    gpu.write_gp0(0x6D000000u);       // Second sprite at (21,20) uses the new texel
    gpu.write_gp0((20u << 16) | 21u);
    gpu.write_gp0(0u);
    gpu.flush_hr();

    const auto& hr = gpu.hr_vram();
    const auto hr_px = [&](u32 x, u32 y) { return hr[static_cast<std::size_t>(y * 2) * (PsxGpu::VRAM_WIDTH * 2) + x * 2]; };
    CHECK(gpu.read_vram_pixel(20, 20) == 0x001Fu && gpu.read_vram_pixel(21, 20) == 0x7C00u, "Native sprites use old/new texel");
    // The upscaled picture is 24-bit (0x00BBGGRR)
    CHECK(hr_px(20, 20) == 0x0000FFu, "Upscaled sprite samples the texture as it was when sent");
    CHECK(hr_px(21, 20) == 0xFF0000u, "Upscaled sprite after the upload samples the new texture");

    // Dark modulation (light value 16/128) keeps 8-bit precision in the upscaled picture: the
    // native 15-bit result collapses to 3/31, the upscaled one stays at 31/255
    upload_pixel(640, 0, 0x001Fu);    // Red texel again
    gpu.write_gp0(0x6C101010u);       // 1x1 modulated textured sprite, colour 16
    gpu.write_gp0((22u << 16) | 22u);
    gpu.write_gp0(0u);
    gpu.flush_hr();
    CHECK((gpu.read_vram_pixel(22, 22) & 0x1Fu) == 3u, "Native dark texel is quantized to 15-bit");
    CHECK(gpu.hr_vram()[static_cast<std::size_t>(22 * 2) * (PsxGpu::VRAM_WIDTH * 2) + 22 * 2] == 0x00001Fu,
          "Upscaled dark texel keeps full 8-bit precision (true colour)");

    // ...but where the PS1 result is black (light value 4/128 -> 15-bit 0), it stays black
    gpu.write_gp0(0x6C040404u);
    gpu.write_gp0((23u << 16) | 23u);
    gpu.write_gp0(0u);
    gpu.flush_hr();
    CHECK(gpu.read_vram_pixel(23, 23) == 0u, "Native very dark texel is black");
    CHECK(gpu.hr_vram()[static_cast<std::size_t>(23 * 2) * (PsxGpu::VRAM_WIDTH * 2) + 23 * 2] == 0u,
          "Upscaled very dark texel stays black like hardware (no faint texture in black areas)");
}

// A polygon whose vertices all use the same texture coordinate must sample exactly that texel in
// the upscaled picture too. Vagrant Story covers the void around rooms with such polygons; with
// float rounding (5 -> 4.9999) the neighbouring transparent texel left holes that showed as streaks.
void test_gpu_upscaled_constant_uv_has_no_holes() {
    PsxGpu gpu;
    constexpr u32 S = 5;
    gpu.set_internal_scale(S);
    gpu.write_gp0(0xA0000000u);              // Texel (5,0) of the 15bpp page at x=640: grey
    gpu.write_gp0((0u << 16) | 645u);
    gpu.write_gp0((1u << 16) | 1u);
    gpu.write_gp0(0x4210u);                  // (texel 4 stays 0 = transparent)
    gpu.write_gp0(0x24808080u);              // Flat textured triangle, all uv = (5,0)
    gpu.write_gp0((100u << 16) | 100u);
    gpu.write_gp0(5u);
    gpu.write_gp0((130u << 16) | 300u);
    gpu.write_gp0((0x010Au << 16) | 5u);     // Texpage x=640, 15bpp
    gpu.write_gp0((300u << 16) | 120u);
    gpu.write_gp0(5u);
    gpu.flush_hr();

    // The upscaled pixel at (x*S, y*S) samples the same point as native pixel (x, y)
    u32 covered = 0, holes = 0;
    for (u32 y = 95; y < 305; ++y) {
        for (u32 x = 95; x < 305; ++x) {
            if (gpu.read_vram_pixel(x, y) == 0) continue;
            ++covered;
            if (gpu.hr_vram()[static_cast<std::size_t>(y * S) * (PsxGpu::VRAM_WIDTH * S) + x * S] == 0) ++holes;
        }
    }
    CHECK(covered > 10000u, "Native constant-uv triangle is drawn");
    CHECK(holes == 0u, "Upscaled constant-uv triangle has no holes (no streaks)");
}

// Vagrant Story's rim light: a white (FFFFFF) copy of the model shifted ~1 pixel toward the light,
// then the normal model on top. Thin mode shifts the copy by 1 upscaled pixel instead of 1 native
// pixel (N upscaled pixels); Soft fades it out semi-transparently; Off skips it.
void test_gpu_rim_light_modes() {
    const auto draw = [](PsxGpu& gpu) {
        gpu.write_gp0(0xA0000000u);          // Opaque grey texel (0,0) on the 15bpp page at x=640
        gpu.write_gp0((0u << 16) | 640u);
        gpu.write_gp0((1u << 16) | 1u);
        gpu.write_gp0(0x4210u);
        const auto tri = [&](u32 color, s32 dx, s32 dy) {
            gpu.write_gp0(0x24000000u | color);
            gpu.write_gp0((static_cast<u32>(20 + dy) << 16) | static_cast<u32>(20 + dx));
            gpu.write_gp0(0u);
            gpu.write_gp0((static_cast<u32>(20 + dy) << 16) | static_cast<u32>(60 + dx));
            gpu.write_gp0(0x010A0000u);      // Texpage x=640, 15bpp
            gpu.write_gp0((static_cast<u32>(60 + dy) << 16) | static_cast<u32>(20 + dx));
            gpu.write_gp0(0u);
        };
        tri(0xFFFFFFu, 1, -1);               // Rim copy: up-right by (1,-1)
        tri(0x808080u, 0, 0);                // Normal model
        gpu.flush_hr();
    };
    constexpr u32 S = 4;
    const auto hr_at = [&](const PsxGpu& gpu, u32 x, u32 y) {
        return gpu.hr_vram()[static_cast<std::size_t>(y) * (PsxGpu::VRAM_WIDTH * S) + x];
    };

    // Native row 19 (just above the normal triangle) is where the game's rim shows; upscaled rows 76..79
    PsxGpu soft;
    soft.set_internal_scale(S);
    draw(soft);                              // Soft is the default
    const u32 outer = hr_at(soft, 30 * S, 19 * S) & 0xFFu, inner = hr_at(soft, 30 * S, 20 * S - 1) & 0xFFu;
    CHECK(soft.read_vram_pixel(30, 19) != 0u, "Soft: native picture is unchanged");
    CHECK(outer > 0u && inner > outer, "Soft: rim light fades outward");
    const u32 rim_full = (static_cast<u32>(((0x10u * 527u + 23u) >> 6)) * 0xFFu) >> 7; // Opaque rim brightness
    CHECK(inner < std::min(255u, rim_full), "Soft: rim light is partly transparent");

    PsxGpu sharp;
    sharp.set_internal_scale(S);
    sharp.set_rim_light(PsxGpu::RimLight::Sharp);
    draw(sharp);
    CHECK(hr_at(sharp, 30 * S, 19 * S) == 0u, "Sharp: upscaled rim no longer reaches a whole native pixel up");
    CHECK(hr_at(sharp, 30 * S, 20 * S - 1) != 0u, "Sharp: upscaled rim is one upscaled pixel tall");

    PsxGpu off;
    off.set_internal_scale(S);
    off.set_rim_light(PsxGpu::RimLight::Off);
    draw(off);
    CHECK(off.read_vram_pixel(30, 19) == 0u && hr_at(off, 30 * S, 20 * S - 1) == 0u, "Off: rim copy is not drawn");
}

// Exact GTE positions (PGXP-style) move upscaled polygon edges inside the native pixel; the
// native picture is unchanged, and entries that do not round to the vertex or are stale are ignored.
void test_gpu_exact_vertex_positions() {
    constexpr u32 S = 4;
    const auto xy = [](s32 x, s32 y) { return (static_cast<u32>(static_cast<u16>(y)) << 16) | static_cast<u16>(x); };
    PgxpCache cache;
    cache.set_enabled(true);
    cache.store(xy(10, 10), 10.75f, 10.0f, 100.0f);
    cache.store(xy(10, 40), 10.75f, 40.0f, 100.0f);
    cache.store(xy(30, 10), 29.40f, 10.0f, 100.0f);   // Does not round down to 30: must be ignored
    PgxpCache::Point pt;
    CHECK(cache.lookup(xy(10, 10), pt) && pt.x == 10.75f, "Exact point is found by its pixel value");
    CHECK(!cache.lookup(xy(30, 10), pt), "Exact point that rounds to another pixel is rejected");

    PsxGpu gpu;
    gpu.set_internal_scale(S);
    gpu.set_pgxp(&cache);
    gpu.write_gp0(0x20FFFFFFu);                         // Flat white triangle
    gpu.write_gp0(xy(10, 10));
    gpu.write_gp0(xy(10, 40));
    gpu.write_gp0(xy(30, 10));
    gpu.flush_hr();
    const auto hr_at = [&](u32 x, u32 y) { return gpu.hr_vram()[static_cast<std::size_t>(y) * (PsxGpu::VRAM_WIDTH * S) + x]; };
    CHECK(gpu.read_vram_pixel(10, 15) != 0u, "Native picture keeps the whole-pixel vertex");
    CHECK(hr_at(10 * S, 15 * S) == 0u && hr_at(10 * S + 2, 15 * S) == 0u, "Upscaled edge starts at the exact position");
    CHECK(hr_at(10 * S + 3, 15 * S) != 0u, "Upscaled edge is drawn from floor(10.75 * 4) = 43");

    for (u32 f = 0; f <= PgxpCache::MAX_AGE_FRAMES; ++f) cache.begin_frame();
    CHECK(!cache.lookup(xy(10, 10), pt), "Stale exact points are not used");
}

// Perspective-correct texturing: vertex A (u=0, depth 100) to B (u=200, depth 400). Halfway across
// the screen the affine PS1 mapping samples u=100; the correct perspective position is u=40.
void test_gpu_perspective_textures() {
    constexpr u32 S = 2;
    const auto xy = [](s32 x, s32 y) { return (static_cast<u32>(static_cast<u16>(y)) << 16) | static_cast<u16>(x); };
    PgxpCache cache;
    cache.set_enabled(true);
    cache.store(xy(10, 10), 10.0f, 10.0f, 100.0f);
    cache.store(xy(110, 10), 110.0f, 10.0f, 400.0f);
    cache.store(xy(10, 60), 10.0f, 60.0f, 100.0f);
    const auto draw = [&](PsxGpu& gpu) {
        gpu.write_gp0(0xA0000000u);                    // 256x1 texture row at (640,0): red = u / 8
        gpu.write_gp0((0u << 16) | 640u);
        gpu.write_gp0((1u << 16) | 256u);
        for (u32 u = 0; u < 256; u += 2) gpu.write_gp0((u >> 3) | 0x0400u | ((((u + 1) >> 3) | 0x0400u) << 16));
        gpu.write_gp0(0x25000000u);                    // Raw textured triangle
        gpu.write_gp0(xy(10, 10));
        gpu.write_gp0(0u);                             // u=0
        gpu.write_gp0(xy(110, 10));
        gpu.write_gp0(0x010A0000u | 200u);             // u=200, texpage x=640 15bpp
        gpu.write_gp0(xy(10, 60));
        gpu.write_gp0(0u);
        gpu.flush_hr();
    };
    const auto red_at = [](const PsxGpu& gpu, u32 x, u32 y) {
        const u32 c = gpu.hr_vram()[static_cast<std::size_t>(y * S) * (PsxGpu::VRAM_WIDTH * S) + x * S] & 0xFFu;
        return (c * 31u + 127u) / 255u; // Back to 5 bits
    };
    PsxGpu affine;
    affine.set_internal_scale(S);
    affine.set_pgxp(&cache);
    draw(affine);
    PsxGpu persp;
    persp.set_internal_scale(S);
    persp.set_pgxp(&cache);
    persp.set_perspective_textures(true);
    draw(persp);
    const u32 ra = red_at(affine, 60, 10), rp = red_at(persp, 60, 10);
    CHECK(ra >= 11u && ra <= 13u, "Affine mapping samples u~100 halfway");
    CHECK(rp >= 4u && rp <= 6u, "Perspective mapping samples u~40 halfway");
    CHECK(persp.read_vram_pixel(60, 10) == affine.read_vram_pixel(60, 10), "Native picture is unchanged");
}

// Save state: loading brings memory, CPU and video memory back; a damaged file changes nothing
void test_save_state_round_trip() {
    PsxRuntime rt;
    CpuContext ctx{};
    rt.write32(0x80010000u, 0x11223344u);
    ctx.set_reg(5, 0xCAFEBABEu);
    ctx.pc = 0x80012340u;
    rt.gpu().write_gp0(0x02FFFFFFu); // Fill (0,0) 16x16 white
    rt.gpu().write_gp0(0u);
    rt.gpu().write_gp0((16u << 16) | 16u);
    std::vector<u8> st = rt.save_state(ctx);
    CHECK(st.size() > PSX_RAM_SIZE_2MB, "State holds at least the RAM");

    rt.write32(0x80010000u, 0xDEADBEEFu);
    ctx.set_reg(5, 1u);
    ctx.pc = 0x80000000u;
    rt.gpu().write_gp0(0x02000000u); // Fill black
    rt.gpu().write_gp0(0u);
    rt.gpu().write_gp0((16u << 16) | 16u);
    CHECK(rt.load_state(st, ctx), "State loads");
    CHECK(rt.read32(0x80010000u) == 0x11223344u, "RAM restored");
    CHECK(ctx.get_reg(5) == 0xCAFEBABEu && ctx.pc == 0x80012340u, "CPU registers restored");
    CHECK(rt.gpu().read_vram_pixel(3, 3) == 0x7FFFu, "VRAM restored");

    std::vector<u8> bad = st;
    bad.resize(bad.size() / 2);
    rt.write32(0x80010000u, 0x55667788u);
    CHECK(!rt.load_state(bad, ctx), "Truncated state is rejected");
    CHECK(rt.read32(0x80010000u) == 0x55667788u, "A rejected state leaves the machine as it was");
    std::vector<u8> wrong = st;
    wrong[0] ^= 0xFFu;
    CHECK(!rt.load_state(wrong, ctx), "Foreign file is rejected");
}

// GP1(03h) display off: the picture must be black (the game hides VRAM set-up before movies)
void test_gpu_display_off_is_black() {
    PsxGpu gpu;
    gpu.write_gp0(0x02FFFFFFu);              // Fill the display area white
    gpu.write_gp0(0u);
    gpu.write_gp0((240u << 16) | 320u);
    CHECK(!gpu.display_enabled(), "Display is off after reset (as on hardware) until the game enables it");
    gpu.write_gp1(0x03000000u);
    CHECK(gpu.display_enabled(), "GP1(03h) switches the display on");
    gpu.write_gp1(0x03000001u);
    CHECK(!gpu.display_enabled(), "GP1(03h) switches the display off");
    const auto path = std::filesystem::temp_directory_path() / "vsport_display_off.ppm";
    CHECK(gpu.dump_ppm(path, false), "Display dump written");
    std::ifstream in(path, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(path);
    CHECK(data.size() > 100 && static_cast<unsigned char>(data.back()) == 0u && static_cast<unsigned char>(data[data.size() / 2]) == 0u,
          "Display dump is black while the display is off");
    gpu.write_gp1(0x03000000u);
    CHECK(gpu.display_enabled(), "GP1(03h) switches the display back on");
}

// GP0(E6h) mask bit, and a sprite whose texture is its own draw area shifted 2 rows (Vagrant
// Story's cutscene trail effect): the upscaled picture must follow the same rules as native VRAM.
void test_gpu_mask_bit_and_self_texturing_sprite() {
    constexpr u32 S = 3;
    PsxGpu gpu;
    gpu.set_internal_scale(S);
    const auto hr_at = [&](u32 x, u32 y) { return gpu.hr_vram()[static_cast<std::size_t>(y * S) * (PsxGpu::VRAM_WIDTH * S) + x * S]; };
    const auto flat_tri = [&](u32 color) {
        gpu.write_gp0(0x20000000u | color);
        gpu.write_gp0((10u << 16) | 10u);
        gpu.write_gp0((10u << 16) | 30u);
        gpu.write_gp0((30u << 16) | 10u);
    };
    gpu.write_gp0(0xE6000001u);              // Set mask while drawing
    flat_tri(0x0000FFu);
    gpu.flush_hr();
    CHECK(gpu.read_vram_pixel(12, 12) == 0x801Fu, "E6 set-mask: native pixel gets bit 15");
    CHECK((hr_at(12, 12) & 0x01000000u) != 0, "E6 set-mask: upscaled pixel gets the mask bit");
    gpu.write_gp0(0xE6000002u);              // Check mask: masked pixels are protected
    flat_tri(0xFF0000u);
    gpu.flush_hr();
    CHECK(gpu.read_vram_pixel(12, 12) == 0x801Fu, "E6 check-mask: native masked pixel not overwritten");
    CHECK((hr_at(12, 12) & 0xFFFFFFu) == 0x0000FFu, "E6 check-mask: upscaled masked pixel not overwritten");
    gpu.write_gp0(0xE6000000u);

    // 16x16 pattern at (640,0), then a raw 15bpp sprite at (640,2) reading (0,0) of that page
    gpu.write_gp0(0xA0000000u);
    gpu.write_gp0((0u << 16) | 640u);
    gpu.write_gp0((16u << 16) | 16u);
    for (u32 i = 0; i < 16 * 16 / 2; ++i) {
        const u32 a = (i * 2) % 16, b = (i * 2 + 1) % 16, y = (i * 2) / 16;
        const u32 ca = ((a * 2) & 31u) | (((y * 2) & 31u) << 5) | 0x4000u, cb = ((b * 2) & 31u) | (((y * 2) & 31u) << 5) | 0x4000u;
        gpu.write_gp0(ca | (cb << 16));
    }
    gpu.write_gp0(0xE100010Au);              // Texpage x=640, 15bpp
    gpu.write_gp0(0x65000000u);              // Raw textured sprite, variable size
    gpu.write_gp0((2u << 16) | 640u);
    gpu.write_gp0(0u);
    gpu.write_gp0((14u << 16) | 16u);
    gpu.flush_hr();
    u32 mismatch = 0;
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 640; x < 656; ++x) {
            const u16 n = gpu.read_vram_pixel(x, y);
            const u32 expect = ((((n & 31u) * 527u + 23u) >> 6)) | (((((n >> 5) & 31u) * 527u + 23u) >> 6) << 8) |
                               (((((n >> 10) & 31u) * 527u + 23u) >> 6) << 16);
            if ((hr_at(x, y) & 0xFFFFFFu) != expect) ++mismatch;
        }
    }
    CHECK(gpu.read_vram_pixel(640, 4) == gpu.read_vram_pixel(640, 0), "Self-texturing sprite repeats rows natively (reads its own output)");
    CHECK(mismatch == 0u, "Upscaled self-texturing sprite matches the native result");
}

// GP0(E2h) texture window must wrap texture coordinates (Vagrant Story dialog balloons use
// E2000318: a 64x64 window), and polygons must not draw their right/bottom edges.
void test_gpu_texture_window_and_fill_rule() {
    PsxGpu gpu;
    gpu.write_vram_pixel(8, 0, 0x001Fu);  // Texel at u=8 (red)
    gpu.write_gp0(0xE1000100u);           // Texpage (0,0), 15bpp direct
    gpu.write_gp0(0xE2000018u);           // Texture window: mask X = 0x18 (*8 = 0xC0)

    // 1x1 raw textured sprite at (100,100) sampling u=72 -> windowed to u = 72 & ~0xC0 = 8
    gpu.write_gp0(0x6D000000u);
    gpu.write_gp0((100u << 16) | 100u);
    gpu.write_gp0(72u);
    CHECK(gpu.read_vram_pixel(100, 100) == 0x001Fu, "Texture window wraps u=72 to u=8");

    gpu.write_gp0(0xE2000000u);           // Window off: u=72 hits an empty (transparent) texel
    gpu.write_gp0(0x6D000000u);
    gpu.write_gp0((101u << 16) | 101u);
    gpu.write_gp0(72u);
    CHECK(gpu.read_vram_pixel(101, 101) == 0u, "Without a texture window u=72 is not wrapped");

    // Flat white quad with corners (10,10)-(14,14) covers exactly x,y in [10,14): 16 pixels
    gpu.write_gp0(0x28FFFFFFu);
    gpu.write_gp0((10u << 16) | 10u);
    gpu.write_gp0((10u << 16) | 14u);
    gpu.write_gp0((14u << 16) | 10u);
    gpu.write_gp0((14u << 16) | 14u);
    u32 covered = 0;
    bool edge_drawn = false;
    for (u32 y = 8; y <= 16; ++y) {
        for (u32 x = 8; x <= 16; ++x) {
            if (gpu.read_vram_pixel(x, y) != 0) {
                ++covered;
                if (x >= 14 || y >= 14 || x < 10 || y < 10) edge_drawn = true;
            }
        }
    }
    CHECK(covered == 16u, "Quad (10,10)-(14,14) draws exactly 4x4 pixels (no seam overlap)");
    CHECK(!edge_drawn, "Right/bottom polygon edges are excluded (top-left fill rule)");
}

// The VBlank period must follow the video mode the game selects (Vagrant Story Europe is PAL).
void test_vblank_period_follows_video_mode() {
    PsxRuntime rt;
    CpuContext ctx{};
    rt.write32(0x1F801814u, 0x08000009u);  // GP1(08h): 320 wide, PAL
    CHECK(rt.gpu().is_pal(), "GP1(08h) bit 3 selects PAL");
    rt.tick_hardware(ctx, 600'000ull);
    CHECK(rt.vblank_count() == 0u, "PAL: no VBlank after 600k cycles (period 677,376)");
    rt.tick_hardware(ctx, 100'000ull);
    CHECK(rt.vblank_count() == 1u, "PAL: first VBlank after 677,376 cycles");

    PsxRuntime ntsc;
    CpuContext nctx{};
    ntsc.write32(0x1F801814u, 0x08000001u); // GP1(08h): 320 wide, NTSC
    ntsc.tick_hardware(nctx, 600'000ull);
    CHECK(ntsc.vblank_count() == 1u, "NTSC: VBlank after 564,480 cycles");
}

// SIO0 digital pad read the way Vagrant Story's driver does it: after each TX byte it clears
// IRQ7, waits for /ACK (SIO_STAT bit 7) to read 0, then waits for IRQ7 from the delayed ACK pulse.
void test_sio_pad_read_with_delayed_ack() {
    PsxRuntime rt;
    CpuContext ctx{};
    rt.set_pad_buttons(static_cast<u16>(~(vsport::pad::CROSS | vsport::pad::START)));
    rt.write16(0x1F80104Au, 0x1003u); // TX enable, /JOY1 select, ACK IRQ enable

    const auto exchange = [&](u8 tx, bool expect_ack) -> u8 {
        rt.write8(0x1F801040u, tx);
        rt.write32(0x1F801070u, ~(1u << 7));                   // Driver clears IRQ7 after TX
        CHECK((rt.read16(0x1F801044u) & 0x0080u) == 0u, "/ACK not asserted right after TX");
        rt.tick_hardware(ctx, 2000ull);                        // Byte transfer + pad response
        const bool irq7 = (rt.read32(0x1F801070u) & (1u << 7)) != 0;
        CHECK(irq7 == expect_ack, "IRQ7 follows the pad's delayed /ACK");
        if (expect_ack) {
            CHECK((rt.read16(0x1F801044u) & 0x0200u) != 0u, "SIO_STAT bit 9 (IRQ) set on /ACK");
            rt.write16(0x1F80104Au, 0x1013u);                  // Acknowledge SIO IRQ
            rt.tick_hardware(ctx, 500ull);                     // /ACK pulse ends
            CHECK((rt.read16(0x1F801044u) & 0x0080u) == 0u, "/ACK pulse released");
        }
        return rt.read8(0x1F801040u);
    };

    CHECK(exchange(0x01u, true) == 0xFFu, "Pad select byte");
    CHECK(exchange(0x42u, true) == 0x41u, "Digital pad ID low (0x41)");
    CHECK(exchange(0x00u, true) == 0x5Au, "Pad ready marker 0x5A");
    const u8 lo = exchange(0x00u, true);
    const u8 hi = exchange(0x00u, false);                       // Last byte has no /ACK
    const u16 buttons = static_cast<u16>(lo | (hi << 8));
    CHECK(buttons == static_cast<u16>(~(vsport::pad::CROSS | vsport::pad::START)), "Pad returns the host button state");
}

// Redump .sbi subchannel overrides (LibCrypt): altered Q data replaces the regular position
void test_sbi_subchannel_overrides() {
    const auto path = std::filesystem::temp_directory_path() / "vsport_test.sbi";
    {
        const u8 bytes[] = {
            'S', 'B', 'I', 0,
            // 03:14:29 type 1: full altered Q (relative 03:12:09, absolute 03:14:2D)
            0x03, 0x14, 0x29, 0x01, 0x41, 0x01, 0x01, 0x03, 0x12, 0x09, 0x00, 0x03, 0x14, 0x2D,
            // 03:14:34 type 3: only the absolute MSF is altered
            0x03, 0x14, 0x34, 0x03, 0x03, 0x04, 0x34,
        };
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }

    DiscImage disc;
    std::string err;
    CHECK(disc.load_sbi(path, &err), "SBI loads");
    CHECK(disc.subq_override_count() == 2u, "SBI has 2 sectors");

    const u32 lba1 = (3u * 60u + 14u) * 75u + 29u - 150u;
    const auto q1 = disc.subchannel_q(lba1);
    CHECK(q1[3] == 0x03 && q1[4] == 0x12 && q1[5] == 0x09 && q1[9] == 0x2D, "Type 1 record replaces the whole Q");

    const u32 lba2 = (3u * 60u + 14u) * 75u + 34u - 150u;
    const auto q2 = disc.subchannel_q(lba2);
    CHECK(q2[7] == 0x03 && q2[8] == 0x04 && q2[9] == 0x34, "Type 3 record replaces the absolute MSF");
    CHECK(q2[3] == 0x03 && q2[4] == 0x12 && q2[5] == 0x34, "Type 3 record keeps the regular relative MSF");

    const auto q_plain = disc.subchannel_q(lba1 + 1u); // 03:14:30 absolute -> relative 03:12:30
    CHECK(q_plain[0] == 0x41 && q_plain[3] == 0x03 && q_plain[4] == 0x12 && q_plain[5] == 0x30 &&
              q_plain[7] == 0x03 && q_plain[8] == 0x14 && q_plain[9] == 0x30,
          "Sectors without an SBI record report their regular position");

    std::filesystem::remove(path);
    std::ofstream bad(path, std::ios::binary);
    bad << "NOPE";
    bad.close();
    CHECK(!disc.load_sbi(path, &err), "Files without the SBI signature are rejected");
    std::filesystem::remove(path);
}

// Memory card image: format, files across blocks, erase/rename, and persistence on disk
void test_memcard_filesystem() {
    const auto path = std::filesystem::temp_directory_path() / "vsport_test_card.mcr";
    std::filesystem::remove(path);
    {
        MemoryCard card;
        std::string err;
        CHECK(card.open(path, &err), "New card image is created");
        CHECK(std::filesystem::file_size(path) == MemoryCard::CARD_SIZE, "Card image is 128 KB");
        CHECK(card.data()[0] == 'M' && card.data()[1] == 'C', "Fresh card is formatted (MC header)");
        CHECK(card.used_blocks() == 0u, "Fresh card has no saves");

        MemoryCard::FileInfo a, b, again;
        CHECK(card.create_file("BESLES-02754VAG00", 1, a) && a.first_block == 1u && a.size == 8192u, "Create 1-block file");
        CHECK(!card.create_file("BESLES-02754VAG00", 1, again), "Creating an existing name fails");
        CHECK(card.create_file("BESLES-02754VAG01", 2, b) && b.size == 16384u, "Create 2-block file");
        CHECK(card.used_blocks() == 3u, "Three blocks in use");

        // Data crossing the block boundary of the 2-block file
        u8 out[16]{}, in[16];
        for (u8 i = 0; i < 16; ++i) out[i] = static_cast<u8>(0xA0 + i);
        CHECK(card.write_file(b, 8184, out, 16) == 16u, "Write across a block boundary");
        CHECK(card.read_file(b, 8184, in, 16) == 16u && std::memcmp(in, out, 16) == 0, "Read back across the boundary");
        CHECK(card.read_file(b, 16380, in, 16) == 4u, "Reads stop at the end of the file");

        MemoryCard::FileInfo f;
        CHECK(card.find_file("BESLES-02754VAG*", 1, f) && f.name == "BESLES-02754VAG00", "Wildcard finds the first save");
        CHECK(card.find_file("BESLES-02754VAG*", f.first_block + 1, f) && f.name == "BESLES-02754VAG01", "...and the next one");
        CHECK(card.erase_file("BESLES-02754VAG00") && !card.find_file("BESLES-02754VAG00", 1, f), "Erase removes the file");
        CHECK(card.rename_file("BESLES-02754VAG01", "BESLES-02754VAG09"), "Rename");
        CHECK(card.flush(&err), "Flush to disk");
    }
    {
        MemoryCard card;
        CHECK(card.open(path), "Reopen the card image");
        MemoryCard::FileInfo f;
        u8 in[16];
        CHECK(card.find_file("BESLES-02754VAG09", 1, f) && card.read_file(f, 8184, in, 16) == 16u && in[0] == 0xA0 && in[15] == 0xAF,
              "Saved data survives on disk");
        CHECK(card.used_blocks() == 2u, "Erased blocks are free again");
    }
    std::filesystem::remove(path);
}

// The game's save code through the BIOS: libapi's device table check, then open/write/read/
// close, firstfile and erase on "bu00:"
void test_bios_memcard_file_calls() {
    const auto path = std::filesystem::temp_directory_path() / "vsport_test_card_bios.mcr";
    std::filesystem::remove(path);
    PsxRuntime rt;
    CHECK(rt.attach_memory_card(0, path), "Attach card 1");

    // libapi looks for the "bu" device in the kernel DCB table (pointer at 0x150, size at 0x154)
    const u32 dcb = rt.read32(0x80000150u), dcb_size = rt.read32(0x80000154u);
    bool has_bu = false;
    for (u32 off = 0; off < dcb_size; off += 0x50u) {
        const u32 name = rt.read32(dcb + off);
        has_bu = has_bu || (rt.read8(name) == 'b' && rt.read8(name + 1) == 'u' && rt.read8(name + 2) == 0);
    }
    CHECK(dcb != 0 && has_bu, "Kernel DCB table lists the memory card device");

    CpuContext ctx{};
    const auto bios = [&](u32 vec, u32 fn, u32 a0, u32 a1 = 0, u32 a2 = 0) {
        ctx.set_reg(reg::T1, fn);
        ctx.set_reg(reg::A0, a0);
        ctx.set_reg(reg::A1, a1);
        ctx.set_reg(reg::A2, a2);
        ctx.set_reg(reg::RA, 0x80010000u);
        rt.handle_bios_vector(ctx, vec);
        return ctx.get_reg(reg::V0);
    };
    const auto put_str = [&](u32 addr, const char* s) {
        for (u32 i = 0;; ++i) { rt.write8(addr + i, static_cast<u8>(s[i])); if (!s[i]) break; }
    };
    put_str(0x80100000u, "bu00:BESLES-02754VAG00");
    put_str(0x80100040u, "bu00:BESLES-02754*");
    for (u32 i = 0; i < 256; ++i) rt.write8(0x80101000u + i, static_cast<u8>(i));

    u32 fd = bios(0xB0, 0x32, 0x80100000u, 0x200u | (1u << 16)); // open(FCREAT, 1 block)
    CHECK(fd >= 2u && fd != 0xFFFFFFFFu, "open(FCREAT) creates the save file");
    bios(0xB0, 0x36, fd);
    CHECK(bios(0xB0, 0x32, 0x80100000u, 0x200u | (1u << 16)) == 0xFFFFFFFFu, "Creating an existing save fails");

    fd = bios(0xB0, 0x32, 0x80100000u, 2u); // open(FWRITE)
    CHECK(bios(0xB0, 0x35, fd, 0x80101000u, 256u) == 256u, "write() 256 bytes");
    bios(0xB0, 0x36, fd);
    fd = bios(0xB0, 0x32, 0x80100000u, 1u); // open(FREAD)
    CHECK(bios(0xB0, 0x34, fd, 0x80102000u, 256u) == 256u, "read() 256 bytes");
    bios(0xB0, 0x36, fd);
    bool same = true;
    for (u32 i = 0; i < 256; ++i) same = same && rt.read8(0x80102000u + i) == static_cast<u8>(i);
    CHECK(same, "Data read back matches what was written");

    CHECK(bios(0xB0, 0x42, 0x80100040u, 0x80103000u) == 0x80103000u && rt.read8(0x80103000u) == 'B' &&
              rt.read32(0x80103018u) == 8192u,
          "firstfile() returns the save with its size");
    CHECK(bios(0xB0, 0x43, 0x80103000u) == 0u, "nextfile() ends the listing");
    CHECK(bios(0xB0, 0x45, 0x80100000u) == 1u && bios(0xB0, 0x42, 0x80100040u, 0x80103000u) == 0u, "erase() deletes the save");

    rt.write8(0x80100002u, '1'); // "bu10:" = slot 2, no card inserted
    CHECK(bios(0xB0, 0x32, 0x80100000u, 1u) == 0xFFFFFFFFu, "open() on an empty slot fails");
    rt.flush_memory_cards();
    std::filesystem::remove(path);
}

void test_gamepad_mapping() {
    using namespace vsport::pad;

    // XInput: face buttons by position, shoulders, triggers, Start/Back
    CHECK(map_xinput(xi::A, 0, 0, 0, 0) == CROSS, "XInput A -> Cross");
    CHECK(map_xinput(xi::B | xi::X | xi::Y, 0, 0, 0, 0) == (CIRCLE | SQUARE | TRIANGLE), "XInput B/X/Y -> Circle/Square/Triangle");
    CHECK(map_xinput(xi::LEFT_SHOULDER | xi::RIGHT_SHOULDER, 0, 0, 0, 0) == (L1 | R1), "XInput LB/RB -> L1/R1");
    CHECK(map_xinput(0, 255, 63, 0, 0) == L2, "XInput triggers use a threshold (LT pressed, RT light touch ignored)");
    CHECK(map_xinput(xi::START | xi::BACK, 0, 0, 0, 0) == (START | SELECT), "XInput Start/Back -> Start/Select");
    CHECK(map_xinput(xi::DPAD_UP | xi::DPAD_LEFT, 0, 0, 0, 0) == (UP | LEFT), "XInput D-pad");
    // Left stick acts as D-pad with a deadzone; XInput +Y is up
    CHECK(map_xinput(0, 0, 0, 0, 30000) == UP, "XInput stick up -> Up");
    CHECK(map_xinput(0, 0, 0, -30000, -30000) == (LEFT | DOWN), "XInput stick down-left -> Down+Left");
    CHECK(map_xinput(0, 0, 0, 8000, -8000) == 0, "XInput stick inside deadzone is ignored");

    // DirectInput POV hat (hundredths of a degree, clockwise from up)
    CHECK(map_pov_hat(0xFFFFFFFFu) == 0, "POV centered");
    CHECK(map_pov_hat(0) == UP, "POV 0 -> Up");
    CHECK(map_pov_hat(4500) == (UP | RIGHT), "POV 45 -> Up+Right");
    CHECK(map_pov_hat(18000) == DOWN, "POV 180 -> Down");
    CHECK(map_pov_hat(27000) == LEFT, "POV 270 -> Left");

    // DirectInput DualShock 4 / DualSense button order
    u8 buttons[128]{};
    buttons[1] = 0x80; // Cross
    buttons[9] = 0x80; // Options -> Start
    buttons[6] = 0x80; // L2
    CHECK(map_dinput(0xFFFFFFFFu, 0, 0, buttons, sizeof(buttons)) == (CROSS | START | L2), "DInput DS4 order: Cross/Options/L2");
    u8 none[128]{};
    CHECK(map_dinput(0xFFFFFFFFu, 0, -900, none, sizeof(none)) == UP, "DInput stick: -Y is up");
    CHECK(map_dinput(0xFFFFFFFFu, 300, 300, none, sizeof(none)) == 0, "DInput stick inside deadzone is ignored");

    // Button remapping
    const ButtonMap def = default_button_map();
    CHECK(apply_button_map(CROSS | L2 | UP, def) == (CROSS | L2 | UP), "Default button map is the identity");
    ButtonMap swap = def;
    swap[0] = 1; // PS1 Cross   <- physical Circle position
    swap[1] = 0; // PS1 Circle  <- physical Cross position
    CHECK(apply_button_map(CROSS, swap) == CIRCLE, "Swapped map: bottom button presses Circle");
    CHECK(apply_button_map(CIRCLE | LEFT, swap) == (CROSS | LEFT), "Swapped map: right button presses Cross, D-pad unchanged");
    ButtonMap dup = def;
    dup[8] = 4; // START <- L1 as well (START's own button now unassigned)
    CHECK(apply_button_map(L1, dup) == (L1 | START), "One physical button can press two PS1 buttons");
    CHECK(apply_button_map(START, dup) == 0, "A physical button with no PS1 button does nothing");
    ButtonMap off = def;
    off[3] = -1;
    CHECK(apply_button_map(TRIANGLE, off) == 0, "Unassigned PS1 button is never pressed");
    CHECK(remap_index_from_name("r2") == 7 && remap_index_from_name("none") == -1 &&
              remap_index_from_name("bogus") == -2, "Remap names round-trip");
}

} // namespace

int main() {
    test_address_mapping_and_scratchpad();
    test_unaligned_lwl_lwr_swl_swr();
    test_mips_and_gte_decoder();
    test_gte_rtps_widescreen_and_nclip();
    test_gpu_upscaled_queue_sees_old_textures();
    test_gpu_upscaled_constant_uv_has_no_holes();
    test_gpu_rim_light_modes();
    test_gpu_mask_bit_and_self_texturing_sprite();
    test_gpu_exact_vertex_positions();
    test_gpu_perspective_textures();
    test_save_state_round_trip();
    test_gpu_display_off_is_black();
    test_gpu_otc_and_dma_linked_list();
    test_interpreter_and_lifter_pipeline();
    test_lifter_labeled_delay_slot_runs_once();
    test_vs_overlay_catalog();
    test_submenu_overlay_signature_switching();
    test_spu_dma4_adpcm_and_adsr();
    test_mdec_dma0_dma1_rgb24_and_rgb15();
    test_dicr_byte_mmio_masking_and_ack();
    test_gpu_texture_window_and_fill_rule();
    test_vblank_period_follows_video_mode();
    test_gamepad_mapping();
    test_sio_pad_read_with_delayed_ack();
    test_sbi_subchannel_overrides();
    test_memcard_filesystem();
    test_bios_memcard_file_calls();

    if (g_failures == 0) {
        std::cout << "[PASS] Tum vsport cekirdek testleri basariyla gecti!\n";
        return 0;
    }
    std::cerr << "[FAIL] Toplam basarisiz test: " << g_failures << "\n";
    return 1;
}
