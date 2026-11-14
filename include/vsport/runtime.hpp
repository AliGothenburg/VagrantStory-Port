#pragma once

#include "vsport/cdrom_iso.hpp"
#include "vsport/gte.hpp"
#include "vsport/memcard.hpp"
#include "vsport/psx_cdrom.hpp"
#include "vsport/psx_exe.hpp"
#include "vsport/psx_gpu.hpp"
#include "vsport/psx_mdec.hpp"
#include "vsport/psx_spu.hpp"
#include "vsport/types.hpp"

#include <array>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace vsport {

class PsxRuntime;
using RecompFunc = void (*)(CpuContext& ctx, PsxRuntime& rt);

class PsxRuntime {
public:
    explicit PsxRuntime(u32 ram_size = PSX_RAM_SIZE_2MB);

    void reset();

    // Load a parsed PS-X EXE into RAM and initialize CPU registers (pc, gp, sp)
    void load_exe(const PsxExeImage& exe, CpuContext& ctx);

    // Load a raw overlay (.PRG) or binary blob directly at a KSEG0 address
    void load_overlay(u32 kseg0_addr, std::span<const u8> data);

    // Attach DiscImage for CD-ROM controller sector streaming
    void attach_disc(DiscImage* disc) noexcept {
        disc_ = disc;
        cdrom_.attach_disc(disc);
    }

    // Memory Bus Access (handles KUSEG/KSEG0/KSEG1, RAM, Scratchpad, MMIO, BIOS)
    [[nodiscard]] u8  read8(u32 vaddr);
    [[nodiscard]] u16 read16(u32 vaddr);
    [[nodiscard]] u32 read32(u32 vaddr);

    void write8(u32 vaddr, u8 val);
    void write16(u32 vaddr, u16 val);
    void write32(u32 vaddr, u32 val);

    // Unaligned MIPS Load/Store Helpers
    [[nodiscard]] u32 exec_lwl(u32 vaddr, u32 cur_rt);
    [[nodiscard]] u32 exec_lwr(u32 vaddr, u32 cur_rt);
    void exec_swl(u32 vaddr, u32 cur_rt);
    void exec_swr(u32 vaddr, u32 cur_rt);

    // Hardware Subsystems
    [[nodiscard]] GteEngine& gte() noexcept { return gte_; }
    [[nodiscard]] const GteEngine& gte() const noexcept { return gte_; }
    [[nodiscard]] PsxGpu& gpu() noexcept { return gpu_; }
    [[nodiscard]] PgxpCache& pgxp() noexcept { return pgxp_; }
    [[nodiscard]] const PsxGpu& gpu() const noexcept { return gpu_; }
    [[nodiscard]] PsxCdrom& cdrom() noexcept { return cdrom_; }
    [[nodiscard]] const PsxCdrom& cdrom() const noexcept { return cdrom_; }
    [[nodiscard]] PsxMdec& mdec() noexcept { return mdec_; }
    [[nodiscard]] const PsxMdec& mdec() const noexcept { return mdec_; }
    [[nodiscard]] PsxSpu& spu() noexcept { return spu_; }
    [[nodiscard]] const PsxSpu& spu() const noexcept { return spu_; }

    // Memory cards (slot 0 = card 1, slot 1 = card 2). Saved to the image file shortly after the
    // game stops writing, and when the runtime is destroyed.
    bool attach_memory_card(u32 slot, const std::filesystem::path& path, std::string* err = nullptr);
    [[nodiscard]] MemoryCard& memory_card(u32 slot) noexcept { return memcards_[slot & 1u]; }
    void flush_memory_cards();
    ~PsxRuntime();
    PsxRuntime(const PsxRuntime&)            = delete;
    PsxRuntime& operator=(const PsxRuntime&) = delete;

    // Digital Controller State (16-bit active-low PS1 pad buttons: 0xFFFF = no buttons pressed)
    void set_pad_buttons(u16 buttons_active_low) noexcept { pad_buttons_ = buttons_active_low; }

    // BIOS HLE & Syscall Handling
    bool handle_bios_vector(CpuContext& ctx, u32 masked_pc);
    void handle_syscall(CpuContext& ctx);
    [[nodiscard]] const std::string& tty_output() const noexcept { return tty_buf_; }
    void clear_tty() noexcept { tty_buf_.clear(); }

