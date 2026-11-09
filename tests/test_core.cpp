#include "vsport/cdrom_iso.hpp"
#include "vsport/cfg_lifter.hpp"
#include "vsport/gte.hpp"
#include "vsport/mips_decoder.hpp"
#include "vsport/psx_exe.hpp"
#include "vsport/psx_gpu.hpp"
#include "vsport/runtime.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
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

} // namespace

int main() {
    test_address_mapping_and_scratchpad();
    test_unaligned_lwl_lwr_swl_swr();
    test_mips_and_gte_decoder();
    test_gte_rtps_widescreen_and_nclip();
    test_gpu_otc_and_dma_linked_list();
    test_interpreter_and_lifter_pipeline();
    test_lifter_labeled_delay_slot_runs_once();
    test_vs_overlay_catalog();
    test_submenu_overlay_signature_switching();
    test_spu_dma4_adpcm_and_adsr();
    test_mdec_dma0_dma1_rgb24_and_rgb15();
    test_dicr_byte_mmio_masking_and_ack();

    if (g_failures == 0) {
        std::cout << "[PASS] Tum vsport cekirdek testleri basariyla gecti!\n";
        return 0;
    }
    std::cerr << "[FAIL] Toplam basarisiz test: " << g_failures << "\n";
    return 1;
}
