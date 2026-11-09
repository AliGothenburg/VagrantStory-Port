#include "vsport/psx_spu.hpp"

#include <algorithm>
#include <fstream>

namespace vsport {

namespace {

// Official Sony PS1 SPU ADPCM IIR prediction filter coefficients (scaled by 64)
constexpr s32 ADPCM_POS_FILTER[5] = { 0,  60, 115,  98, 122 };
constexpr s32 ADPCM_NEG_FILTER[5] = { 0,   0, -52, -55, -60 };

s16 clamp_s16(s32 v) noexcept {
    if (v < -32768) return -32768;
    if (v >  32767) return  32767;
    return static_cast<s16>(v);
}

void write_le16_stream(std::ofstream& out, u16 v) {
    const char b[2] = {
        static_cast<char>(v & 0xFFu),
        static_cast<char>((v >> 8) & 0xFFu)
    };
    out.write(b, 2);
}

void write_le32_stream(std::ofstream& out, u32 v) {
    const char b[4] = {
        static_cast<char>(v & 0xFFu),
        static_cast<char>((v >> 8) & 0xFFu),
        static_cast<char>((v >> 16) & 0xFFu),
        static_cast<char>((v >> 24) & 0xFFu)
    };
    out.write(b, 4);
}

} // namespace

PsxSpu::PsxSpu() : ram_(SPU_RAM_SIZE, 0) {
    reset();
}

void PsxSpu::reset() {
    std::fill(ram_.begin(), ram_.end(), static_cast<u8>(0));
    regs_.fill(0);
    for (auto& v : voices_) {
        v = Voice{};
    }

    main_vol_l_      = 0x3FFFu;
    main_vol_r_      = 0x3FFFu;
    cd_vol_l_        = 0x7FFFu;
    cd_vol_r_        = 0x7FFFu;
    spucnt_          = 0;
    spustat_         = 0;
    xfer_ctrl_       = 0x0004u;
    xfer_addr_reg_   = 0;
    xfer_cur_addr_   = 0;
    irq_addr_reg_    = 0;
    endx_mask_       = 0x00FFFFFFu;
    irq_pending_     = false;

    xa_hist_l1_      = 0;
    xa_hist_l2_      = 0;
    xa_hist_r1_      = 0;
    xa_hist_r2_      = 0;
    cd_audio_fifo_.clear();

    cycle_accum_        = 0;
    bytes_uploaded_     = 0;
    key_on_events_      = 0;
    xa_sectors_decoded_ = 0;
    frames_rendered_    = 0;
    pcm_fifo_.clear();
}

s16 PsxSpu::decode_vol_reg(u16 reg) noexcept {
    if ((reg & 0x8000u) == 0) {
        // Fixed volume mode: 15-bit signed (-0x4000..+0x3FFF -> -0x8000..+0x7FFE)
        return static_cast<s16>(reg << 1);
    }
    // Sweep mode fallback: approximate target amplitude from low 7 bits
    const s32 level = static_cast<s32>(reg & 0x007Fu) << 8;
    return static_cast<s16>(std::min<s32>(0x7FFFu, level));
}

bool PsxSpu::check_irq_addr(u32 addr) noexcept {
    if ((spucnt_ & 0x0040u) == 0) {
        return false;
    }
    const u32 watch_addr = (static_cast<u32>(irq_addr_reg_) * 8u) & 0x7FFFFu;
    if (((addr ^ watch_addr) & ~0x0Fu) == 0) {
        spustat_ |= 0x0040u; // Bit 6: IRQ9 flag
        irq_pending_ = true;
        return true;
    }
    return false;
}

u16 PsxSpu::read_reg16(u32 paddr) {
    if (paddr < 0x1F801C00u || paddr >= 0x1F801E00u) {
        return 0;
    }
    const u32 off = paddr - 0x1F801C00u;

    // Voice registers 0..23 (0x1F801C00..0x1F801D7F)
    if (off < 0x180u) {
        const u32 v_idx = off >> 4;
        const u32 v_reg = off & 0x0Eu;
        const auto& v   = voices_[v_idx];
        switch (v_reg) {
            case 0x00: return v.vol_left_reg;
            case 0x02: return v.vol_right_reg;
            case 0x04: return v.pitch;
            case 0x06: return v.start_addr_reg;
            case 0x08: return v.adsr_lo;
            case 0x0A: return v.adsr_hi;
            case 0x0C: return static_cast<u16>(v.adsr_vol & 0xFFFF);
            case 0x0E: return v.repeat_addr_reg;
            default:   break;
        }
    }

    switch (paddr) {
        case 0x1F801D80u: return main_vol_l_;
        case 0x1F801D82u: return main_vol_r_;
        case 0x1F801D9Cu: return static_cast<u16>(endx_mask_ & 0xFFFFu);
        case 0x1F801D9Eu: return static_cast<u16>((endx_mask_ >> 16) & 0x00FFu);
        case 0x1F801DA4u: return irq_addr_reg_;
        case 0x1F801DA6u: return xfer_addr_reg_;
        case 0x1F801DAAu: return spucnt_;
        case 0x1F801DACu: return xfer_ctrl_;
        case 0x1F801DAEu:
            // SPUSTAT: bits 0..5 mirror SPUCNT 0..5, bit 6 = IRQ9 flag, bit 10 = busy(0)
            return static_cast<u16>((spustat_ & 0x0040u) | (spucnt_ & 0x003Fu));
        default:
            break;
    }

    // Current voice volume readback mirrors (0x1F801DC0+ or 0x1F801E00 in extended map)
    return regs_[(off >> 1) & 0xFFu];
}

void PsxSpu::write_reg16(u32 paddr, u16 val) {
    if (paddr < 0x1F801C00u || paddr >= 0x1F801E00u) {
        return;
    }
    const u32 off = paddr - 0x1F801C00u;
    regs_[(off >> 1) & 0xFFu] = val;

    if (off < 0x180u) {
        const u32 v_idx = off >> 4;
        const u32 v_reg = off & 0x0Eu;
        auto& v         = voices_[v_idx];
        switch (v_reg) {
            case 0x00: v.vol_left_reg    = val; break;
            case 0x02: v.vol_right_reg   = val; break;
            case 0x04: v.pitch           = val; break;
            case 0x06: v.start_addr_reg  = val; break;
            case 0x08: v.adsr_lo         = val; break;
            case 0x0A: v.adsr_hi         = val; break;
            case 0x0C: v.adsr_vol        = static_cast<s32>(val & 0x7FFFu); break;
            case 0x0E: v.repeat_addr_reg = val; break;
            default:   break;
        }
        return;
    }

    switch (paddr) {
        case 0x1F801D80u: main_vol_l_ = val; break;
        case 0x1F801D82u: main_vol_r_ = val; break;
        case 0x1F801D88u: // KON Voices 0..15
            for (u32 i = 0; i < 16u; ++i) {
                if ((val & (1u << i)) != 0) key_on_voice(i);
            }
            break;
        case 0x1F801D8Au: // KON Voices 16..23
            for (u32 i = 0; i < 8u; ++i) {
                if ((val & (1u << i)) != 0) key_on_voice(16u + i);
            }
            break;
        case 0x1F801D8Cu: // KOFF Voices 0..15
            for (u32 i = 0; i < 16u; ++i) {
                if ((val & (1u << i)) != 0) key_off_voice(i);
            }
            break;
        case 0x1F801D8Eu: // KOFF Voices 16..23
            for (u32 i = 0; i < 8u; ++i) {
                if ((val & (1u << i)) != 0) key_off_voice(16u + i);
            }
            break;
        case 0x1F801DA4u:
            irq_addr_reg_ = val;
            break;
        case 0x1F801DA6u:
            xfer_addr_reg_ = val;
            xfer_cur_addr_ = (static_cast<u32>(val) * 8u) & 0x7FFFFu;
            break;
        case 0x1F801DA8u: // SPU RAM Manual Transfer FIFO
            ram_[xfer_cur_addr_ & 0x7FFFFu]        = static_cast<u8>(val & 0xFFu);
            ram_[(xfer_cur_addr_ + 1u) & 0x7FFFFu] = static_cast<u8>((val >> 8) & 0xFFu);
            (void)check_irq_addr(xfer_cur_addr_);
            xfer_cur_addr_ = (xfer_cur_addr_ + 2u) & 0x7FFFFu;
            bytes_uploaded_ += 2u;
            break;
        case 0x1F801DAAu:
            spucnt_ = val;
            if ((spucnt_ & 0x0040u) == 0 || (spucnt_ & 0x8000u) == 0) {
                // Disabling IRQ9 (bit 6) or SPU Enable (bit 15) acknowledges IRQ9 flag
                spustat_ &= ~0x0040u;
            }
            break;
        case 0x1F801DACu:
            xfer_ctrl_ = val;
            break;
        case 0x1F801DB0u:
            cd_vol_l_ = static_cast<s16>(val);
            break;
        case 0x1F801DB2u:
            cd_vol_r_ = static_cast<s16>(val);
            break;
        default:
            break;
    }
}

void PsxSpu::write_dma_word(u32 word) {
    write_reg16(0x1F801DA8u, static_cast<u16>(word & 0xFFFFu));
    write_reg16(0x1F801DA8u, static_cast<u16>((word >> 16) & 0xFFFFu));
}

u32 PsxSpu::read_dma_word() {
    u32 w = 0;
    for (u32 b = 0; b < 4u; ++b) {
        w |= static_cast<u32>(ram_[(xfer_cur_addr_ + b) & 0x7FFFFu]) << (b * 8u);
    }
    (void)check_irq_addr(xfer_cur_addr_);
    xfer_cur_addr_ = (xfer_cur_addr_ + 4u) & 0x7FFFFu;
    return w;
}

void PsxSpu::push_cd_audio_sector_xa(const u8* raw_sector_2352) {
    if (raw_sector_2352 == nullptr) return;
    const u8 coding    = raw_sector_2352[19];
    const bool stereo  = (coding & 0x03u) == 1u;
    const bool half_hz = (coding & 0x0Cu) == 4u; // 1 = 18.9 kHz, 0 = 37.8 kHz
    const bool bits8   = (coding & 0x30u) != 0;
    if (bits8) return; // Vagrant Story uses standard 4-bit XA ADPCM

    std::vector<s16> raw_lr;
    raw_lr.reserve(4032);

    auto decode_unit = [](const u8* grp, u32 su, s32& h1, s32& h2, s16* out28) {
        const u8 param = grp[4u + su];
        u32 shift      = param & 0x0Fu;
        if (shift > 12u) shift = 9u;
        const u32 flt  = (param >> 4) & 0x03u;
        const s32 f0   = ADPCM_POS_FILTER[flt];
        const s32 f1   = ADPCM_NEG_FILTER[flt];

        for (u32 s = 0; s < 28u; ++s) {
            const u8 b   = grp[16u + s * 4u + (su >> 1)];
            const u8 nib = ((su & 1u) == 0) ? (b & 0x0Fu) : ((b >> 4) & 0x0Fu);
            const s32 sr = static_cast<s32>(static_cast<s16>(static_cast<u16>(nib) << 12)) >> shift;
            const s32 pr = (h1 * f0 + h2 * f1 + 32) >> 6;
            const s16 p  = clamp_s16(sr + pr);
            h2           = h1;
            h1           = p;
            out28[s]     = p;
        }
    };

    for (u32 sg = 0; sg < 18u; ++sg) {
        const u8* grp = raw_sector_2352 + 24u + sg * 128u;
        if (stereo) {
            s16 l28[28]{}, r28[28]{};
            for (u32 pair = 0; pair < 4u; ++pair) {
                decode_unit(grp, pair * 2u + 0u, xa_hist_l1_, xa_hist_l2_, l28);
                decode_unit(grp, pair * 2u + 1u, xa_hist_r1_, xa_hist_r2_, r28);
                for (u32 s = 0; s < 28u; ++s) {
                    raw_lr.push_back(l28[s]);
                    raw_lr.push_back(r28[s]);
                }
            }
        } else {
            s16 m28[28]{};
            for (u32 su = 0; su < 8u; ++su) {
                decode_unit(grp, su, xa_hist_l1_, xa_hist_l2_, m28);
                for (u32 s = 0; s < 28u; ++s) {
                    raw_lr.push_back(m28[s]);
                    raw_lr.push_back(m28[s]);
                }
            }
        }
    }

    // Resample from 37,800 Hz (6:7) or 18,900 Hz (3:7) to 44,100 Hz
    const std::size_t in_frames  = raw_lr.size() / 2u;
    const std::size_t num_num    = half_hz ? 3u : 6u;
    const std::size_t out_frames = (in_frames * 7u) / num_num;

    if (cd_audio_fifo_.size() > SAMPLE_RATE * 8u) {
        cd_audio_fifo_.clear();
    }

    for (std::size_t i = 0; i < out_frames; ++i) {
        const std::size_t src_pos_fp = (i * num_num * 4096u) / 7u;
        const std::size_t idx0       = std::min(in_frames - 1u, src_pos_fp >> 12);
        const std::size_t idx1       = std::min(in_frames - 1u, idx0 + 1u);
        const s32 frac               = static_cast<s32>(src_pos_fp & 0xFFFu);

        const s32 l0 = raw_lr[idx0 * 2u + 0u], l1 = raw_lr[idx1 * 2u + 0u];
        const s32 r0 = raw_lr[idx0 * 2u + 1u], r1 = raw_lr[idx1 * 2u + 1u];

        cd_audio_fifo_.push_back(clamp_s16(l0 + (((l1 - l0) * frac) >> 12)));
        cd_audio_fifo_.push_back(clamp_s16(r0 + (((r1 - r0) * frac) >> 12)));
    }

    ++xa_sectors_decoded_;
}

void PsxSpu::key_on_voice(u32 v_idx) {
    if (v_idx >= NUM_VOICES) return;
    auto& v           = voices_[v_idx];
    v.cur_addr        = (static_cast<u32>(v.start_addr_reg) * 8u) & 0x7FFF0u;
    v.phase_accum     = 0;
    v.hist1           = 0;
    v.hist2           = 0;
    v.prev_last       = 0;
    v.adsr_vol        = 0;
    v.adsr_ticks      = 0;
    v.env_phase       = EnvPhase::Attack;
    v.active          = true;
    endx_mask_ &= ~(1u << v_idx);
    ++key_on_events_;
    decode_adpcm_block(v_idx);
}

void PsxSpu::key_off_voice(u32 v_idx) {
    if (v_idx >= NUM_VOICES) return;
    auto& v = voices_[v_idx];
    if (v.active && v.env_phase != EnvPhase::Off) {
        v.env_phase  = EnvPhase::Release;
        v.adsr_ticks = 0;
    }
}

void PsxSpu::decode_adpcm_block(u32 v_idx) {
    auto& v = voices_[v_idx];
    const u32 base = v.cur_addr & 0x7FFF0u;
    (void)check_irq_addr(base);

    const u8 shift_filter = ram_[base + 0u];
    const u8 flags        = ram_[base + 1u];
    v.block_flags         = flags;

    // Flag bit 2 (0x04): Loop Start — latch current block address into repeat_addr_reg
    if ((flags & 0x04u) != 0) {
        v.repeat_addr_reg = static_cast<u16>(base >> 3);
    }

    u32 shift  = shift_filter & 0x0Fu;
    if (shift > 12u) shift = 9u; // Hardware undocumented behavior >12 behaves like 9
    u32 filter = (shift_filter >> 4) & 0x07u;
    if (filter > 4u) filter = 4u;

    const s32 f0 = ADPCM_POS_FILTER[filter];
    const s32 f1 = ADPCM_NEG_FILTER[filter];

    v.prev_last = v.block_pcm[27];

    for (u32 i = 0; i < 28u; ++i) {
        const u8 packed = ram_[base + 2u + (i >> 1)];
        const u8 nib    = ((i & 1u) == 0) ? (packed & 0x0Fu) : ((packed >> 4) & 0x0Fu);
        // Sign-extend 4-bit nibble to 16-bit and apply shift
        const s32 s_raw = static_cast<s32>(static_cast<s16>(static_cast<u16>(nib) << 12)) >> shift;
        const s32 pred  = (v.hist1 * f0 + v.hist2 * f1 + 32) >> 6;
        const s16 pcm   = clamp_s16(s_raw + pred);

        v.hist2        = v.hist1;
        v.hist1        = pcm;
        v.block_pcm[i] = pcm;
    }
}

void PsxSpu::step_adsr(Voice& v) {
    if (v.env_phase == EnvPhase::Off) {
        v.adsr_vol = 0;
        v.active   = false;
        return;
    }

    ++v.adsr_ticks;

    auto apply_rate = [&](u32 rate_shift, s32 step_base, bool decreasing, bool exponential) {
        const u32 interval = (rate_shift >= 11u) ? (1u << (rate_shift - 11u)) : 1u;
        if ((v.adsr_ticks % interval) != 0) {
            return;
        }
        s32 delta = (rate_shift < 11u) ? (step_base << (11u - rate_shift)) : step_base;
        if (exponential) {
            if (!decreasing && v.adsr_vol > 0x6000) {
                delta = std::max<s32>(1, delta >> 2);
            } else if (decreasing) {
                delta = (delta * v.adsr_vol) >> 15;
                if (delta == 0 && v.adsr_vol > 0) delta = 1;
            }
        }
        v.adsr_vol = decreasing ? std::max<s32>(0, v.adsr_vol - delta)
                                : std::min<s32>(0x7FFF, v.adsr_vol + delta);
    };

    switch (v.env_phase) {
        case EnvPhase::Attack: {
            const u32 ar      = (v.adsr_lo >> 8) & 0x7Fu;
            const bool exp_m  = (v.adsr_lo & 0x8000u) != 0;
            const u32 shift   = (ar >> 2) & 0x1Fu;
            const s32 step    = static_cast<s32>(7u - (ar & 3u));
            apply_rate(shift, step, false, exp_m);
            if (v.adsr_vol >= 0x7FFF) {
                v.adsr_vol   = 0x7FFF;
                v.env_phase  = EnvPhase::Decay;
                v.adsr_ticks = 0;
            }
            break;
        }
        case EnvPhase::Decay: {
            const u32 dr_sh  = (v.adsr_lo >> 4) & 0x0Fu;
            const u32 sl_idx = v.adsr_lo & 0x0Fu;
            const s32 target = std::min<s32>(0x7FFF, static_cast<s32>((sl_idx + 1u) << 11) - 1);
            apply_rate(dr_sh, 8, true, true);
            if (v.adsr_vol <= target) {
                v.adsr_vol   = target;
                v.env_phase  = EnvPhase::Sustain;
                v.adsr_ticks = 0;
            }
            break;
        }
        case EnvPhase::Sustain: {
            const u32 sr     = (v.adsr_hi >> 6) & 0x7Fu;
            const bool dec   = (v.adsr_hi & 0x4000u) != 0;
            const bool exp_m = (v.adsr_hi & 0x8000u) != 0;
            const u32 shift  = (sr >> 2) & 0x1Fu;
            const s32 step   = dec ? static_cast<s32>(8u - (sr & 3u))
                                   : static_cast<s32>(7u - (sr & 3u));
            if (shift < 31u) {
                apply_rate(shift, step, dec, exp_m);
            }
            break;
        }
        case EnvPhase::Release: {
            const u32 rr_sh  = v.adsr_hi & 0x1Fu;
            const bool exp_m = (v.adsr_hi & 0x0020u) != 0;
            apply_rate(rr_sh, 8, true, exp_m);
            if (v.adsr_vol <= 0) {
                v.adsr_vol  = 0;
                v.env_phase = EnvPhase::Off;
                v.active    = false;
            }
            break;
        }
        case EnvPhase::Off:
            break;
    }
}

void PsxSpu::mix_one_sample() {
    s32 mix_l = 0;
    s32 mix_r = 0;

    const bool spu_enabled = (spucnt_ & 0x8000u) != 0;
    const bool unmuted     = (spucnt_ & 0x4000u) != 0;

    if (spu_enabled) {
        for (u32 i = 0; i < NUM_VOICES; ++i) {
            auto& v = voices_[i];
            if (!v.active) continue;

            step_adsr(v);
            if (!v.active) continue;

            const u32 s_idx = v.phase_accum >> 12;
            const s32 frac  = static_cast<s32>(v.phase_accum & 0xFFFu);
            const s32 s0    = (s_idx == 0) ? v.prev_last : v.block_pcm[s_idx - 1u];
            const s32 s1    = v.block_pcm[std::min<u32>(27u, s_idx)];
            const s32 interp = s0 + (((s1 - s0) * frac) >> 12);

            const s32 env_sample = (interp * v.adsr_vol) >> 15;
            const s32 vl         = decode_vol_reg(v.vol_left_reg);
            const s32 vr         = decode_vol_reg(v.vol_right_reg);

            mix_l += (env_sample * vl) >> 15;
            mix_r += (env_sample * vr) >> 15;

            // Advance pitch counter (capped at 0x4000 = 4x 44.1 kHz per hardware spec)
            const u32 step = std::min<u32>(v.pitch, 0x4000u);
            v.phase_accum += step;

            while (v.phase_accum >= (28u << 12)) {
                v.phase_accum -= (28u << 12);
                if ((v.block_flags & 0x01u) != 0) {
                    // Loop End reached!
                    endx_mask_ |= (1u << i);
                    v.cur_addr = (static_cast<u32>(v.repeat_addr_reg) * 8u) & 0x7FFF0u;
                    if ((v.block_flags & 0x02u) == 0) {
                        // One-shot non-repeating block: mute envelope immediately
                        v.adsr_vol  = 0;
                        v.env_phase = EnvPhase::Off;
                        v.active    = false;
                        break;
                    }
                } else {
                    v.cur_addr = (v.cur_addr + 16u) & 0x7FFF0u;
                }
                decode_adpcm_block(i);
            }
        }
    }

    // Mix CD-ROM XA ADPCM Stereo stream when SPUCNT bit 0 (CD Audio Enable) is active
    if (!cd_audio_fifo_.empty()) {
        const s32 cdl = cd_audio_fifo_.front(); cd_audio_fifo_.pop_front();
        const s32 cdr = cd_audio_fifo_.front(); cd_audio_fifo_.pop_front();
        if ((spucnt_ & 0x0001u) != 0) {
            mix_l += (cdl * static_cast<s32>(cd_vol_l_)) >> 15;
            mix_r += (cdr * static_cast<s32>(cd_vol_r_)) >> 15;
        }
    }

    if (!unmuted) {
        mix_l = 0;
        mix_r = 0;
    } else {
        const s32 mvl = decode_vol_reg(main_vol_l_);
        const s32 mvr = decode_vol_reg(main_vol_r_);
        mix_l = (mix_l * mvl) >> 15;
        mix_r = (mix_r * mvr) >> 15;
    }

    const s16 out_l = clamp_s16(mix_l);
    const s16 out_r = clamp_s16(mix_r);

    if (pcm_fifo_.size() >= MAX_BUFFERED_STEREO_FRAMES * 2u) {
        pcm_fifo_.pop_front();
        pcm_fifo_.pop_front();
    }
    pcm_fifo_.push_back(out_l);
    pcm_fifo_.push_back(out_r);
    ++frames_rendered_;
}

bool PsxSpu::tick(u64 elapsed_cycles) {
    cycle_accum_ += elapsed_cycles;
    while (cycle_accum_ >= CYCLES_PER_SAMPLE) {
        cycle_accum_ -= CYCLES_PER_SAMPLE;
        mix_one_sample();
    }
    if (irq_pending_) {
        irq_pending_ = false;
        return true;
    }
    return false;
}

std::size_t PsxSpu::pop_samples(std::vector<s16>& out_interleaved_lr, std::size_t max_stereo_frames) {
    const std::size_t avail_frames = pcm_fifo_.size() / 2u;
    const std::size_t take_frames  = std::min(avail_frames, max_stereo_frames);
    out_interleaved_lr.resize(take_frames * 2u);
    for (std::size_t i = 0; i < take_frames * 2u; ++i) {
        out_interleaved_lr[i] = pcm_fifo_.front();
        pcm_fifo_.pop_front();
    }
    return take_frames;
}

bool PsxSpu::dump_wav(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return false;
    }

    const u32 num_samples = static_cast<u32>(pcm_fifo_.size()); // Interleaved L + R samples
    const u32 data_bytes  = num_samples * sizeof(s16);
    const u32 riff_size   = 36u + data_bytes;

    out.write("RIFF", 4);
    write_le32_stream(out, riff_size);
    out.write("WAVE", 4);

    // fmt chunk (16 bytes PCM)
    out.write("fmt ", 4);
    write_le32_stream(out, 16u);
    write_le16_stream(out, 1u);                // AudioFormat = 1 (PCM)
    write_le16_stream(out, 2u);                // NumChannels = 2 (Stereo)
    write_le32_stream(out, SAMPLE_RATE);       // SampleRate  = 44100
    write_le32_stream(out, SAMPLE_RATE * 4u);  // ByteRate    = 44100 * 2 * 2
    write_le16_stream(out, 4u);                // BlockAlign  = 4
    write_le16_stream(out, 16u);               // BitsPerSample = 16

    // data chunk
    out.write("data", 4);
    write_le32_stream(out, data_bytes);
    for (s16 s : pcm_fifo_) {
        write_le16_stream(out, static_cast<u16>(s));
    }
    return out.good();
}

} // namespace vsport