    // Periodic Hardware Tick (Timers, VBlank IRQ0, CD-ROM IRQ2, HookEntryInt dispatch)
    void tick_hardware(CpuContext& ctx, u64 elapsed_cycles);
    void poll_hardware(CpuContext& ctx) {
        if ((ctx.cycles - last_hw_cycle_) >= 64ull) {
            const u64 delta = ctx.cycles - last_hw_cycle_;
            last_hw_cycle_  = ctx.cycles;
            tick_hardware(ctx, delta);
        }
    }

    // AOT Unwind State (for ReturnFromException inside ISRs, longjmp, and cycle budgets)
    [[nodiscard]] bool in_interrupt() const noexcept { return in_interrupt_; }
    [[nodiscard]] u64 longjmp_token() const noexcept { return longjmp_token_; }
    [[nodiscard]] bool should_unwind(const CpuContext& ctx, bool entered_in_irq, u64 entry_lj) const noexcept {
        if (ctx.halted || (entered_in_irq && !in_interrupt_) || (longjmp_token_ != entry_lj)) {
            return true;
        }
        if (cycle_limit_ != 0) {
            const u64 hard_limit = entered_in_irq ? (cycle_limit_ + 5'000'000ull) : cycle_limit_;
            if (ctx.cycles >= hard_limit) {
                return true;
            }
        }
        return false;
    }

    // Optional host VBlank callback (pumps Win32 window & presents 60fps frames even inside AOT game loops)
    void set_vblank_callback(std::function<void()> cb) { on_vblank_ = std::move(cb); }

    // Hybrid AOT Recompilation + Overlay-Aware Signature Dispatch + Fallback Interpreter
    void register_func(u32 kseg0_addr, RecompFunc fn);
    void register_func(u32 kseg0_addr, u32 byte_len, u64 fnv1a_hash, RecompFunc fn);
    [[nodiscard]] bool has_recompiled_func(u32 vaddr) const noexcept;
    void call_function(CpuContext& ctx, u32 target_pc);

    // Executes a single MIPS instruction in interpreter mode
    void step_interpreter(CpuContext& ctx);

    // Runs up to max_steps instructions
    u64 run(CpuContext& ctx, u64 max_steps);

    [[nodiscard]] const std::set<u32>& discovered_call_targets() const noexcept { return discovered_calls_; }
    [[nodiscard]] std::span<const u8> ram_span() const noexcept { return ram_; }
    [[nodiscard]] u32 vblank_count() const noexcept { return vblank_frames_; }
    [[nodiscard]] u64 aot_dispatch_count() const noexcept { return aot_dispatches_; }

private:
    struct AotEntry {
        u32 byte_len      = 0; // 0 = unconditional hook (unit tests / HLE)
        u64 fnv1a_hash    = 0;
        RecompFunc fn     = nullptr;
        u32 cached_epoch  = 0;
        bool cached_valid = false;
    };

    RecompFunc find_aot_func(u32 canonical_addr);
    void bump_ram_range_epoch(u32 paddr, std::size_t len) noexcept;
    u8  read_mmio8(u32 paddr);
    u16 read_mmio16(u32 paddr);
    u32 read_mmio32(u32 paddr);
    void write_mmio8(u32 paddr, u8 val);
    void write_mmio16(u32 paddr, u16 val);
    void write_mmio32(u32 paddr, u32 val);

    void sio_pad_ack() noexcept;
    void tick_sio(u64 elapsed_cycles) noexcept;
    void execute_dma(u32 channel);
    void pump_mdec_out_dma();
    void update_dicr_irq();
    void deliver_kernel_event(u32 ev_class, u32 ev_spec);
    void dispatch_bios_irq_events(CpuContext& ctx, u32 ev_class, u32 ev_spec);
    void check_and_enter_interrupt(CpuContext& ctx);
    void restore_interrupted_context(CpuContext& ctx) noexcept;
    std::string read_cstr(u32 vaddr, u32 max_len = 512);
    std::string format_bios_printf(CpuContext& ctx);

    struct DmaChannel {
        u32 madr = 0;
        u32 bcr  = 0;
        u32 chcr = 0;
    };

    struct TimerState {
        u16 counter = 0;
        u16 mode    = 0x0400u;
        u16 target  = 0xFFFFu;
    };

