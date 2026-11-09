#include "vsport/psx_cdrom.hpp"
#include "vsport/psx_spu.hpp"

#include <algorithm>
#include <cstring>

namespace vsport {

void PsxCdrom::reset() {
    index_            = 0;
    ie_               = 0x1Fu; // Post-BIOS boot state: CD-ROM interrupts enabled after loading PS-X EXE
    irq_flag_         = 0;
    stat_             = 0x02; // Motor on, disc present
    mode_             = 0;
    filter_file_      = 0;
    filter_chan_      = 0;
    muted_            = false;
    param_fifo_.clear();
    resp_fifo_.clear();
    pending_irqs_.clear();
    has_pending_seek_ = false;
    seek_lba_         = 0;
    read_lba_         = 0;
    last_read_lba_    = 0;
    reading_          = false;
    read_timer_       = 0;
    raw_sector_.fill(0);
    data_fifo_.clear();
    data_pos_         = 0;
    sector_seq_       = 0;
    fifo_sector_seq_  = 0;
    bfrd_             = false;
    sectors_read_     = 0;
}

u8 PsxCdrom::read_reg8(u32 port_offset) {
    switch (port_offset & 3u) {
        case 0: { // 0x1F801800: Status Register
            u8 st = index_ & 3u;
            if (param_fifo_.empty())       st |= (1u << 3); // PRMEMPT
            if (param_fifo_.size() < 16)   st |= (1u << 4); // PRMWRDY
            if (!resp_fifo_.empty())       st |= (1u << 5); // RSLRRDY
            if (data_pos_ < data_fifo_.size()) st |= (1u << 6); // DRQSTS
            return st;
        }
        case 1: { // 0x1F801801: Response FIFO (all indices)
            if (resp_fifo_.empty()) return 0;
            const u8 v = resp_fifo_.front();
            resp_fifo_.pop_front();
            return v;
        }
        case 2: { // 0x1F801802: Data FIFO (all indices)
            if (data_pos_ < data_fifo_.size()) {
                return data_fifo_[data_pos_++];
            }
            return 0;
        }
        case 3: { // 0x1F801803: IE (Index 0,2) or IF (Index 1,3)
            if ((index_ & 1u) == 0) {
                return static_cast<u8>(0xE0u | (ie_ & 0x1Fu));
            }
            return static_cast<u8>(0xE0u | (irq_flag_ & 0x1Fu));
        }
    }
    return 0;
}

void PsxCdrom::write_reg8(u32 port_offset, u8 val) {
    switch (port_offset & 3u) {
        case 0: // 0x1F801800: Index selector
            index_ = val & 3u;
            break;

        case 1: // 0x1F801801
            if (index_ == 0) {
                execute_command(val);
            }
            break;

        case 2: // 0x1F801802
            if (index_ == 0) {
                if (param_fifo_.size() < 16) {
                    param_fifo_.push_back(val);
                }
            } else if (index_ == 1) {
                ie_ = val & 0x1Fu;
            }
            break;

        case 3: // 0x1F801803
            if ((index_ & 1u) == 0) {
                // Request Register (Index 0 / 2): bit 7 (BFRD, "want data") transfers the drive's
                // newest sector into the host data FIFO on its 0 -> 1 transition; writing 1 while it
                // is already 1 changes nothing, and writing 0 resets the FIFO. libcd clears and sets
                // BFRD once per sector and then sets it again for each CdGetSector() (header DMA,
                // then data DMA): a later sector arriving in between must not replace the FIFO.
                if (index_ == 0) {
                    const bool want = (val & 0x80u) != 0;
                    if (want && !bfrd_) {
                        load_current_sector_buffer();
                        fifo_sector_seq_ = sector_seq_;
                    } else if (!want) {
                        data_fifo_.clear();
                        data_pos_ = 0;
                    }
                    bfrd_ = want;
                }
            } else {
                // Interrupt Flag Acknowledge (Index 1 / 3)
                irq_flag_ &= static_cast<u8>(~(val & 0x1Fu));
                if ((val & 0x40u) != 0) {
                    param_fifo_.clear();
                }
            }
            break;
    }
}

u32 PsxCdrom::read_data_word() {
    u32 w = 0;
    for (u32 b = 0; b < 4; ++b) {
        const u8 byte_val = (data_pos_ < data_fifo_.size()) ? data_fifo_[data_pos_++] : 0u;
        w |= (static_cast<u32>(byte_val) << (b * 8u));
    }
    return w;
}

void PsxCdrom::load_current_sector_buffer() {
    data_fifo_.clear();
    data_pos_ = 0;

    // Bit 5 of Setmode: 0 = 2048-byte user data, 1 = 2340-byte whole sector (starting after 12-byte sync)
    if ((mode_ & 0x20u) != 0) {
        data_fifo_.assign(raw_sector_.begin() + 12, raw_sector_.begin() + 12 + 2340);
    } else {
        // Mode 2 Form 1 user data starts at +24; Mode 1 starts at +16
        const std::size_t off = (raw_sector_[15] == 1) ? 16u : 24u;
        data_fifo_.assign(raw_sector_.begin() + static_cast<std::ptrdiff_t>(off),
                          raw_sector_.begin() + static_cast<std::ptrdiff_t>(off + 2048u));
    }
}

void PsxCdrom::queue_response(u8 irq_type, std::initializer_list<u8> bytes, u64 delay_cycles) {
    QueuedResponse qr{};
    qr.irq_type     = irq_type;
    qr.bytes.assign(bytes.begin(), bytes.end());
    qr.delay_cycles = delay_cycles;
    pending_irqs_.push_back(std::move(qr));
}

bool PsxCdrom::promote_next_irq() {
    if (irq_flag_ != 0 || pending_irqs_.empty()) {
        return false;
    }

    auto& front = pending_irqs_.front();
    if (front.delay_cycles > 0) {
        return false;
    }

    irq_flag_ = front.irq_type & 0x1Fu;
    resp_fifo_.clear();
    for (u8 b : front.bytes) {
        resp_fifo_.push_back(b);
    }
    pending_irqs_.pop_front();

    return (irq_flag_ & ie_) != 0;
}

bool PsxCdrom::tick(u64 elapsed_cycles) {
    bool fire_irq2 = false;

    if (!pending_irqs_.empty() && irq_flag_ == 0) {
        auto& front = pending_irqs_.front();
        if (front.delay_cycles > elapsed_cycles) {
            front.delay_cycles -= elapsed_cycles;
        } else {
            front.delay_cycles = 0;
            if (promote_next_irq()) {
                fire_irq2 = true;
            }
        }
    }

    if (reading_ && disc_ != nullptr) {
        // Use exact real-time 2x (225,792 cycles) / 1x (451,584 cycles) cadence during XA-ADPCM streaming,
        // and fast loading cadence for ordinary file data reads. A fast read still waits until the
        // CPU has taken the previous data sector (BFRD) or a real sector time has passed, so the
        // drive never runs ahead of a loader that fetches each sector in several DMA steps.
        const bool xa_streaming = (mode_ & 0x40u) != 0;
        const u64 real_period   = ((mode_ & 0x80u) != 0) ? 225792ull : 451584ull;
        const bool prev_fetched = fifo_sector_seq_ == sector_seq_;
        const u64 sector_period = (xa_streaming || !prev_fetched)
            ? real_period
            : (((mode_ & 0x80u) != 0) ? 12000ull : 24000ull);
        read_timer_ += elapsed_cycles;
        if (read_timer_ >= sector_period && pending_irqs_.empty() && irq_flag_ == 0) {
            read_timer_ = 0;
            disc_->read_sector_raw2352(read_lba_, raw_sector_.data());
            last_read_lba_ = read_lba_;
            ++read_lba_;
            ++sectors_read_;
            stat_ = 0x22u; // MotorOn | Reading

            const bool is_xa_audio = (raw_sector_[15] == 2u) && ((raw_sector_[18] & 0x44u) == 0x44u);
            if (xa_streaming && is_xa_audio) {
                const bool match_filter = ((mode_ & 0x08u) == 0) ||
                                          (raw_sector_[16] == filter_file_ && raw_sector_[17] == filter_chan_);
                if (match_filter && !muted_ && spu_ != nullptr) {
                    spu_->push_cd_audio_sector_xa(raw_sector_.data());
                }
                // Hardware routes XA-ADPCM audio sectors to SPU without raising CPU INT1 DataReady
            } else {
                // New data sector in the drive buffer: announce it with INT1(stat_). The host FIFO
                // keeps the previous sector until the CPU requests the new one with BFRD; code that
                // leaves BFRD set gets it loaded right away, but only once the previous sector has
                // been read out completely (never overwrite a FIFO the CPU is still draining).
                ++sector_seq_;
                if (bfrd_ && data_pos_ >= data_fifo_.size()) {
                    load_current_sector_buffer();
                    fifo_sector_seq_ = sector_seq_;
                }
                queue_response(0x01, {stat_}, 0);
                if (promote_next_irq()) {
                    fire_irq2 = true;
                }
            }
        }
    }

    return fire_irq2;
}

void PsxCdrom::execute_command(u8 cmd) {
    if (logged_cmds_ < 40) {
        char b[96]{};
        std::snprintf(b, sizeof(b), "CMD=0x%02X (params=%zu, ie=0x%02X, if=0x%02X)\n",
                      cmd, param_fifo_.size(), ie_, irq_flag_);
        debug_log_ += b;
        ++logged_cmds_;
    }

    auto pop_param = [&]() -> u8 {
        if (param_fifo_.empty()) return 0;
        const u8 v = param_fifo_.front();
        param_fifo_.pop_front();
        return v;
    };

    switch (cmd) {
        case 0x01: // CdlNop / Getstat
            queue_response(0x03, {stat_}, 800);
            break;

        case 0x02: { // CdlSetloc (amm, ass, asect in BCD)
            const u32 mm = bcd_to_bin(pop_param());
            const u32 ss = bcd_to_bin(pop_param());
            const u32 ff = bcd_to_bin(pop_param());
            const u32 total_frames = (mm * 60u + ss) * 75u + ff;
            seek_lba_         = (total_frames >= 150u) ? (total_frames - 150u) : 0u;
            has_pending_seek_ = true;
            queue_response(0x03, {stat_}, 800);
            break;
        }

        case 0x06: // CdlReadN
        case 0x1B: { // CdlReadS
            if (has_pending_seek_) {
                read_lba_         = seek_lba_;
                has_pending_seek_ = false;
            }
            reading_    = true;
            read_timer_ = 0;
            stat_       = 0x22u; // MotorOn | Reading
            queue_response(0x03, {stat_}, 800);
            break;
        }

        case 0x07: // CdlStandby
            stat_ = 0x02u;
            queue_response(0x03, {stat_}, 800);
            queue_response(0x02, {stat_}, 4000);
            break;

        case 0x08: // CdlStop
            reading_ = false;
            queue_response(0x03, {stat_}, 800);
            stat_ = 0x00u;
            queue_response(0x02, {stat_}, 4000);
            break;

        case 0x09: // CdlPause
            reading_ = false;
            queue_response(0x03, {stat_}, 800);
            stat_ = 0x02u;
            queue_response(0x02, {stat_}, 3000);
            break;

        case 0x0A: // CdlInit
            reading_ = false;
            stat_    = 0x02u;
            mode_    = 0x00u;
            queue_response(0x03, {stat_}, 800);
            queue_response(0x02, {stat_}, 4000);
            break;

        case 0x0B: // CdlMute
            muted_ = true;
            queue_response(0x03, {stat_}, 800);
            break;
        case 0x0C: // CdlDemute
            muted_ = false;
            queue_response(0x03, {stat_}, 800);
            break;

        case 0x0D: // CdlSetfilter
            filter_file_ = pop_param();
            filter_chan_ = pop_param();
            queue_response(0x03, {stat_}, 800);
            break;

        case 0x0E: // CdlSetmode
            mode_ = pop_param();
            queue_response(0x03, {stat_}, 800);
            break;

        case 0x0F: // CdlGetparam
            queue_response(0x03, {stat_, mode_, 0x00, filter_file_, filter_chan_}, 800);
            break;

        case 0x10: // CdlGetlocL (8 bytes header + subheader of last sector)
            queue_response(0x03, {
                raw_sector_[12], raw_sector_[13], raw_sector_[14], raw_sector_[15],
                raw_sector_[16], raw_sector_[17], raw_sector_[18], raw_sector_[19]
            }, 800);
            break;

        case 0x11: { // CdlGetlocP (track, index, rel_msf, abs_msf)
            // The drive reports the Q-subchannel position under the head: while reading that is
            // the sector most recently delivered (read_lba_ already points at the next one).
            // Vagrant Story's loader checks this after every sector and re-seeks on a mismatch.
            // LibCrypt sectors (listed in the disc's .sbi) carry altered Q data with a broken CRC.
            // The drive drops Q frames that fail the CRC and keeps reporting the last valid
            // position, so on such a sector GetlocP still shows the previous sector. LibCrypt
            // detects exactly that "stuck" position; passing the altered Q through instead makes
            // the check compute a wrong key.
            const u32 cur = reading_ ? (read_lba_ > 0 ? read_lba_ - 1u : 0u) : read_lba_;
            std::array<u8, 10> q{};
            if (disc_ != nullptr) {
                u32 q_lba = cur;
                while (q_lba > 0 && disc_->has_subq_override(q_lba)) {
                    --q_lba;
                }
                q = disc_->subchannel_q(q_lba);
            } else {
                const u32 abs_f = cur + 150u;
                q = {0x41u, 0x01u, 0x01u,
                     bin_to_bcd(cur / (60u * 75u)), bin_to_bcd((cur / 75u) % 60u), bin_to_bcd(cur % 75u),
                     0x00u,
                     bin_to_bcd(abs_f / (60u * 75u)), bin_to_bcd((abs_f / 75u) % 60u), bin_to_bcd(abs_f % 75u)};
            }
            // GetlocP: track, index, relative MSF, absolute MSF (Q bytes 1..5 and 7..9)
            queue_response(0x03, {q[1], q[2], q[3], q[4], q[5], q[7], q[8], q[9]}, 800);
            break;
        }

        case 0x13: // CdlGetTN (first track 1, last track 1)
            queue_response(0x03, {stat_, 0x01, 0x01}, 800);
            break;

        case 0x14: { // CdlGetTD
            const u8 trk = bcd_to_bin(pop_param());
            if (trk == 0 && disc_ != nullptr) {
                const u32 end_f = disc_->total_sectors() + 150u;
                queue_response(0x03, {stat_, bin_to_bcd(end_f / (60u * 75u)), bin_to_bcd((end_f / 75u) % 60u)}, 800);
            } else {
                queue_response(0x03, {stat_, 0x00, 0x02}, 800);
            }
            break;
        }

        case 0x15: // CdlSeekL
        case 0x16: // CdlSeekP
            if (has_pending_seek_) {
                read_lba_         = seek_lba_;
                has_pending_seek_ = false;
                if (disc_ != nullptr) {
                    disc_->read_sector_raw2352(read_lba_, raw_sector_.data());
                }
            }
            reading_ = false;
            stat_    = 0x02u;
            queue_response(0x03, {stat_}, 800);
            queue_response(0x02, {stat_}, 3000);
            break;

        case 0x19: { // CdlTest
            const u8 sub = pop_param();
            if (sub == 0x20) {
                queue_response(0x03, {0x98, 0x06, 0x10, 0xC3}, 800);
            } else {
                queue_response(0x03, {stat_}, 800);
            }
            break;
        }

        case 0x1A: // CdlGetID
            queue_response(0x03, {stat_}, 800);
            queue_response(0x02, {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'E'}, 4000);
            break;

        case 0x1E: // ReadTOC
            queue_response(0x03, {stat_}, 800);
            queue_response(0x02, {stat_}, 4000);
            break;

        default:
            queue_response(0x03, {stat_}, 800);
            break;
    }

    param_fifo_.clear();
}

} // namespace vsport
