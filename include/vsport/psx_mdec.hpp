#pragma once

#include "vsport/types.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace vsport {

class StateIO;

// PlayStation 1 Hardware Macroblock Decoder (MDEC — 0x1F801820 / 0x1F801824, DMA0 In / DMA1 Out)
class PsxMdec {
public:
    PsxMdec();
    void reset();

    // 0x1F801820: MDEC0 Command/Parameter Write & Decoded Pixel Output Read
    void write_cmd_or_param(u32 word);
    [[nodiscard]] u32 read_data();

    // 0x1F801824: MDEC1 Control/Reset Write & Status Read
    void write_ctrl(u32 word);
    [[nodiscard]] u32 read_status() const noexcept;

    [[nodiscard]] bool accepting_params() const noexcept { return remaining_param_words_ > 0; }
    [[nodiscard]] bool has_output() const noexcept { return out_read_pos_ < out_fifo_.size(); }
    [[nodiscard]] std::size_t available_output_words() const noexcept {
        return (out_read_pos_ < out_fifo_.size()) ? (out_fifo_.size() - out_read_pos_) : 0u;
    }
    [[nodiscard]] u64 macroblocks_decoded() const noexcept { return macroblocks_decoded_; }
    [[nodiscard]] u64 commands_executed() const noexcept { return commands_executed_; }

    // Save state: writes or reads the whole state (see state_io.hpp)
    void serialize(StateIO& io);

private:
    void init_default_tables() noexcept;
    void finish_command();
    void decode_macroblock_stream();
    bool decode_rl_block(const std::vector<u16>& rle, std::size_t& pos, const std::array<u8, 64>& iq,
                         std::array<s32, 64>& out_spatial) const noexcept;
    void idct_8x8(const std::array<s32, 64>& coeff, std::array<s32, 64>& out_spatial) const noexcept;

    u8 active_cmd_             = 0;
    u8 depth_                  = 0; // 0=4bit, 1=8bit, 2=24bit RGB888, 3=15bit RGB555
    bool signed_out_           = false;
    bool set_bit15_            = false;
    bool iq_color_             = false;
    bool dma0_in_enable_       = false;
    bool dma1_out_enable_      = false;
    u32 remaining_param_words_ = 0;
    u16 status_low16_          = 0;

    std::array<u8, 64> iq_y_{};
    std::array<u8, 64> iq_uv_{};
    std::array<s16, 64> scale_table_{};

    std::vector<u32> in_fifo_;
    std::vector<u32> out_fifo_;
    std::size_t out_read_pos_ = 0;

    u64 macroblocks_decoded_ = 0;
    u64 commands_executed_   = 0;
};

} // namespace vsport
