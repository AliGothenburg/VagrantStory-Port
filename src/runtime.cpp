#include "vsport/runtime.hpp"
#include "vsport/mips_decoder.hpp"

#include <cstdio>
#include <cstring>

namespace vsport {

namespace {

u16 load_le16(const u8* p) noexcept {
    return static_cast<u16>(p[0]) | static_cast<u16>(static_cast<u16>(p[1]) << 8);
}

u32 load_le32(const u8* p) noexcept {
    return static_cast<u32>(p[0]) |
           (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

void store_le16(u8* p, u16 v) noexcept {
    p[0] = static_cast<u8>(v & 0xFFu);
    p[1] = static_cast<u8>((v >> 8) & 0xFFu);
}

void store_le32(u8* p, u32 v) noexcept {
    p[0] = static_cast<u8>(v & 0xFFu);
    p[1] = static_cast<u8>((v >> 8) & 0xFFu);
    p[2] = static_cast<u8>((v >> 16) & 0xFFu);
    p[3] = static_cast<u8>((v >> 24) & 0xFFu);
}

} // namespace

PsxRuntime::PsxRuntime(u32 ram_size)
    : ram_(ram_size, 0), bios_(PSX_BIOS_SIZE, 0) {
    reset();
}

void PsxRuntime::reset() {
    std::fill(ram_.begin(), ram_.end(), static_cast<u8>(0));
    scratchpad_.fill(0);
    std::fill(bios_.begin(), bios_.end(), static_cast<u8>(0));
    gte_.reset();
    gpu_.reset();
    cdrom_.reset();
    cdrom_.attach_disc(disc_);
    cdrom_.attach_spu(&spu_);

    dma_.fill(DmaChannel{});
    dma1_transfer_done_ = false;
    dma1_busy_cycles_   = 0;
    timers_.fill(TimerState{});
    mdec_.reset();
    spu_.reset();

    pad_buttons_     = 0xFFFFu;
    sio_stat_        = 0x0005u;
    sio_mode_        = 0;
    sio_ctrl_        = 0;
    sio_baud_        = 0x0088u;
    sio_rx_byte_     = 0xFFu;
    sio_step_        = 0;
    sio_selected_    = false;
    sio_is_memcard_  = false;

    dpcr_   = 0x07654321u;
    dicr_   = 0;
    i_stat_ = 0;
    i_mask_ = 0;

    last_hw_cycle_ = 0;
    vblank_accum_  = 0;
    hblank_accum_  = 0;
    sysclk8_accum_ = 0;
    vblank_frames_ = 0;

    hook_entry_int_addr_ = 0;
    in_interrupt_        = false;
    longjmp_token_       = 0;
    saved_irq_ctx_       = CpuContext{};
    int_rp_heads_.fill(0);
    clear_rcnt_flags_.fill(1u); // BIOS auto-clears Root Counters 0..3 until ChangeClearRCnt(t, 0)
    clear_pad_flag_ = 1u;
    events_.fill(KernelEvent{});
    rand_seed_ = 1;

    heap_base_ = 0x80180000u;
    heap_cur_  = 0x80180000u;
    heap_end_  = 0x801F0000u;
    tty_buf_.clear();
    cycle_limit_         = 0;
    aot_dispatches_      = 0;
    pending_aot_call_pc_ = 0;
    for (u32& ep : ram_page_epoch_) {
        ++ep;
        if (ep == 0) ep = 1u;
    }

    for (u32 vec : {0x00A0u, 0x00B0u, 0x00C0u}) {
        store_le32(&ram_[vec], 0x03E00008u);     // jr $ra
        store_le32(&ram_[vec + 4], 0x00000000u); // nop
    }
}

void PsxRuntime::bump_ram_range_epoch(u32 paddr, std::size_t len) noexcept {
    if (len == 0 || paddr >= ram_.size()) return;
    const std::size_t end_off = std::min<std::size_t>(ram_.size() - 1u, paddr + len - 1u);
    const u32 p0 = paddr >> 12;
    const u32 p1 = static_cast<u32>(end_off >> 12);
    for (u32 p = p0; p <= p1 && p < ram_page_epoch_.size(); ++p) {
        if (++ram_page_epoch_[p] == 0) {
            ram_page_epoch_[p] = 1u;
        }
    }
}

void PsxRuntime::load_exe(const PsxExeImage& exe, CpuContext& ctx) {
    const u32 paddr = mask_psx_addr(exe.header.t_addr);
    if (paddr < ram_.size()) {
        const std::size_t max_copy = std::min<std::size_t>(exe.payload.size(), ram_.size() - paddr);
        std::memcpy(&ram_[paddr], exe.payload.data(), max_copy);
        bump_ram_range_epoch(paddr, max_copy);
    }

    if (exe.header.b_addr != 0 && exe.header.b_size != 0) {
        const u32 bp = mask_psx_addr(exe.header.b_addr);
        if (bp < ram_.size()) {
            const std::size_t max_zero = std::min<std::size_t>(exe.header.b_size, ram_.size() - bp);
            std::memset(&ram_[bp], 0, max_zero);
            bump_ram_range_epoch(bp, max_zero);
        }
    }

    ctx = CpuContext{};
    ctx.pc       = exe.header.pc0;
    ctx.next_pc  = exe.header.pc0 + 4u;
    pending_aot_call_pc_ = to_kseg0(exe.header.pc0);
    ctx.cop0[12] = 0x40000401u; // COP2 enabled + IM2 hardware IRQ enabled + IEc enabled
    ctx.set_reg(reg::GP, exe.header.gp0);

    const u32 sp_val = (exe.header.s_addr != 0) ? (exe.header.s_addr + exe.header.s_size) : 0x801FFF00u;
    ctx.set_reg(reg::SP, sp_val);
    ctx.set_reg(reg::FP, sp_val);
    ctx.set_reg(reg::RA, 0x80000000u);
    store_le32(&ram_[0], 0x0000000Du); // BREAK sentinel at 0x80000000
    bump_ram_range_epoch(0, 4);
}

void PsxRuntime::load_overlay(u32 kseg0_addr, std::span<const u8> data) {
    const u32 paddr = mask_psx_addr(kseg0_addr);
    if (paddr < ram_.size()) {
        const std::size_t max_copy = std::min<std::size_t>(data.size(), ram_.size() - paddr);
        std::memcpy(&ram_[paddr], data.data(), max_copy);
        bump_ram_range_epoch(paddr, max_copy);
    }
}

u8 PsxRuntime::read8(u32 vaddr) {
    const u32 paddr = mask_psx_addr(vaddr);
    if (paddr < ram_.size()) {
        return ram_[paddr];
    }
    if (paddr >= PSX_SCRATCHPAD_PHYS && paddr < PSX_SCRATCHPAD_PHYS + PSX_SCRATCHPAD_SIZE) {
        return scratchpad_[paddr - PSX_SCRATCHPAD_PHYS];
    }
    if (paddr >= PSX_BIOS_PHYS_START && paddr < PSX_BIOS_PHYS_START + PSX_BIOS_SIZE) {
        return bios_[paddr - PSX_BIOS_PHYS_START];
    }
    if (paddr >= PSX_MMIO_PHYS_START && paddr <= PSX_MMIO_PHYS_END) {
        return read_mmio8(paddr);
    }
    return 0;
}

u16 PsxRuntime::read16(u32 vaddr) {
    const u32 paddr = mask_psx_addr(vaddr);
    if (paddr + 1 < ram_.size()) {
        return load_le16(&ram_[paddr]);
    }
    if (paddr >= PSX_SCRATCHPAD_PHYS && paddr + 1 < PSX_SCRATCHPAD_PHYS + PSX_SCRATCHPAD_SIZE) {
        return load_le16(&scratchpad_[paddr - PSX_SCRATCHPAD_PHYS]);
    }
    if (paddr >= PSX_BIOS_PHYS_START && paddr + 1 < PSX_BIOS_PHYS_START + PSX_BIOS_SIZE) {
        return load_le16(&bios_[paddr - PSX_BIOS_PHYS_START]);
    }
    if (paddr >= PSX_MMIO_PHYS_START && paddr <= PSX_MMIO_PHYS_END) {
        return read_mmio16(paddr);
    }
    return 0;
}

struct TraceEntry { u32 pc, ra, sp; };
static TraceEntry g_trace_ring[64];
static u32 g_trace_pos = 0;
static TraceEntry g_watch[16];
static u32 g_watch_pos = 0;
static TraceEntry g_call_ring[64];
static u32 g_call_pos = 0;

u32 PsxRuntime::read32(u32 vaddr) {
    const u32 paddr = mask_psx_addr(vaddr);
    if (paddr + 3 < ram_.size()) {
        return load_le32(&ram_[paddr]);
    }
    if (paddr >= PSX_SCRATCHPAD_PHYS && paddr + 3 < PSX_SCRATCHPAD_PHYS + PSX_SCRATCHPAD_SIZE) {
        return load_le32(&scratchpad_[paddr - PSX_SCRATCHPAD_PHYS]);
    }
    if (paddr >= PSX_BIOS_PHYS_START && paddr + 3 < PSX_BIOS_PHYS_START + PSX_BIOS_SIZE) {
        return load_le32(&bios_[paddr - PSX_BIOS_PHYS_START]);
    }
    if (paddr >= PSX_MMIO_PHYS_START && paddr <= PSX_MMIO_PHYS_END) {
        return read_mmio32(paddr);
    }
    return 0;
}

void PsxRuntime::write8(u32 vaddr, u8 val) {
    const u32 paddr = mask_psx_addr(vaddr);
    if (paddr < ram_.size()) {
        ram_[paddr] = val;
        if (++ram_page_epoch_[paddr >> 12] == 0) ram_page_epoch_[paddr >> 12] = 1u;
        return;
    }
    if (paddr >= PSX_SCRATCHPAD_PHYS && paddr < PSX_SCRATCHPAD_PHYS + PSX_SCRATCHPAD_SIZE) {
        scratchpad_[paddr - PSX_SCRATCHPAD_PHYS] = val;
        return;
    }
    if (paddr >= PSX_MMIO_PHYS_START && paddr <= PSX_MMIO_PHYS_END) {
        write_mmio8(paddr, val);
    }
}

void PsxRuntime::write16(u32 vaddr, u16 val) {
    const u32 paddr = mask_psx_addr(vaddr);
    if (paddr + 1 < ram_.size()) {
        store_le16(&ram_[paddr], val);
        const u32 p0 = paddr >> 12, p1 = (paddr + 1u) >> 12;
        if (++ram_page_epoch_[p0] == 0) ram_page_epoch_[p0] = 1u;
        if (p1 != p0 && ++ram_page_epoch_[p1] == 0) ram_page_epoch_[p1] = 1u;
        return;
    }
    if (paddr >= PSX_SCRATCHPAD_PHYS && paddr + 1 < PSX_SCRATCHPAD_PHYS + PSX_SCRATCHPAD_SIZE) {
        store_le16(&scratchpad_[paddr - PSX_SCRATCHPAD_PHYS], val);
        return;
    }
    if (paddr >= PSX_MMIO_PHYS_START && paddr <= PSX_MMIO_PHYS_END) {
        write_mmio16(paddr, val);
    }
}

void PsxRuntime::write32(u32 vaddr, u32 val) {
    const u32 paddr = mask_psx_addr(vaddr);
    if (val == 2u && paddr >= 0x1FE000u && paddr < 0x200000u) { g_watch[g_watch_pos++ & 15u] = {paddr, g_trace_ring[(g_trace_pos - 1) & 63u].pc, 0}; }
    if (paddr + 3 < ram_.size()) {
        store_le32(&ram_[paddr], val);
        const u32 p0 = paddr >> 12, p1 = (paddr + 3u) >> 12;
        if (++ram_page_epoch_[p0] == 0) ram_page_epoch_[p0] = 1u;
        if (p1 != p0 && ++ram_page_epoch_[p1] == 0) ram_page_epoch_[p1] = 1u;
        return;
    }
    if (paddr >= PSX_SCRATCHPAD_PHYS && paddr + 3 < PSX_SCRATCHPAD_PHYS + PSX_SCRATCHPAD_SIZE) {
        store_le32(&scratchpad_[paddr - PSX_SCRATCHPAD_PHYS], val);
        return;
    }
    if (paddr >= PSX_MMIO_PHYS_START && paddr <= PSX_MMIO_PHYS_END) {
        write_mmio32(paddr, val);
    }
}

u32 PsxRuntime::exec_lwl(u32 vaddr, u32 cur_rt) {
    const u32 aligned = read32(vaddr & ~3u);
    switch (vaddr & 3u) {
        case 0: return (cur_rt & 0x00FFFFFFu) | (aligned << 24);
        case 1: return (cur_rt & 0x0000FFFFu) | (aligned << 16);
        case 2: return (cur_rt & 0x000000FFu) | (aligned << 8);
        default: return aligned;
    }
}

u32 PsxRuntime::exec_lwr(u32 vaddr, u32 cur_rt) {
    const u32 aligned = read32(vaddr & ~3u);
    switch (vaddr & 3u) {
        case 0: return aligned;
        case 1: return (cur_rt & 0xFF000000u) | (aligned >> 8);
        case 2: return (cur_rt & 0xFFFF0000u) | (aligned >> 16);
        default: return (cur_rt & 0xFFFFFF00u) | (aligned >> 24);
    }
}

void PsxRuntime::exec_swl(u32 vaddr, u32 cur_rt) {
    const u32 base    = vaddr & ~3u;
    const u32 aligned = read32(base);
    u32 merged = 0;
    switch (vaddr & 3u) {
        case 0: merged = (aligned & 0xFFFFFF00u) | (cur_rt >> 24); break;
        case 1: merged = (aligned & 0xFFFF0000u) | (cur_rt >> 16); break;
        case 2: merged = (aligned & 0xFF000000u) | (cur_rt >> 8);  break;
        default: merged = cur_rt; break;
    }
    write32(base, merged);
}

void PsxRuntime::exec_swr(u32 vaddr, u32 cur_rt) {
    const u32 base    = vaddr & ~3u;
    const u32 aligned = read32(base);
    u32 merged = 0;
    switch (vaddr & 3u) {
        case 0: merged = cur_rt; break;
        case 1: merged = (aligned & 0x000000FFu) | (cur_rt << 8);  break;
        case 2: merged = (aligned & 0x0000FFFFu) | (cur_rt << 16); break;
        default: merged = (aligned & 0x00FFFFFFu) | (cur_rt << 24); break;
    }
    write32(base, merged);
}

u8 PsxRuntime::read_mmio8(u32 paddr) {
    // CD-ROM 8-bit ports 0x1F801800..0x1F801803
    if (paddr >= 0x1F801800u && paddr <= 0x1F801803u) {
        return cdrom_.read_reg8(paddr - 0x1F801800u);
    }
    // SIO0 Joypad/MemCard RX Data (0x1F801040)
    if (paddr == 0x1F801040u) {
        sio_stat_ &= ~0x0002u; // Clear RX FIFO Not Empty
        return sio_rx_byte_;
    }
    const u32 w = read_mmio32(paddr & ~3u);
    return static_cast<u8>((w >> ((paddr & 3u) * 8u)) & 0xFFu);
}

u16 PsxRuntime::read_mmio16(u32 paddr) {
    if (paddr >= 0x1F801800u && paddr <= 0x1F801802u) {
        const u16 lo = cdrom_.read_reg8(paddr - 0x1F801800u);
        const u16 hi = cdrom_.read_reg8(paddr + 1u - 0x1F801800u);
        return static_cast<u16>(lo | (hi << 8));
    }
    // SIO0 registers 0x1F801040..0x1F80104E
    switch (paddr) {
        case 0x1F801040u:
            sio_stat_ &= ~0x0002u;
            return sio_rx_byte_;
        case 0x1F801044u: return sio_stat_;
        case 0x1F801048u: return sio_mode_;
        case 0x1F80104Au: return sio_ctrl_;
        case 0x1F80104Eu: return sio_baud_;
        default: break;
    }
    // Hardware Timers 0..2 (0x1F801100..0x1F80112F)
    if (paddr >= 0x1F801100u && paddr < 0x1F801130u) {
        const u32 idx = (paddr - 0x1F801100u) >> 4;
        const u32 reg = paddr & 0x0Fu;
        if (idx < 3) {
            if (reg == 0x0) return timers_[idx].counter;
            if (reg == 0x4) {
                const u16 m = timers_[idx].mode;
                timers_[idx].mode &= ~0x1800u; // Clear reached bits on read
                return m;
            }
            if (reg == 0x8) return timers_[idx].target;
        }
        return 0;
    }
    // SPU 16-bit registers (0x1F801C00..0x1F801DFF)
    if (paddr >= 0x1F801C00u && paddr < 0x1F801E00u) {
        return spu_.read_reg16(paddr);
    }

    const u32 w = read_mmio32(paddr & ~3u);
    return static_cast<u16>((w >> ((paddr & 2u) * 8u)) & 0xFFFFu);
}

u32 PsxRuntime::read_mmio32(u32 paddr) {
    if (paddr >= 0x1F801040u && paddr < 0x1F801050u) {
        return read_mmio16(paddr);
    }
    if (paddr >= 0x1F801100u && paddr < 0x1F801130u) {
        return read_mmio16(paddr);
    }
    if (paddr >= 0x1F801800u && paddr <= 0x1F801803u) {
        return cdrom_.read_data_word();
    }
    if (paddr >= 0x1F801C00u && paddr < 0x1F801E00u) {
        return static_cast<u32>(read_mmio16(paddr)) | (static_cast<u32>(read_mmio16(paddr + 2u)) << 16);
    }
    switch (paddr) {
        case 0x1F801070u: return i_stat_;
        case 0x1F801074u: return i_mask_;
        case 0x1F8010F0u: return dpcr_;
        case 0x1F8010F4u: return dicr_;
        case 0x1F801810u: return gpu_.read_gpuread();
        case 0x1F801814u: return gpu_.read_gpustat();
        case 0x1F801820u: return mdec_.read_data();
        case 0x1F801824u: return mdec_.read_status();
        default:
            if (paddr >= 0x1F801080u && paddr < 0x1F8010F0u) {
                const u32 ch  = (paddr - 0x1F801080u) >> 4;
                const u32 reg = paddr & 0x0Fu;
                if (ch < 7) {
                    if (reg == 0x0) return dma_[ch].madr;
                    if (reg == 0x4) return dma_[ch].bcr;
                    if (reg == 0x8) return dma_[ch].chcr;
                }
            }
            return 0;
    }
}

void PsxRuntime::write_mmio8(u32 paddr, u8 val) {
    if (paddr >= 0x1F801800u && paddr <= 0x1F801803u) {
        cdrom_.write_reg8(paddr - 0x1F801800u, val);
        return;
    }
    if (paddr == 0x1F801040u) {
        // SIO0 full-duplex shift register: every TX byte shifts 8 bits in on RX (0xFF when idle/unplugged)
        if (!sio_selected_) {
            sio_rx_byte_ = 0xFFu;
            sio_stat_ |= 0x0002u;
            return;
        }
        // Only Slot 0 (Port 1, sio_ctrl_ bit 13 == 0) has Digital Pad 1 plugged in; Slot 1 shifts in 0xFF without /ACK
        const bool slot0 = (sio_ctrl_ & 0x2000u) == 0;
        if (!slot0) {
            sio_rx_byte_ = 0xFFu;
            sio_stat_ |= 0x0002u;
            return;
        }
        switch (sio_step_) {
            case 0:
                if (val == 0x01u) {
                    sio_is_memcard_ = false;
                    sio_rx_byte_    = 0xFFu;
                    sio_step_       = 1;
                    sio_stat_ |= 0x0082u;
                    if ((sio_ctrl_ & 0x1000u) != 0) i_stat_ |= (1u << 7);
                } else {
                    sio_is_memcard_ = true;
                    sio_rx_byte_    = 0xFFu;
                    sio_stat_ |= 0x0002u;
                }
                break;
            case 1:
                if (val == 0x42u && !sio_is_memcard_) {
                    sio_rx_byte_ = 0x41u; // Digital Pad ID low (0x5A41)
                    sio_step_    = 2;
                    sio_stat_ |= 0x0082u;
                    if ((sio_ctrl_ & 0x1000u) != 0) i_stat_ |= (1u << 7);
                } else {
                    sio_rx_byte_ = 0xFFu;
                    sio_step_    = 0;
                    sio_stat_ |= 0x0002u;
                }
                break;
            case 2:
                sio_rx_byte_ = 0x5Au; // Ready marker
                sio_step_    = 3;
                sio_stat_ |= 0x0082u;
                if ((sio_ctrl_ & 0x1000u) != 0) i_stat_ |= (1u << 7);
                break;
            case 3:
                sio_rx_byte_ = static_cast<u8>(pad_buttons_ & 0xFFu);
                sio_step_    = 4;
                sio_stat_ |= 0x0082u;
                if ((sio_ctrl_ & 0x1000u) != 0) i_stat_ |= (1u << 7);
                break;
            case 4:
            default:
                sio_rx_byte_ = static_cast<u8>((pad_buttons_ >> 8) & 0xFFu);
                sio_step_    = 0;
                sio_stat_ |= 0x0002u; // Final byte does not ACK DSR
                break;
        }
        return;
    }
    if (paddr >= 0x1F801070u && paddr < 0x1F801074u) {
        const u32 shift = (paddr & 3u) * 8u;
        const u32 mask  = (static_cast<u32>(val) << shift) | ~(0xFFu << shift);
        i_stat_ &= mask;
        return;
    }
    if (paddr >= 0x1F801074u && paddr < 0x1F801078u) {
        const u32 shift = (paddr & 3u) * 8u;
        i_mask_ = ((i_mask_ & ~(0xFFu << shift)) | (static_cast<u32>(val) << shift)) & 0x7FFu;
        return;
    }
    if (paddr >= 0x1F801080u && paddr < 0x1F8010F0u) {
        const u32 ch    = (paddr - 0x1F801080u) >> 4;
        const u32 reg   = paddr & 0x0Cu;
        const u32 shift = (paddr & 3u) * 8u;
        if (ch < 7) {
            if (reg == 0x0) {
                dma_[ch].madr = ((dma_[ch].madr & ~(0xFFu << shift)) | (static_cast<u32>(val) << shift)) & 0x00FFFFFFu;
            } else if (reg == 0x4) {
                dma_[ch].bcr = (dma_[ch].bcr & ~(0xFFu << shift)) | (static_cast<u32>(val) << shift);
            } else if (reg == 0x8) {
                dma_[ch].chcr = (dma_[ch].chcr & ~(0xFFu << shift)) | (static_cast<u32>(val) << shift);
                if ((paddr & 3u) == 3u && (dma_[ch].chcr & (1u << 24)) != 0) {
                    execute_dma(ch);
                }
            }
        }
        return;
    }
    if (paddr >= 0x1F8010F0u && paddr < 0x1F8010F4u) {
        const u32 shift = (paddr & 3u) * 8u;
        dpcr_ = (dpcr_ & ~(0xFFu << shift)) | (static_cast<u32>(val) << shift);
        return;
    }
    if (paddr >= 0x1F8010F4u && paddr < 0x1F8010F8u) {
        switch (paddr & 3u) {
            case 0:
                dicr_ = (dicr_ & ~0x0000003Fu) | (static_cast<u32>(val) & 0x3Fu);
                break;
            case 1:
                dicr_ = (dicr_ & ~0x00008000u) | ((static_cast<u32>(val) & 0x80u) << 8);
                update_dicr_irq();
                break;
            case 2:
                dicr_ = (dicr_ & ~0x00FF0000u) | (static_cast<u32>(val) << 16);
                update_dicr_irq();
                break;
            case 3: {
                const u32 ack   = static_cast<u32>(val) & 0x7Fu;
                const u32 flags = ((dicr_ >> 24) & 0x7Fu) & ~ack;
                dicr_           = (dicr_ & 0x00FFFFFFu) | (flags << 24);
                update_dicr_irq();
                break;
            }
        }
        return;
    }
    if (paddr == 0x1F802023u || paddr == 0x1F802080u || paddr == 0x1F802041u) {
        if (paddr != 0x1F802041u) {
            tty_buf_.push_back(static_cast<char>(val));
        }
    }
}

void PsxRuntime::write_mmio16(u32 paddr, u16 val) {
    if (paddr >= 0x1F801800u && paddr <= 0x1F801802u) {
        cdrom_.write_reg8(paddr - 0x1F801800u, static_cast<u8>(val & 0xFFu));
        cdrom_.write_reg8(paddr + 1u - 0x1F801800u, static_cast<u8>((val >> 8) & 0xFFu));
        return;
    }
    switch (paddr) {
        case 0x1F801040u:
            write_mmio8(paddr, static_cast<u8>(val & 0xFFu));
            return;
        case 0x1F801048u: sio_mode_ = val; return;
        case 0x1F80104Au:
            sio_ctrl_ = val;
            if ((val & 0x0040u) != 0) { // Reset
                sio_stat_     = 0x0005u;
                sio_step_     = 0;
                sio_selected_ = false;
            }
            if ((val & 0x0010u) != 0) { // Ack IRQ
                sio_stat_ &= ~0x0200u;
            }
            if ((val & 0x0002u) != 0) { // /JOYn Select asserted
                if (!sio_selected_) {
                    sio_selected_ = true;
                    sio_step_     = 0;
                }
            } else {
                sio_selected_ = false;
                sio_step_     = 0;
            }
            return;
        case 0x1F80104Eu: sio_baud_ = val; return;
        case 0x1F801070u: i_stat_ &= (static_cast<u32>(val) | 0xFFFF0000u); return;
        case 0x1F801072u: i_stat_ &= ((static_cast<u32>(val) << 16) | 0x0000FFFFu); return;
        case 0x1F801074u: i_mask_ = val & 0x7FFu; return;
        case 0x1F801076u: return;
        default: break;
    }

    if (paddr >= 0x1F801080u && paddr < 0x1F8010F8u) {
        write_mmio8(paddr, static_cast<u8>(val & 0xFFu));
        write_mmio8(paddr + 1u, static_cast<u8>((val >> 8) & 0xFFu));
        return;
    }

    if (paddr >= 0x1F801100u && paddr < 0x1F801130u) {
        const u32 idx = (paddr - 0x1F801100u) >> 4;
        const u32 reg = paddr & 0x0Fu;
        if (idx < 3) {
            if (reg == 0x0) timers_[idx].counter = val;
            if (reg == 0x4) {
                timers_[idx].mode    = static_cast<u16>((val & 0x03FFu) | 0x0400u);
                timers_[idx].counter = 0; // Writing mode resets counter to 0 on PS1!
            }
            if (reg == 0x8) timers_[idx].target = val;
        }
        return;
    }

    if (paddr >= 0x1F801C00u && paddr < 0x1F801E00u) {
        spu_.write_reg16(paddr, val);
        return;
    }

    write_mmio32(paddr & ~3u, val);
}

void PsxRuntime::write_mmio32(u32 paddr, u32 val) {
    if (paddr == 0x1F801040u) {
        write_mmio8(paddr, static_cast<u8>(val & 0xFFu));
        return;
    }
    if (paddr >= 0x1F801044u && paddr < 0x1F801050u) {
        write_mmio16(paddr, static_cast<u16>(val & 0xFFFFu));
        return;
    }
    if (paddr >= 0x1F801100u && paddr < 0x1F801130u) {
        write_mmio16(paddr, static_cast<u16>(val & 0xFFFFu));
        return;
    }
    if (paddr >= 0x1F801C00u && paddr < 0x1F801E00u) {
        write_mmio16(paddr, static_cast<u16>(val & 0xFFFFu));
        write_mmio16(paddr + 2u, static_cast<u16>(val >> 16));
        return;
    }
    switch (paddr) {
        case 0x1F801070u:
            i_stat_ &= val;
            break;
        case 0x1F801074u:
            i_mask_ = val & 0x7FFu;
            break;
        case 0x1F8010F0u:
            dpcr_ = val;
            break;
        case 0x1F8010F4u: {
            const u32 ack = (val >> 24) & 0x7Fu;
            const u32 flags = ((dicr_ >> 24) & 0x7Fu) & ~ack;
            dicr_ = (val & 0x00FF803Fu) | (flags << 24);
            update_dicr_irq();
            break;
        }
        case 0x1F801810u:
            gpu_.write_gp0(val);
            break;
        case 0x1F801814u:
            gpu_.write_gp1(val);
            break;
        case 0x1F801820u:
            mdec_.write_cmd_or_param(val);
            pump_mdec_out_dma();
            break;
        case 0x1F801824u:
            mdec_.write_ctrl(val);
            break;
        default:
            if (paddr >= 0x1F801080u && paddr < 0x1F8010F0u) {
                const u32 ch  = (paddr - 0x1F801080u) >> 4;
                const u32 reg = paddr & 0x0Fu;
                if (ch < 7) {
                    if (reg == 0x0) dma_[ch].madr = val & 0x00FFFFFFu;
                    if (reg == 0x4) dma_[ch].bcr  = val;
                    if (reg == 0x8) {
                        dma_[ch].chcr = val;
                        if ((val & (1u << 24)) != 0) {
                            execute_dma(ch);
                        }
                    }
                }
            }
            break;
    }
}

void PsxRuntime::update_dicr_irq() {
    const bool prev_master = (dicr_ & 0x80000000u) != 0;
    const bool force_irq   = (dicr_ & (1u << 15)) != 0;
    const bool master_en   = (dicr_ & (1u << 23)) != 0;
    const u32  en_flags    = (dicr_ >> 16) & 0x7Fu;
    const u32  pend_flags  = (dicr_ >> 24) & 0x7Fu;
    const bool now_master  = force_irq || (master_en && ((en_flags & pend_flags) != 0));

    if (now_master) {
        dicr_ |= 0x80000000u;
        if (!prev_master) {
            i_stat_ |= (1u << 3); // Trigger IRQ3 (DMA)
            deliver_kernel_event(0xF0000004u, 0x0002u);
        }
    } else {
        dicr_ &= ~0x80000000u;
    }
}

void PsxRuntime::pump_mdec_out_dma() {
    auto& d1 = dma_[1];
    if ((d1.chcr & (1u << 24)) == 0 || dma1_transfer_done_ || !mdec_.has_output()) {
        return;
    }

    const u32 sync_mode   = (d1.chcr >> 9) & 3u;
    const u32 bs          = d1.bcr & 0xFFFFu;
    const u32 ba          = (sync_mode == 1) ? ((d1.bcr >> 16) & 0xFFFFu) : 1u;
    const u32 total_words = (bs == 0 ? 0x10000u : bs) * (ba == 0 ? 1u : ba);
    if (mdec_.available_output_words() < total_words && mdec_.accepting_params()) {
        return;
    }

    u32 addr = d1.madr & 0x1FFFFCu;
    for (u32 i = 0; i < total_words; ++i) {
        write32(addr, mdec_.read_data());
        addr = (addr + 4u) & 0x1FFFFCu;
    }
    d1.madr = addr;
    dma1_transfer_done_ = true;
    dma1_busy_cycles_   = 1024ull;
}

void PsxRuntime::execute_dma(u32 ch) {
    auto& d = dma_[ch];
    const u32 sync_mode = (d.chcr >> 9) & 3u;
    const bool from_ram = (d.chcr & 1u) != 0;

    if (ch == 0) {
        // MDEC-In DMA0 (PSX RAM -> 0x1F801820 MDEC Command & RLE Stream)
        const u32 bs = d.bcr & 0xFFFFu;
        const u32 ba = (sync_mode == 1) ? ((d.bcr >> 16) & 0xFFFFu) : 1u;
        const u32 total_words = (bs == 0 ? 0x10000u : bs) * (ba == 0 ? 1u : ba);
        u32 addr = d.madr & 0x1FFFFCu;
        for (u32 i = 0; i < total_words; ++i) {
            if (from_ram) {
                mdec_.write_cmd_or_param(read32(addr));
                addr = (addr + 4u) & 0x1FFFFCu;
                if (!mdec_.accepting_params() && mdec_.has_output()) {
                    // Stop feeding trailing 32-word DMA block padding once macroblocks are decoded
                    break;
                }
            }
        }
        d.madr = addr;
        pump_mdec_out_dma();
    } else if (ch == 1) {
        // MDEC-Out DMA1 (0x1F801820 MDEC Decoded Pixels -> PSX RAM)
        // Armed asynchronously so DecDCToutCallback (IRQ3) can safely chain strip transfers
        dma1_transfer_done_ = false;
        dma1_busy_cycles_   = 0;
        pump_mdec_out_dma();
        return;
    } else if (ch == 6) {
        // OTC: Clear Ordering Table backwards in RAM
        u32 addr  = d.madr & 0x1FFFFCu;
        u32 words = d.bcr & 0xFFFFu;
        if (words == 0) words = 0x10000u;

        while (words > 1) {
            const u32 prev = (addr - 4u) & 0x1FFFFCu;
            write32(addr, prev);
            addr = prev;
            --words;
        }
        write32(addr, 0x00FFFFFFu);
    } else if (ch == 2) {
        // GPU DMA
        if (sync_mode == 2) {
            u32 addr  = d.madr & 0x1FFFFCu;
            u32 guard = 0;
            while ((addr & 0x00FFFFFFu) != 0x00FFFFFFu && guard++ < 0x40000u) {
                const u32 hdr   = read32(addr);
                const u32 count = hdr >> 24;
                for (u32 i = 1; i <= count; ++i) {
                    gpu_.write_gp0(read32((addr + i * 4u) & 0x1FFFFCu));
                }
                const u32 next = hdr & 0x00FFFFFFu;
                if (next == 0x00FFFFFFu) break;
                addr = next & 0x1FFFFCu;
            }
        } else {
            const u32 bs = d.bcr & 0xFFFFu;
            const u32 ba = (sync_mode == 1) ? ((d.bcr >> 16) & 0xFFFFu) : 1u;
            const u32 total_words = (bs == 0 ? 0x10000u : bs) * (ba == 0 ? 1u : ba);
            u32 addr = d.madr & 0x1FFFFCu;
            for (u32 i = 0; i < total_words; ++i) {
                if (from_ram) {
                    gpu_.write_gp0(read32(addr));
                } else {
                    write32(addr, gpu_.read_gpuread());
                }
                addr = (addr + 4u) & 0x1FFFFCu;
            }
        }
    } else if (ch == 3) {
        // CD-ROM DMA3 (CD Data FIFO -> PSX RAM)
        u32 words = d.bcr & 0xFFFFu;
        if (sync_mode == 1) {
            const u32 ba = (d.bcr >> 16) & 0xFFFFu;
            words *= (ba == 0 ? 1u : ba);
        }
        if (words == 0) words = 2048u / 4u;
        u32 addr = d.madr & 0x1FFFFCu;
        for (u32 i = 0; i < words; ++i) {
            write32(addr, cdrom_.read_data_word());
            addr = (addr + 4u) & 0x1FFFFCu;
        }
    } else if (ch == 4) {
        // SPU DMA4 (PSX RAM <-> 512 KB SPU Sound RAM)
        const u32 bs = d.bcr & 0xFFFFu;
        const u32 ba = (sync_mode == 1) ? ((d.bcr >> 16) & 0xFFFFu) : 1u;
        const u32 total_words = (bs == 0 ? 0x10000u : bs) * (ba == 0 ? 1u : ba);
        u32 addr = d.madr & 0x1FFFFCu;
        for (u32 i = 0; i < total_words; ++i) {
            if (from_ram) {
                spu_.write_dma_word(read32(addr));
            } else {
                write32(addr, spu_.read_dma_word());
            }
            addr = (addr + 4u) & 0x1FFFFCu;
        }
    }

    d.chcr &= ~(1u << 24);
    d.chcr &= ~(1u << 28);

    // Raise channel completion bit in DICR if enabled for this channel!
    if ((dicr_ & (1u << (16u + ch))) != 0) {
        if ((dicr_ & (1u << (24u + ch))) == 0) {
            dicr_ &= ~0x80000000u;
        }
        dicr_ |= (1u << (24u + ch));
        update_dicr_irq();
    }
}

void PsxRuntime::deliver_kernel_event(u32 ev_class, u32 ev_spec) {
    for (auto& ev : events_) {
        if (ev.allocated && ev.enabled && ev.class_id == ev_class && ev.spec == ev_spec) {
            if ((ev.mode & 0xF000u) != 0x1000u) {
                ev.delivered = true;
            }
        }
    }
}

void PsxRuntime::dispatch_bios_irq_events(CpuContext& ctx, u32 ev_class, u32 ev_spec) {
    for (auto& ev : events_) {
        if (!ev.allocated || !ev.enabled || ev.class_id != ev_class || ev.spec != ev_spec) {
            continue;
        }
        if ((ev.mode & 0xF000u) == 0x1000u && ev.func_addr != 0) {
            ctx.set_reg(reg::SP, 0x8000F780u);
            ctx.set_reg(reg::GP, saved_irq_ctx_.get_reg(reg::GP));
            ctx.set_reg(reg::RA, 0x80000500u);
            call_function(ctx, ev.func_addr);
            if (!in_interrupt_) {
                return;
            }
        } else {
            ev.delivered = true;
        }
    }
}

void PsxRuntime::tick_hardware(CpuContext& ctx, u64 elapsed_cycles) {
    if (elapsed_cycles == 0) return;

    // 1. Timer 0 (pixel/sysclk) & Timer 1 (HBlank ~2160 cycles) & Timer 2 (sysclk or sysclk/8)
    {
        const u32 next0 = static_cast<u32>(timers_[0].counter) + static_cast<u32>(elapsed_cycles);
        if ((timers_[0].mode & 0x0008u) != 0 && timers_[0].target > 0 && next0 >= timers_[0].target) {
            timers_[0].counter = 0;
            timers_[0].mode |= 0x0800u;
            if ((timers_[0].mode & 0x0010u) != 0) {
                i_stat_ |= (1u << 4);
                deliver_kernel_event(0xF2000000u, 0x0002u);
            }
        } else {
            timers_[0].counter = static_cast<u16>(next0);
        }
    }

    hblank_accum_ += elapsed_cycles;
    while (hblank_accum_ >= 2160ull) {
        hblank_accum_ -= 2160ull;
        if (((timers_[1].mode >> 8) & 1u) != 0) {
            // HBlank clock source
            ++timers_[1].counter;
        } else {
            timers_[1].counter = static_cast<u16>(timers_[1].counter + 2160u);
        }
        if ((timers_[1].mode & 0x0008u) != 0 && timers_[1].counter >= timers_[1].target && timers_[1].target > 0) {
            timers_[1].counter = 0;
            timers_[1].mode |= 0x0800u;
            if ((timers_[1].mode & 0x0010u) != 0) {
                i_stat_ |= (1u << 5);
                deliver_kernel_event(0xF2000001u, 0x0002u);
            }
        }
    }

    sysclk8_accum_ += elapsed_cycles;
    while (sysclk8_accum_ >= 8ull) {
        sysclk8_accum_ -= 8ull;
        const u32 step = ((timers_[2].mode & 0x0200u) != 0) ? 1u : 8u;
        const u32 next = static_cast<u32>(timers_[2].counter) + step;
        if ((timers_[2].mode & 0x0008u) != 0 && timers_[2].target > 0 && next >= timers_[2].target) {
            timers_[2].counter = 0;
            timers_[2].mode |= 0x0800u;
            if ((timers_[2].mode & 0x0010u) != 0) {
                i_stat_ |= (1u << 6);
                deliver_kernel_event(0xF2000002u, 0x0002u);
            }
        } else {
            timers_[2].counter = static_cast<u16>(next);
        }
    }

    // 2. VBlank IRQ0 (~677,376 cycles @ 50Hz PAL / ~564,480 @ 60Hz NTSC)
    vblank_accum_ += elapsed_cycles;
    constexpr u64 VBLANK_PERIOD = 564'480ull;
    if (vblank_accum_ >= VBLANK_PERIOD) {
        vblank_accum_ -= VBLANK_PERIOD;
        ++vblank_frames_;
        i_stat_ |= (1u << 0); // IRQ0 VBlank
        deliver_kernel_event(0xF0000001u, 0x0002u);
        deliver_kernel_event(0xF2000003u, 0x0002u);
        if (on_vblank_) {
            on_vblank_();
        }
    }

    // 3. CD-ROM State Machine & IRQ2
    if (cdrom_.tick(elapsed_cycles)) {
        i_stat_ |= (1u << 2); // IRQ2 CD-ROM
        deliver_kernel_event(0xF0000003u, 0x0002u);
    }

    // 3b. SPU 24-Voice Sony ADPCM Synthesizer & IRQ9
    if (spu_.tick(elapsed_cycles)) {
        i_stat_ |= (1u << 9); // IRQ9 SPU
        deliver_kernel_event(0xF0000009u, 0x0002u);
    }

    // 3c. MDEC-Out DMA1 strip completion & IRQ3 (DecDCToutCallback)
    if ((dma_[1].chcr & (1u << 24)) != 0) {
        pump_mdec_out_dma();
        if (dma1_transfer_done_) {
            if (dma1_busy_cycles_ > elapsed_cycles) {
                dma1_busy_cycles_ -= elapsed_cycles;
            } else if (!in_interrupt_) {
                dma1_busy_cycles_   = 0;
                dma1_transfer_done_ = false;
                dma_[1].chcr &= ~(1u << 24);
                dma_[1].chcr &= ~(1u << 28);
                if ((dicr_ & (1u << 17)) != 0) {
                    if ((dicr_ & (1u << 25)) == 0) {
                        dicr_ &= ~0x80000000u;
                    }
                    dicr_ |= (1u << 25);
                    update_dicr_irq();
                }
            }
        }
    }

    // 4. Dispatch pending hardware interrupt into PsyQ HookEntryInt if enabled
    check_and_enter_interrupt(ctx);
}

void PsxRuntime::check_and_enter_interrupt(CpuContext& ctx) {
    if (in_interrupt_ || ctx.halted) {
        return;
    }
    const u32 pending_initial = i_stat_ & i_mask_;
    if (pending_initial == 0) {
        return;
    }
    // Check COP0 SR IEc (bit 0) and IM2 (bit 10)
    if ((ctx.cop0[12] & 0x0401u) != 0x0401u) {
        return;
    }

    const bool has_int_rp   = (int_rp_heads_[0] | int_rp_heads_[1] | int_rp_heads_[2] | int_rp_heads_[3]) != 0;
    const bool has_rcnt_irq = (pending_initial & 0x0071u) != 0;
    if (hook_entry_int_addr_ == 0 && !has_int_rp && !has_rcnt_irq) {
        return;
    }

    // Save interrupted CPU context for B0:0x17 ReturnFromException()
    saved_irq_ctx_       = ctx;
    saved_aot_pc_        = pending_aot_call_pc_;
    pending_aot_call_pc_ = 0;
    in_interrupt_        = true;

    // Push COP0 SR mode bits (disable interrupts during ISR)
    ctx.cop0[12] = (ctx.cop0[12] & ~0x3Fu) | ((ctx.cop0[12] & 0x0Fu) << 2);
    ctx.cop0[13] = (1u << 10); // Cause IP2

    // 1. Run SysEnqIntRP priority chains (0..3) registered by libcd / StartCARD / StartPAD
    for (u32 prio = 0; prio < 4; ++prio) {
        u32 cur = int_rp_heads_[prio];
        u32 chain_guard = 0;
        while (cur != 0 && chain_guard++ < 16) {
            const u32 next  = read32(cur + 0x00u);
            const u32 func2 = read32(cur + 0x04u);
            const u32 func1 = read32(cur + 0x08u);

            if (func1 != 0) {
                ctx.set_reg(reg::SP, 0x8000F780u);
                ctx.set_reg(reg::GP, saved_irq_ctx_.get_reg(reg::GP));
                ctx.set_reg(reg::RA, 0x80000500u);
                ctx.set_reg(reg::V0, 0u);
                call_function(ctx, func1);
                if (!in_interrupt_) {
                    return; // Handler invoked B0:0x17 ReturnFromException directly!
                }

                if (ctx.get_reg(reg::V0) != 0) {
                    if (func2 != 0) {
                        ctx.set_reg(reg::SP, 0x8000F780u);
                        ctx.set_reg(reg::GP, saved_irq_ctx_.get_reg(reg::GP));
                        ctx.set_reg(reg::RA, 0x80000500u);
                        call_function(ctx, func2);
                        if (!in_interrupt_) {
                            return; // Handler invoked B0:0x17 ReturnFromException directly!
                        }
                    }
                    break;
                }
            }
            cur = next;
        }
    }

    // 2. Built-in BIOS Root Counter & Kernel Event handler (RCntCNT0..2 = IRQ4..6, RCntCNT3 = IRQ0 VBlank)
    const u32 active_rcnt = i_stat_ & i_mask_;
    for (u32 t = 0; t < 3; ++t) {
        const u32 irq_bit = 1u << (4u + t);
        if ((active_rcnt & irq_bit) != 0) {
            if (clear_rcnt_flags_[t] != 0) {
                i_stat_ &= ~irq_bit;
            }
            dispatch_bios_irq_events(ctx, 0xF2000000u + t, 0x0002u);
            if (!in_interrupt_) return;
            dispatch_bios_irq_events(ctx, 0xF0000005u + t, 0x0002u);
            if (!in_interrupt_) return;
        }
    }
    if ((active_rcnt & 1u) != 0) {
        if (clear_rcnt_flags_[3] != 0) {
            i_stat_ &= ~1u;
        }
        dispatch_bios_irq_events(ctx, 0xF2000003u, 0x0002u);
        if (!in_interrupt_) return;
        dispatch_bios_irq_events(ctx, 0xF0000001u, 0x0002u);
        if (!in_interrupt_) return;
    }

    // 3. If unmasked interrupts remain pending and HookEntryInt (ResetGraph/startIntr) is active, execute jtb ISR synchronously
    if ((i_stat_ & i_mask_) != 0 && hook_entry_int_addr_ != 0) {
        const u32 jb = hook_entry_int_addr_;
        const u32 target_ra = read32(jb + 0x00u);
        ctx.set_reg(reg::RA, target_ra);
        ctx.set_reg(reg::SP, read32(jb + 0x04u));
        ctx.set_reg(reg::FP, read32(jb + 0x08u));
        for (u32 i = 0; i < 8; ++i) {
            ctx.set_reg(reg::S0 + i, read32(jb + 0x0Cu + i * 4u));
        }
        ctx.set_reg(reg::GP, read32(jb + 0x2Cu));
        ctx.set_reg(reg::V0, 1u);
        ctx.pc      = target_ra;
        ctx.next_pc = target_ra + 4u;

        u32 irq_guard = 0;
        while (in_interrupt_ && !ctx.halted && irq_guard++ < 2'000'000u) {
            if (pending_aot_call_pc_ != 0 && ctx.next_pc == ctx.pc + 4u && to_kseg0(ctx.pc) == pending_aot_call_pc_) {
                pending_aot_call_pc_ = 0;
                if (RecompFunc aot = find_aot_func(to_kseg0(ctx.pc))) {
                    ++aot_dispatches_;
                    aot(ctx, *this);
                    continue;
                }
            }
            step_interpreter(ctx);
        }
        if (in_interrupt_) {
            const u64 cur_cycles = ctx.cycles;
            ctx                  = saved_irq_ctx_;
            ctx.cycles           = cur_cycles;
            pending_aot_call_pc_ = saved_aot_pc_;
            in_interrupt_        = false;
        }
        return;
    }

    // Otherwise all firing IRQ bits were handled by SysEnqIntRP; restore context immediately
    const u64 cur_cycles = ctx.cycles;
    ctx                  = saved_irq_ctx_;
    ctx.cycles           = cur_cycles;
    pending_aot_call_pc_ = saved_aot_pc_;
    in_interrupt_        = false;
}

std::string PsxRuntime::read_cstr(u32 vaddr, u32 max_len) {
    std::string s;
    for (u32 i = 0; i < max_len; ++i) {
        const u8 c = read8(vaddr + i);
        if (c == 0) break;
        s.push_back(static_cast<char>(c));
    }
    return s;
}

std::string PsxRuntime::format_bios_printf(CpuContext& ctx) {
    const std::string fmt = read_cstr(ctx.get_reg(reg::A0), 1024);
    u32 arg_idx = 1; // Next arg starts at $a1 (idx 1->a1, 2->a2, 3->a3, >=4 -> sp + 4*idx)

    auto next_arg = [&]() -> u32 {
        u32 val = 0;
        if (arg_idx == 1) val = ctx.get_reg(reg::A1);
        else if (arg_idx == 2) val = ctx.get_reg(reg::A2);
        else if (arg_idx == 3) val = ctx.get_reg(reg::A3);
        else val = read32(ctx.get_reg(reg::SP) + arg_idx * 4u);
        ++arg_idx;
        return val;
    };

    std::string out;
    out.reserve(fmt.size() + 32);

    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            out.push_back(fmt[i]);
            continue;
        }
        if (i + 1 < fmt.size() && fmt[i + 1] == '%') {
            out.push_back('%');
            ++i;
            continue;
        }

        // Parse simple format spec: %[0][width][l][d|i|u|x|X|p|s|c]
        std::string spec = "%";
        ++i;
        while (i < fmt.size() && (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == '0' ||
                                  (fmt[i] >= '1' && fmt[i] <= '9'))) {
            spec.push_back(fmt[i++]);
        }
        if (i < fmt.size() && fmt[i] == 'l') {
            ++i;
        }
        if (i >= fmt.size()) break;

        const char conv = fmt[i];
        char buf[64]{};
        switch (conv) {
            case 'd':
            case 'i':
                spec.push_back('d');
                std::snprintf(buf, sizeof(buf), spec.c_str(), static_cast<int>(static_cast<s32>(next_arg())));
                out += buf;
                break;
            case 'u':
                spec.push_back('u');
                std::snprintf(buf, sizeof(buf), spec.c_str(), static_cast<unsigned int>(next_arg()));
                out += buf;
                break;
            case 'x':
            case 'X':
            case 'p':
                spec.push_back(conv == 'X' ? 'X' : 'x');
                std::snprintf(buf, sizeof(buf), spec.c_str(), static_cast<unsigned int>(next_arg()));
                out += buf;
                break;
            case 'c':
                out.push_back(static_cast<char>(next_arg() & 0xFFu));
                break;
            case 's': {
                const u32 ptr = next_arg();
                out += (ptr != 0) ? read_cstr(ptr, 512) : "(null)";
                break;
            }
            default:
                out.push_back(conv);
                break;
        }
    }
    return out;
}

bool PsxRuntime::handle_bios_vector(CpuContext& ctx, u32 masked_pc) {
    if (masked_pc != 0xA0u && masked_pc != 0xB0u && masked_pc != 0xC0u) {
        return false;
    }

    const u32 fn = ctx.get_reg(reg::T1) & 0xFFu;
    const u32 a0 = ctx.get_reg(reg::A0);
    const u32 a1 = ctx.get_reg(reg::A1);
    const u32 a2 = ctx.get_reg(reg::A2);
    const u32 a3 = ctx.get_reg(reg::A3);

    if (masked_pc == 0xA0u) {
        switch (fn) {
            case 0x13: { // setjmp(jmp_buf* buf) -> MUST return 0!
                write32(a0 + 0x00u, ctx.get_reg(reg::RA));
                write32(a0 + 0x04u, ctx.get_reg(reg::SP));
                write32(a0 + 0x08u, ctx.get_reg(reg::FP));
                for (u32 i = 0; i < 8; ++i) {
                    write32(a0 + 0x0Cu + i * 4u, ctx.get_reg(reg::S0 + i));
                }
                write32(a0 + 0x2Cu, ctx.get_reg(reg::GP));
                ctx.set_reg(reg::V0, 0u);
                break;
            }
            case 0x14: { // longjmp(jmp_buf* buf, int val)
                const u32 ret_ra = read32(a0 + 0x00u);
                ctx.set_reg(reg::RA, ret_ra);
                ctx.set_reg(reg::SP, read32(a0 + 0x04u));
                ctx.set_reg(reg::FP, read32(a0 + 0x08u));
                for (u32 i = 0; i < 8; ++i) {
                    ctx.set_reg(reg::S0 + i, read32(a0 + 0x0Cu + i * 4u));
                }
                ctx.set_reg(reg::GP, read32(a0 + 0x2Cu));
                ctx.set_reg(reg::V0, (a1 == 0) ? 1u : a1);
                ctx.pc      = ret_ra;
                ctx.next_pc = ret_ra + 4u;
                ++longjmp_token_;
                return true;
            }
            case 0x15: { // strcat
                const u32 dlen = static_cast<u32>(read_cstr(a0, 65536).size());
                u32 i = 0;
                while (true) {
                    const u8 c = read8(a1 + i);
                    write8(a0 + dlen + i, c);
                    if (c == 0) break;
                    ++i;
                }
                ctx.set_reg(reg::V0, a0);
                break;
            }
            case 0x17: { // strcmp
                const std::string s1 = read_cstr(a0);
                const std::string s2 = read_cstr(a1);
                ctx.set_reg(reg::V0, static_cast<u32>(std::strcmp(s1.c_str(), s2.c_str())));
                break;
            }
            case 0x18: { // strncmp
                const std::string s1 = read_cstr(a0, a2);
                const std::string s2 = read_cstr(a1, a2);
                ctx.set_reg(reg::V0, static_cast<u32>(std::strncmp(s1.c_str(), s2.c_str(), a2)));
                break;
            }
            case 0x19: { // strcpy
                u32 i = 0;
                while (true) {
                    const u8 c = read8(a1 + i);
                    write8(a0 + i, c);
                    if (c == 0) break;
                    ++i;
                }
                ctx.set_reg(reg::V0, a0);
                break;
            }
            case 0x1A: { // strncpy
                bool ended = false;
                for (u32 i = 0; i < a2; ++i) {
                    const u8 c = ended ? 0u : read8(a1 + i);
                    if (c == 0) ended = true;
                    write8(a0 + i, c);
                }
                ctx.set_reg(reg::V0, a0);
                break;
            }
            case 0x1B: { // strlen
                ctx.set_reg(reg::V0, static_cast<u32>(read_cstr(a0, 65536).size()));
                break;
            }
            case 0x27: { // bcopy(src, dst, len)
                for (u32 i = 0; i < a2; ++i) write8(a1 + i, read8(a0 + i));
                break;
            }
            case 0x28: { // bzero(dst, len)
                for (u32 i = 0; i < a1; ++i) write8(a0 + i, 0);
                break;
            }
            case 0x2A: { // memcpy(dst, src, len)
                for (u32 i = 0; i < a2; ++i) write8(a0 + i, read8(a1 + i));
                ctx.set_reg(reg::V0, a0);
                break;
            }
            case 0x2B: { // memset(dst, val, len)
                for (u32 i = 0; i < a2; ++i) write8(a0 + i, static_cast<u8>(a1 & 0xFFu));
                ctx.set_reg(reg::V0, a0);
                break;
            }
            case 0x2F: { // rand
                rand_seed_ = rand_seed_ * 1103515245u + 12345u;
                ctx.set_reg(reg::V0, (rand_seed_ >> 16) & 0x7FFFu);
                break;
            }
            case 0x30: { // srand
                rand_seed_ = a0;
                break;
            }
            case 0x33: { // malloc
                const u32 aligned = (a0 + 7u) & ~7u;
                if (heap_cur_ + aligned <= heap_end_) {
                    ctx.set_reg(reg::V0, heap_cur_);
                    heap_cur_ += aligned;
                } else {
                    ctx.set_reg(reg::V0, 0);
                }
                break;
            }
            case 0x39: { // InitHeap
                heap_base_ = a0;
                heap_cur_  = a0;
                heap_end_  = a0 + a1;
                break;
            }
            case 0x3C: { // putchar
                tty_buf_.push_back(static_cast<char>(a0 & 0xFFu));
                break;
            }
            case 0x3E: { // puts
                tty_buf_ += read_cstr(a0);
                tty_buf_.push_back('\n');
                break;
            }
            case 0x3F: { // printf
                const std::string formatted = format_bios_printf(ctx);
                tty_buf_ += formatted;
                ctx.set_reg(reg::V0, static_cast<u32>(formatted.size()));
                break;
            }
            case 0x44: // FlushCache
                break;
            case 0x49: // GPU_cw
                gpu_.write_gp0(a0);
                break;
            case 0x70: // _bu_init
                deliver_kernel_event(0xF4000001u, 0x0004u);
                ctx.set_reg(reg::V0, 1);
                break;
            case 0x72: // _96_remove
                ctx.set_reg(reg::V0, 1);
                break;
            default:
                ctx.set_reg(reg::V0, 0);
                break;
        }
    } else if (masked_pc == 0xB0u) {
        switch (fn) {
            case 0x02: { // SetRCnt(spec, target, mode)
                const u32 idx = a0 & 0xFFFFu;
                if (idx >= 3) {
                    ctx.set_reg(reg::V0, 0);
                    break;
                }
                timers_[idx].counter = 0;
                timers_[idx].target  = static_cast<u16>(a1 & 0xFFFFu);
                u16 hw_mode = 0x0448u; // Reset on target (bit 3) + Repeat IRQ (bit 6) + IRQ not active (bit 10)
                if (idx < 2) {
                    if ((a2 & 0x0100u) != 0) hw_mode |= 0x0100u;
                } else {
                    if ((a2 & 0x0001u) == 0) hw_mode |= 0x0200u; // sysclk/8 unless RCntMdSC (bit 0)
                }
                if ((a2 & 0x1000u) != 0) {
                    hw_mode |= 0x0010u; // Enable IRQ on target
                }
                timers_[idx].mode = hw_mode;
                ctx.set_reg(reg::V0, 1);
                break;
            }
            case 0x03: { // GetRCnt(spec)
                const u32 idx = a0 & 0xFFFFu;
                ctx.set_reg(reg::V0, (idx < 3) ? timers_[idx].counter : (vblank_frames_ & 0xFFFFu));
                break;
            }
            case 0x04: { // StartRCnt(spec)
                const u32 idx = a0 & 0xFFFFu;
                if (idx < 3)      i_mask_ |= (1u << (4u + idx));
                else if (idx == 3) i_mask_ |= 1u;
                ctx.set_reg(reg::V0, 1);
                break;
            }
            case 0x05: { // StopRCnt(spec)
                const u32 idx = a0 & 0xFFFFu;
                if (idx < 3)      i_mask_ &= ~(1u << (4u + idx));
                else if (idx == 3) i_mask_ &= ~1u;
                ctx.set_reg(reg::V0, 1);
                break;
            }
            case 0x06: { // ResetRCnt(spec)
                const u32 idx = a0 & 0xFFFFu;
                if (idx < 3) timers_[idx].counter = 0;
                ctx.set_reg(reg::V0, 1);
                break;
            }
            case 0x07: { // DeliverEvent(class, spec)
                deliver_kernel_event(a0, a1);
                break;
            }
            case 0x08: { // OpenEvent(class, spec, mode, func)
                u32 handle = 0xFFFFFFFFu;
                for (u32 i = 0; i < events_.size(); ++i) {
                    if (!events_[i].allocated) {
                        events_[i] = KernelEvent{true, false, false, a0, a1, a2, a3};
                        handle = 0xF1000000u | (i << 4);
                        break;
                    }
                }
                ctx.set_reg(reg::V0, handle);
                break;
            }
            case 0x09: { // CloseEvent(handle)
                const u32 idx = (a0 >> 4) & 0x3Fu;
                if ((a0 & 0xFF000000u) == 0xF1000000u && idx < events_.size()) {
                    events_[idx] = KernelEvent{};
                }
                ctx.set_reg(reg::V0, 1);
                break;
            }
            case 0x0A: { // WaitEvent(handle)
                const u32 idx = (a0 >> 4) & 0x3Fu;
                if ((a0 & 0xFF000000u) == 0xF1000000u && idx < events_.size()) {
                    events_[idx].delivered = false;
                }
                ctx.set_reg(reg::V0, 1);
                break;
            }
            case 0x0B: { // TestEvent(handle)
                const u32 idx = (a0 >> 4) & 0x3Fu;
                u32 ready = 0;
                if ((a0 & 0xFF000000u) == 0xF1000000u && idx < events_.size() && events_[idx].delivered) {
                    events_[idx].delivered = false;
                    ready = 1;
                }
                ctx.set_reg(reg::V0, ready);
                break;
            }
            case 0x0C: { // EnableEvent(handle)
                const u32 idx = (a0 >> 4) & 0x3Fu;
                if ((a0 & 0xFF000000u) == 0xF1000000u && idx < events_.size()) {
                    events_[idx].enabled = true;
                }
                ctx.set_reg(reg::V0, 1);
                break;
            }
            case 0x0D: { // DisableEvent(handle)
                const u32 idx = (a0 >> 4) & 0x3Fu;
                if ((a0 & 0xFF000000u) == 0xF1000000u && idx < events_.size()) {
                    events_[idx].enabled = false;
                }
                ctx.set_reg(reg::V0, 1);
                break;
            }
            case 0x17: { // ReturnFromException()
                if (in_interrupt_) {
                    const u64 cur_cycles = ctx.cycles;
                    ctx                  = saved_irq_ctx_;
                    ctx.cycles           = cur_cycles;
                    pending_aot_call_pc_ = saved_aot_pc_;
                    in_interrupt_        = false;
                    return true;
                }
                break;
            }
            case 0x18: { // ResetEntryInt()
                hook_entry_int_addr_ = 0;
                break;
            }
            case 0x19: { // HookEntryInt(jmp_buf* addr)
                const u32 prev       = hook_entry_int_addr_;
                hook_entry_int_addr_ = a0;
                ctx.set_reg(reg::V0, prev);
                break;
            }
            case 0x20: { // UnDeliverEvent(class, spec)
                for (auto& ev : events_) {
                    if (ev.allocated && ev.class_id == a0 && ev.spec == a1) {
                        ev.delivered = false;
                    }
                }
                break;
            }
            case 0x3D: { // std_out_putchar
                tty_buf_.push_back(static_cast<char>(a0 & 0xFFu));
                break;
            }
            case 0x3F: { // puts
                tty_buf_ += read_cstr(a0);
                tty_buf_.push_back('\n');
                break;
            }
            case 0x5B: { // ChangeClearPAD(int val)
                const u32 prev  = clear_pad_flag_;
                clear_pad_flag_ = a0;
                ctx.set_reg(reg::V0, prev);
                break;
            }
            default:
                ctx.set_reg(reg::V0, 0);
                break;
        }
    } else {
        // C0 Kernel Vector
        switch (fn) {
            case 0x02: { // SysEnqIntRP(priority, struc)
                const u32 prio = a0 & 3u;
                write32(a1 + 0u, int_rp_heads_[prio]);
                int_rp_heads_[prio] = a1;
                ctx.set_reg(reg::V0, 1);
                break;
            }
            case 0x03: { // SysDeqIntRP(priority, struc)
                const u32 prio = a0 & 3u;
                u32 cur  = int_rp_heads_[prio];
                u32 prev = 0;
                while (cur != 0) {
                    const u32 next = read32(cur);
                    if (cur == a1) {
                        if (prev == 0) int_rp_heads_[prio] = next;
                        else           write32(prev, next);
                        break;
                    }
                    prev = cur;
                    cur  = next;
                }
                ctx.set_reg(reg::V0, 1);
                break;
            }
            case 0x0A: { // ChangeClearRCnt(int t, int flag)
                const u32 t    = a0 & 3u;
                const u32 prev = clear_rcnt_flags_[t];
                clear_rcnt_flags_[t] = a1;
                ctx.set_reg(reg::V0, prev);
                break;
            }
            default:
                ctx.set_reg(reg::V0, 0);
                break;
        }
    }

    ctx.pc      = ctx.get_reg(reg::RA);
    ctx.next_pc = ctx.pc + 4u;
    return true;
}

void PsxRuntime::handle_syscall(CpuContext& ctx) {
    const u32 code = ctx.get_reg(reg::A0);
    switch (code) {
        case 1: { // EnterCriticalSection
            const u32 was_enabled = ((ctx.cop0[12] & 0x0401u) == 0x0401u) ? 1u : 0u;
            ctx.cop0[12] &= ~0x0401u;
            ctx.set_reg(reg::V0, was_enabled);
            break;
        }
        case 2: // ExitCriticalSection
            ctx.cop0[12] |= 0x0401u;
            break;
        default:
            break;
    }
}

void PsxRuntime::register_func(u32 kseg0_addr, RecompFunc fn) {
    aot_funcs_[to_kseg0(kseg0_addr)].push_back(AotEntry{0u, 0ull, fn, 0u, true});
}

void PsxRuntime::register_func(u32 kseg0_addr, u32 byte_len, u64 fnv1a_hash, RecompFunc fn) {
    aot_funcs_[to_kseg0(kseg0_addr)].push_back(AotEntry{byte_len, fnv1a_hash, fn, 0u, false});
}

bool PsxRuntime::has_recompiled_func(u32 vaddr) const noexcept {
    return aot_funcs_.find(to_kseg0(vaddr)) != aot_funcs_.end();
}

RecompFunc PsxRuntime::find_aot_func(u32 canonical_addr) {
    static const struct DenyRange {
        u32 lo = 0, hi = 0;
        DenyRange() {
            if (const char* e = std::getenv("VS_AOT_DENY")) {
                unsigned a = 0, b = 0;
                if (std::sscanf(e, "%x-%x", &a, &b) == 2) { lo = a; hi = b; }
            }
        }
    } deny;
    if (canonical_addr >= deny.lo && canonical_addr < deny.hi) {
        return nullptr;
    }
    const auto it = aot_funcs_.find(canonical_addr);
    if (it == aot_funcs_.end()) {
        return nullptr;
    }

    const u32 paddr = mask_psx_addr(canonical_addr);
    for (auto& entry : it->second) {
        if (entry.byte_len == 0) {
            return entry.fn;
        }
        if (paddr + entry.byte_len > ram_.size()) {
            continue;
        }
        const u32 p0 = paddr >> 12;
        const u32 p1 = (paddr + entry.byte_len - 1u) >> 12;
        u32 ep = 2166136261u;
        for (u32 p = p0; p <= p1; ++p) {
            ep = (ep ^ ram_page_epoch_[p]) * 16777619u;
        }
        if (ep == 0) ep = 1u;

        if (entry.cached_epoch != ep) {
            entry.cached_epoch = ep;
            entry.cached_valid = (fnv1a_64(&ram_[paddr], entry.byte_len) == entry.fnv1a_hash);
        }
        if (entry.cached_valid) {
            return entry.fn;
        }
    }
    return nullptr;
}

// Diagnostic: prints the last interpreter-executed PCs (used after an unexpected CPU halt).
void dump_halt_trace() {
    for (u32 k = 0; k < 64u; ++k) { const TraceEntry& c = g_call_ring[(g_call_pos + k) & 63u]; std::printf("[call] tgt=%08X ra=%08X sp=%08X\n", c.pc, c.ra, c.sp); }
    for (u32 k = 0; k < 16u; ++k) { const TraceEntry& w = g_watch[(g_watch_pos + k) & 15u]; std::printf("[watch2] addr=%06X lastpc=%08X\n", w.pc, w.ra); }
    const u32 n = g_trace_pos < 64u ? g_trace_pos : 64u;
    for (u32 k = n; k > 0; --k) {
        const TraceEntry& e = g_trace_ring[(g_trace_pos - k) & 63u];
        std::printf("[trace] pc=%08X ra=%08X sp=%08X\n", e.pc, e.ra, e.sp);
    }
}

void PsxRuntime::call_function(CpuContext& ctx, u32 target_pc) {
    g_call_ring[g_call_pos++ & 63u] = {target_pc, ctx.get_reg(reg::RA), ctx.get_reg(reg::SP)};
    const u32 masked = mask_psx_addr(target_pc);
    if (handle_bios_vector(ctx, masked)) {
        return;
    }

    const u32 canonical = to_kseg0(target_pc);
    discovered_calls_.insert(canonical);

    if (RecompFunc aot = find_aot_func(canonical)) {
        ++aot_dispatches_;
        ctx.pc      = target_pc;
        ctx.next_pc = target_pc + 4u;
        const u32 chk_sp = ctx.get_reg(reg::SP), chk_ra = ctx.get_reg(reg::RA);
        const u64 chk_lj = longjmp_token_;
        aot(ctx, *this);
        // A `jr $ra` whose target lies inside this very function (pseudo-call via branch with a
        // hand-built $ra, e.g. `bne ..., func_entry` with $ra preset) cannot be expressed by the
        // lifted C++ body: resume the remaining code of that function in the interpreter.
        if (!ctx.halted && longjmp_token_ == chk_lj && ctx.pc != chk_ra) {
            u32 span = 0;
            if (const auto fit = aot_funcs_.find(canonical); fit != aot_funcs_.end()) {
                for (const auto& en : fit->second) {
                    if (en.fn == aot) span = en.byte_len;
                }
            }
            const u32 cur = to_kseg0(ctx.pc);
            if (span != 0 && cur >= canonical && cur < canonical + span) {
                u64 guard = 0;
                while (!ctx.halted && ctx.pc != chk_ra && guard++ < 50'000'000ull) {
                    if (should_unwind(ctx, in_interrupt_, chk_lj)) {
                        return;
                    }
                    if (pending_aot_call_pc_ != 0 && ctx.next_pc == ctx.pc + 4u && to_kseg0(ctx.pc) == pending_aot_call_pc_) {
                        pending_aot_call_pc_ = 0;
                        if (RecompFunc inner = find_aot_func(to_kseg0(ctx.pc))) {
                            ++aot_dispatches_;
                            inner(ctx, *this);
                            continue;
                        }
                    }
                    step_interpreter(ctx);
                }
            }
        }        return;
    }

    const bool started_in_irq = in_interrupt_;
    const u64 entry_lj        = longjmp_token_;
    const u32 return_ra       = ctx.get_reg(reg::RA);
    ctx.pc      = target_pc;
    ctx.next_pc = target_pc + 4u;

    u64 guard = 0;
    while (!ctx.halted && ctx.pc != return_ra && guard++ < 10'000'000ull) {
        if (should_unwind(ctx, started_in_irq, entry_lj)) {
            return;
        }
        if (pending_aot_call_pc_ != 0 && ctx.next_pc == ctx.pc + 4u && to_kseg0(ctx.pc) == pending_aot_call_pc_) {
            pending_aot_call_pc_ = 0;
            if (RecompFunc aot = find_aot_func(to_kseg0(ctx.pc))) {
                ++aot_dispatches_;
                aot(ctx, *this);
                continue;
            }
        }
        step_interpreter(ctx);
    }
}

void PsxRuntime::step_interpreter(CpuContext& ctx) {
    if (ctx.halted) return;

    // Periodic hardware tick every 64 instructions
    if ((ctx.cycles - last_hw_cycle_) >= 64ull) {
        const u64 delta = ctx.cycles - last_hw_cycle_;
        last_hw_cycle_  = ctx.cycles;
        tick_hardware(ctx, delta);
    }

    const u32 cur_pc    = ctx.pc;
    const u32 masked_pc = mask_psx_addr(cur_pc);
    if (handle_bios_vector(ctx, masked_pc)) {
        ctx.cycles += 3;
        return;
    }

    g_trace_ring[g_trace_pos++ & 63u] = {cur_pc, ctx.get_reg(reg::RA), ctx.get_reg(reg::SP)};

    const u32 word = read32(cur_pc);
    ctx.pc         = ctx.next_pc;
    ctx.next_pc    = ctx.pc + 4u;
    ctx.cycles += 3;

    const DecodedInst i = decode_mips(word);
    const u32 rs_val = ctx.get_reg(i.rs);
    const u32 rt_val = ctx.get_reg(i.rt);

    switch (i.op) {
        case MipsOp::NOP: break;
        case MipsOp::SLL:  ctx.set_reg(i.rd, rt_val << i.shamt); break;
        case MipsOp::SRL:  ctx.set_reg(i.rd, rt_val >> i.shamt); break;
        case MipsOp::SRA:  ctx.set_reg(i.rd, static_cast<u32>(static_cast<s32>(rt_val) >> i.shamt)); break;
        case MipsOp::SLLV: ctx.set_reg(i.rd, rt_val << (rs_val & 31u)); break;
        case MipsOp::SRLV: ctx.set_reg(i.rd, rt_val >> (rs_val & 31u)); break;
        case MipsOp::SRAV: ctx.set_reg(i.rd, static_cast<u32>(static_cast<s32>(rt_val) >> (rs_val & 31u))); break;

        case MipsOp::JR:
            ctx.next_pc = rs_val;
            break;
        case MipsOp::JALR:
            ctx.set_reg(i.rd, cur_pc + 8u);
            ctx.next_pc          = rs_val;
            pending_aot_call_pc_ = to_kseg0(rs_val);
            discovered_calls_.insert(to_kseg0(rs_val));
            break;
        case MipsOp::J:
            ctx.next_pc = i.jump_target(cur_pc);
            break;
        case MipsOp::JAL: {
            const u32 tgt = i.jump_target(cur_pc);
            ctx.set_reg(reg::RA, cur_pc + 8u);
            ctx.next_pc = tgt;
            if (mask_psx_addr(tgt) >= 0x1000u) {
                pending_aot_call_pc_ = to_kseg0(tgt);
                discovered_calls_.insert(to_kseg0(tgt));
            }
            break;
        }
        case MipsOp::BEQ:
            if (rs_val == rt_val) ctx.next_pc = i.branch_target(cur_pc);
            break;
        case MipsOp::BNE:
            if (rs_val != rt_val) ctx.next_pc = i.branch_target(cur_pc);
            break;
        case MipsOp::BLEZ:
            if (static_cast<s32>(rs_val) <= 0) ctx.next_pc = i.branch_target(cur_pc);
            break;
        case MipsOp::BGTZ:
            if (static_cast<s32>(rs_val) > 0) ctx.next_pc = i.branch_target(cur_pc);
            break;
        case MipsOp::BLTZ:
            if (static_cast<s32>(rs_val) < 0) ctx.next_pc = i.branch_target(cur_pc);
            break;
        case MipsOp::BGEZ:
            if (static_cast<s32>(rs_val) >= 0) ctx.next_pc = i.branch_target(cur_pc);
            break;
        case MipsOp::BLTZAL:
            ctx.set_reg(reg::RA, cur_pc + 8u);
            if (static_cast<s32>(rs_val) < 0) ctx.next_pc = i.branch_target(cur_pc);
            break;
        case MipsOp::BGEZAL:
            ctx.set_reg(reg::RA, cur_pc + 8u);
            if (static_cast<s32>(rs_val) >= 0) ctx.next_pc = i.branch_target(cur_pc);
            break;

        case MipsOp::SYSCALL:
            handle_syscall(ctx);
            break;
        case MipsOp::BREAK:
            ctx.halted = true;
            break;

        case MipsOp::MFHI: ctx.set_reg(i.rd, ctx.hi); break;
        case MipsOp::MTHI: ctx.hi = rs_val; break;
        case MipsOp::MFLO: ctx.set_reg(i.rd, ctx.lo); break;
        case MipsOp::MTLO: ctx.lo = rs_val; break;
        case MipsOp::MULT: {
            const s64 prod = static_cast<s64>(static_cast<s32>(rs_val)) * static_cast<s64>(static_cast<s32>(rt_val));
            ctx.lo = static_cast<u32>(prod & 0xFFFFFFFFu);
            ctx.hi = static_cast<u32>((static_cast<u64>(prod) >> 32) & 0xFFFFFFFFu);
            break;
        }
        case MipsOp::MULTU: {
            const u64 prod = static_cast<u64>(rs_val) * static_cast<u64>(rt_val);
            ctx.lo = static_cast<u32>(prod & 0xFFFFFFFFu);
            ctx.hi = static_cast<u32>((prod >> 32) & 0xFFFFFFFFu);
            break;
        }
        case MipsOp::DIV: {
            const s32 n = static_cast<s32>(rs_val);
            const s32 d = static_cast<s32>(rt_val);
            if (d == 0) {
                ctx.lo = (n >= 0) ? 0xFFFFFFFFu : 1u;
                ctx.hi = static_cast<u32>(n);
            } else if (static_cast<u32>(n) == 0x80000000u && d == -1) {
                ctx.lo = 0x80000000u;
                ctx.hi = 0;
            } else {
                ctx.lo = static_cast<u32>(n / d);
                ctx.hi = static_cast<u32>(n % d);
            }
            break;
        }
        case MipsOp::DIVU: {
            if (rt_val == 0) {
                ctx.lo = 0xFFFFFFFFu;
                ctx.hi = rs_val;
            } else {
                ctx.lo = rs_val / rt_val;
                ctx.hi = rs_val % rt_val;
            }
            break;
        }

        case MipsOp::ADD:
        case MipsOp::ADDU: ctx.set_reg(i.rd, rs_val + rt_val); break;
        case MipsOp::SUB:
        case MipsOp::SUBU: ctx.set_reg(i.rd, rs_val - rt_val); break;
        case MipsOp::AND:  ctx.set_reg(i.rd, rs_val & rt_val); break;
        case MipsOp::OR:   ctx.set_reg(i.rd, rs_val | rt_val); break;
        case MipsOp::XOR:  ctx.set_reg(i.rd, rs_val ^ rt_val); break;
        case MipsOp::NOR:  ctx.set_reg(i.rd, ~(rs_val | rt_val)); break;
        case MipsOp::SLT:  ctx.set_reg(i.rd, (static_cast<s32>(rs_val) < static_cast<s32>(rt_val)) ? 1u : 0u); break;
        case MipsOp::SLTU: ctx.set_reg(i.rd, (rs_val < rt_val) ? 1u : 0u); break;

        case MipsOp::ADDI:
        case MipsOp::ADDIU: ctx.set_reg(i.rt, rs_val + static_cast<u32>(i.simm16())); break;
        case MipsOp::SLTI:  ctx.set_reg(i.rt, (static_cast<s32>(rs_val) < i.simm16()) ? 1u : 0u); break;
        case MipsOp::SLTIU: ctx.set_reg(i.rt, (rs_val < static_cast<u32>(i.simm16())) ? 1u : 0u); break;
        case MipsOp::ANDI:  ctx.set_reg(i.rt, rs_val & i.uimm16()); break;
        case MipsOp::ORI:   ctx.set_reg(i.rt, rs_val | i.uimm16()); break;
        case MipsOp::XORI:  ctx.set_reg(i.rt, rs_val ^ i.uimm16()); break;
        case MipsOp::LUI:   ctx.set_reg(i.rt, i.uimm16() << 16); break;

        case MipsOp::LB:  ctx.set_reg(i.rt, static_cast<u32>(static_cast<s32>(static_cast<s8>(read8(rs_val + static_cast<u32>(i.simm16())))))); break;
        case MipsOp::LBU: ctx.set_reg(i.rt, read8(rs_val + static_cast<u32>(i.simm16()))); break;
        case MipsOp::LH:  ctx.set_reg(i.rt, static_cast<u32>(static_cast<s32>(static_cast<s16>(read16(rs_val + static_cast<u32>(i.simm16())))))); break;
        case MipsOp::LHU: ctx.set_reg(i.rt, read16(rs_val + static_cast<u32>(i.simm16()))); break;
        case MipsOp::LW:  ctx.set_reg(i.rt, read32(rs_val + static_cast<u32>(i.simm16()))); break;
        case MipsOp::LWL: ctx.set_reg(i.rt, exec_lwl(rs_val + static_cast<u32>(i.simm16()), rt_val)); break;
        case MipsOp::LWR: ctx.set_reg(i.rt, exec_lwr(rs_val + static_cast<u32>(i.simm16()), rt_val)); break;

        case MipsOp::SB:  write8(rs_val + static_cast<u32>(i.simm16()), static_cast<u8>(rt_val & 0xFFu)); break;
        case MipsOp::SH:  write16(rs_val + static_cast<u32>(i.simm16()), static_cast<u16>(rt_val & 0xFFFFu)); break;
        case MipsOp::SW:  write32(rs_val + static_cast<u32>(i.simm16()), rt_val); break;
        case MipsOp::SWL: exec_swl(rs_val + static_cast<u32>(i.simm16()), rt_val); break;
        case MipsOp::SWR: exec_swr(rs_val + static_cast<u32>(i.simm16()), rt_val); break;

        case MipsOp::MFC0: ctx.set_reg(i.rt, ctx.cop0[i.rd & 31u]); break;
        case MipsOp::MTC0: ctx.cop0[i.rd & 31u] = rt_val; break;
        case MipsOp::RFE:  ctx.cop0[12] = (ctx.cop0[12] & ~0x0Fu) | ((ctx.cop0[12] >> 2) & 0x0Fu); break;

        case MipsOp::MFC2: ctx.set_reg(i.rt, gte_.read_data(i.rd)); break;
        case MipsOp::CFC2: ctx.set_reg(i.rt, gte_.read_ctrl(i.rd)); break;
        case MipsOp::MTC2: gte_.write_data(i.rd, rt_val); break;
        case MipsOp::CTC2: gte_.write_ctrl(i.rd, rt_val); break;
        case MipsOp::LWC2: gte_.write_data(i.rt, read32(rs_val + static_cast<u32>(i.simm16()))); break;
        case MipsOp::SWC2: write32(rs_val + static_cast<u32>(i.simm16()), gte_.read_data(i.rt)); break;
        case MipsOp::GTE_CMD: gte_.execute(word); break;

        case MipsOp::INVALID:
        default:
            ctx.halted = true;
            break;
    }
}

u64 PsxRuntime::run(CpuContext& ctx, u64 max_steps) {
    const u64 start      = ctx.cycles;
    const u64 prev_limit = cycle_limit_;
    cycle_limit_         = start + max_steps;

    while (!ctx.halted && (ctx.cycles - start) < max_steps) {
        if (pending_aot_call_pc_ != 0 && ctx.next_pc == ctx.pc + 4u && to_kseg0(ctx.pc) == pending_aot_call_pc_) {
            pending_aot_call_pc_ = 0;
            if (RecompFunc aot = find_aot_func(to_kseg0(ctx.pc))) {
                ++aot_dispatches_;
                aot(ctx, *this);
                poll_hardware(ctx);
                continue;
            }
        }
        step_interpreter(ctx);
    }

    cycle_limit_ = prev_limit;
    return ctx.cycles - start;
}

} // namespace vsport