    struct KernelEvent {
        bool allocated = false;
        bool enabled   = false;
        bool delivered = false;
        u32 class_id   = 0;
        u32 spec       = 0;
        u32 mode       = 0;
        u32 func_addr  = 0;
    };

    std::vector<u8> ram_;
    std::array<u8, PSX_SCRATCHPAD_SIZE> scratchpad_{};
    std::vector<u8> bios_;

    PgxpCache pgxp_; // Exact vertex positions (GTE -> GPU), upscaled picture only
    GteEngine gte_;
    PsxGpu gpu_;
    PsxCdrom cdrom_;
    PsxMdec mdec_;
    PsxSpu spu_;
    DiscImage* disc_ = nullptr;

    std::array<DmaChannel, 7> dma_{};
    bool dma1_transfer_done_ = false;
    u64 dma1_busy_cycles_    = 0;
    std::array<TimerState, 3> timers_{};

    // SIO0 Joypad state machine
    u16 pad_buttons_    = 0xFFFFu;
    u16 sio_stat_       = 0x0005u;
    u16 sio_mode_       = 0;
    u16 sio_ctrl_       = 0;
    u16 sio_baud_       = 0x0088u;
    u8  sio_rx_byte_    = 0xFFu;
    u8  sio_step_       = 0;
    bool sio_selected_  = false;
    bool sio_is_memcard_ = false;
    // /ACK timing: delay from TX write to the pad's ACK pulse (~8 bits at SIO_BAUD 0x88 plus the
    // pad's response time), and the pulse width
    static constexpr u32 SIO_ACK_DELAY_CYCLES = 1200;
    static constexpr u32 SIO_ACK_PULSE_CYCLES = 100;
    u32 sio_ack_delay_  = 0;
    u32 sio_ack_pulse_  = 0;

    u32 dpcr_   = 0x07654321u;
    u32 dicr_   = 0;
    u32 i_stat_ = 0;
    u32 i_mask_ = 0;

    // Hardware timing accumulators
    u64 last_hw_cycle_ = 0;
    u64 vblank_accum_  = 0;
    u64 hblank_accum_  = 0;
    u64 sysclk8_accum_ = 0;
    u32 vblank_frames_ = 0;

    // Kernel HLE interrupt & event state
    u32 hook_entry_int_addr_ = 0;
    bool in_interrupt_       = false;
    u64 longjmp_token_       = 0;
    CpuContext saved_irq_ctx_{};
    std::array<u32, 4> int_rp_heads_{};
    std::array<u32, 4> clear_rcnt_flags_{};
    u32 clear_pad_flag_ = 0;
    std::array<KernelEvent, 64> events_{};

    // BIOS memory card HLE (_card_info / _card_read / _card_write ...) and the "bu" file device
    bool bios_card_call(CpuContext& ctx, u32 table, u32 fn);
    bool bios_file_call(CpuContext& ctx, u32 table, u32 fn);
    void setup_kernel_tables();
    std::array<MemoryCard, 2> memcards_{};
    u32 card_chan_               = 0;
    u32 memcard_last_write_vb_   = 0;

    struct OpenFile {
        bool used  = false;
        u32 slot   = 0;
        u32 mode   = 0;
        u32 pos    = 0;
        MemoryCard::FileInfo info;
    };
    std::array<OpenFile, 16> open_files_{}; // fd = index + 2 (0/1 are the TTY)
    std::string find_pattern_;              // firstfile()/nextfile() search state
    u32 find_slot_       = 0;
    u32 find_next_block_ = 16;
    u32 rand_seed_ = 1;

    u32 heap_base_ = 0x80180000u;
    u32 heap_end_  = 0x801F0000u;
    u32 heap_cur_  = 0x80180000u;

    std::string tty_buf_;
    u64 cycle_limit_ = 0;
    std::function<void()> on_vblank_;
    std::array<u32, 2048> ram_page_epoch_{};
    std::unordered_map<u32, std::vector<AotEntry>> aot_funcs_;
    u64 aot_dispatches_      = 0;
    u32 pending_aot_call_pc_ = 0;
    u32 saved_aot_pc_        = 0;
    std::set<u32> discovered_calls_;
};

} // namespace vsport
