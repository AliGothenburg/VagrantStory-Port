#pragma once

#include "vsport/cdrom_iso.hpp"
#include "vsport/types.hpp"

#include <array>
#include <deque>
#include <vector>

namespace vsport {

class StateIO;

class PsxSpu;

// PlayStation 1 CD-ROM Controller (Ports 0x1F801800..0x1F801803 + DMA3 Data FIFO)
class PsxCdrom {
public:
    PsxCdrom() { reset(); }

    void reset();
    void attach_disc(DiscImage* disc) noexcept { disc_ = disc; }
    void attach_spu(PsxSpu* spu) noexcept { spu_ = spu; }

    // 8-bit port access (offset = 0..3 corresponding to 0x1F801800..0x1F801803)
    [[nodiscard]] u8 read_reg8(u32 port_offset);
    void write_reg8(u32 port_offset, u8 val);

    // 32-bit word read from Data FIFO (used by DMA3 and CPU 32-bit reads from 0x1F801802/1800)
    [[nodiscard]] u32 read_data_word();

    // Advance CD-ROM controller state machine; returns true if IRQ2 should be asserted in I_STAT
    bool tick(u64 elapsed_cycles);

    [[nodiscard]] u32 sectors_read() const noexcept { return sectors_read_; }
    [[nodiscard]] u32 last_read_lba() const noexcept { return last_read_lba_; }
    [[nodiscard]] const std::string& debug_log() const noexcept { return debug_log_; }

    // Save state: writes or reads the whole state (see state_io.hpp)
    void serialize(StateIO& io);

private:
    struct QueuedResponse {
        u8 irq_type = 0; // 1 = INT1 (DataReady), 2 = INT2 (Complete), 3 = INT3 (Ack), 5 = INT5 (Error)
        std::vector<u8> bytes;
        u64 delay_cycles = 0;
    };

    void execute_command(u8 cmd);
    void queue_response(u8 irq_type, std::initializer_list<u8> bytes, u64 delay_cycles = 1200);
    bool promote_next_irq();
    void load_current_sector_buffer();

    static u8 bcd_to_bin(u8 bcd) noexcept { return static_cast<u8>(((bcd >> 4) * 10u) + (bcd & 0x0Fu)); }
    static u8 bin_to_bcd(u32 val) noexcept { return static_cast<u8>(((val / 10u) << 4) | (val % 10u)); }

    DiscImage* disc_ = nullptr;
    PsxSpu* spu_     = nullptr;

    u8 index_       = 0;    // Bits 0..1 of 0x1F801800
    u8 ie_          = 0;    // Interrupt Enable (5 bits)
    u8 irq_flag_    = 0;    // Active Interrupt Flag (bits 0..4)
    u8 stat_        = 0x02; // Spindle motor on
    u8 mode_        = 0;    // Setmode byte (bit 5 = 2340 whole sector, bit 7 = 2x speed)
    u8 filter_file_ = 0;
    u8 filter_chan_ = 0;
    bool muted_     = false;

    std::deque<u8> param_fifo_;
    std::deque<u8> resp_fifo_;
    std::deque<QueuedResponse> pending_irqs_;

    bool has_pending_seek_ = false;
    u32 seek_lba_          = 0;
    u32 read_lba_          = 0;
    u32 last_read_lba_     = 0;
    bool reading_          = false;
    u64 read_timer_        = 0;

    std::array<u8, 2352> raw_sector_{};    // Drive sector buffer (newest sector read)
    std::vector<u8> data_fifo_;            // Host data FIFO (filled by BFRD)
    u32 sector_seq_      = 0;              // Data sectors announced with INT1
    u32 fifo_sector_seq_ = 0;              // Sector sequence currently loaded into data_fifo_
    bool bfrd_           = false;          // Request register bit 7 (Want Data) as last written
    std::size_t data_pos_  = 0;
    u32 sectors_read_      = 0;
    u32 logged_cmds_       = 0;
    std::string debug_log_;
};

} // namespace vsport
