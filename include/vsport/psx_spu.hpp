#pragma once

#include "vsport/types.hpp"

#include <array>
#include <deque>
#include <filesystem>
#include <vector>

namespace vsport {

// PlayStation 1 SPU (24-Voice Sony 4-bit ADPCM Hardware Synthesizer + 512 KB Sound RAM + DMA4)
class PsxSpu {
public:
    static constexpr u32 SPU_RAM_SIZE = 512u * 1024u; // 512 KB
    static constexpr u32 NUM_VOICES   = 24u;
    static constexpr u32 SAMPLE_RATE  = 44100u;
    static constexpr u32 CYCLES_PER_SAMPLE = 768u;    // 33,868,800 Hz / 44,100 Hz

    PsxSpu();
    void reset();

    // 16-bit MMIO access for 0x1F801C00..0x1F801DFF
    [[nodiscard]] u16 read_reg16(u32 paddr);
    void write_reg16(u32 paddr, u16 val);

    // DMA4 (SPU <-> PSX RAM) 32-bit word port
    void write_dma_word(u32 word);
    [[nodiscard]] u32 read_dma_word();

    // Decodes a 2352-byte Mode 2 Form 2 CD-ROM XA ADPCM sector into the 44.1 kHz CD-Audio mixer
    void push_cd_audio_sector_xa(const u8* raw_sector_2352);

    // Advances hardware voices & mixes 44.1 kHz stereo PCM; returns true if SPU IRQ9 triggered
    bool tick(u64 elapsed_cycles);

    // Drains up to max_stereo_frames (interleaved L, R s16 pairs) from the output queue
    std::size_t pop_samples(std::vector<s16>& out_interleaved_lr, std::size_t max_stereo_frames);

    // Exports buffered stereo audio to a 16-bit 44.1 kHz RIFF WAV file
    bool dump_wav(const std::filesystem::path& path) const;

    // Diagnostics & telemetry
    [[nodiscard]] u64 bytes_uploaded() const noexcept { return bytes_uploaded_; }
    [[nodiscard]] u64 key_on_count() const noexcept { return key_on_events_; }
    [[nodiscard]] u64 xa_sectors_decoded() const noexcept { return xa_sectors_decoded_; }
    [[nodiscard]] u64 frames_rendered() const noexcept { return frames_rendered_; }
    [[nodiscard]] std::size_t buffered_frames() const noexcept { return pcm_fifo_.size() / 2u; }
    [[nodiscard]] const std::vector<u8>& sound_ram() const noexcept { return ram_; }

private:
    enum class EnvPhase : u8 {
        Off = 0,
        Attack,
        Decay,
        Sustain,
        Release
    };

    struct Voice {
        u16 vol_left_reg    = 0;
        u16 vol_right_reg   = 0;
        u16 pitch           = 0x1000u;
        u16 start_addr_reg  = 0;
        u16 adsr_lo         = 0;
        u16 adsr_hi         = 0;
        u16 repeat_addr_reg = 0;

        s32 adsr_vol        = 0; // 0..0x7FFF
        u32 adsr_ticks      = 0;
        EnvPhase env_phase  = EnvPhase::Off;
        bool active         = false;

        u32 cur_addr        = 0; // Byte offset in 512 KB SPU RAM (16-byte aligned)
        u32 phase_accum     = 0; // 12-bit fractional position (0..28*4096)
        s32 hist1           = 0;
        s32 hist2           = 0;
        u8  block_flags     = 0;
        s16 prev_last       = 0;
        std::array<s16, 28> block_pcm{};
    };

    void key_on_voice(u32 v_idx);
    void key_off_voice(u32 v_idx);
    void decode_adpcm_block(u32 v_idx);
    void step_adsr(Voice& v);
    void mix_one_sample();
    [[nodiscard]] bool check_irq_addr(u32 addr) noexcept;

    static s16 decode_vol_reg(u16 reg) noexcept;

    std::vector<u8> ram_;
    std::array<u16, 256> regs_{};
    std::array<Voice, NUM_VOICES> voices_{};

    u16 main_vol_l_    = 0x3FFFu;
    u16 main_vol_r_    = 0x3FFFu;
    s16 cd_vol_l_      = 0x7FFFu;
    s16 cd_vol_r_      = 0x7FFFu;
    u16 spucnt_        = 0;
    u16 spustat_       = 0;
    u16 xfer_ctrl_     = 0x0004u;
    u16 xfer_addr_reg_ = 0;
    u32 xfer_cur_addr_ = 0;
    u16 irq_addr_reg_  = 0;
    u32 endx_mask_     = 0;
    bool irq_pending_  = false;

    s32 xa_hist_l1_    = 0;
    s32 xa_hist_l2_    = 0;
    s32 xa_hist_r1_    = 0;
    s32 xa_hist_r2_    = 0;
    std::deque<s16> cd_audio_fifo_;

    u64 cycle_accum_        = 0;
    u64 bytes_uploaded_     = 0;
    u64 key_on_events_      = 0;
    u64 xa_sectors_decoded_ = 0;
    u64 frames_rendered_    = 0;

    static constexpr std::size_t MAX_BUFFERED_STEREO_FRAMES = SAMPLE_RATE * 12u; // Cap at 12 sec
    std::deque<s16> pcm_fifo_;
};

} // namespace vsport
