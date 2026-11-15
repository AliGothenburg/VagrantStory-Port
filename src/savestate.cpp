// Save states: serialize() of every hardware block and PsxRuntime::save_state / load_state.
// A state holds the whole emulated machine except the memory card images (so loading a state
// never undoes saves made on the card) and host-only data (upscaled picture, sound output queue,
// native-code caches), which are rebuilt after loading.
#include "vsport/runtime.hpp"
#include "vsport/state_io.hpp"

#include <algorithm>
#include <cstring>

namespace vsport {

namespace {
constexpr u32 STATE_MAGIC   = 0x54535356u; // "VSST"
constexpr u32 STATE_VERSION = 2; // 2: reverb state, voice history for cubic interpolation
constexpr u32 fourcc(char a, char b, char c, char d) {
    return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8) |
           (static_cast<u32>(static_cast<u8>(c)) << 16) | (static_cast<u32>(static_cast<u8>(d)) << 24);
}
} // namespace

void GteEngine::serialize(StateIO& io) {
    io.tag(fourcc('G', 'T', 'E', ' '));
    io.pod(data_);
    io.pod(ctrl_);
}

void PsxGpu::serialize(StateIO& io) {
    io.tag(fourcc('G', 'P', 'U', ' '));
    if (io.saving()) flush_hr();
    io.vec(vram_);
    io.pod(mask_set_);
    io.pod(mask_check_);
    io.vec(fifo_);
    io.pod(expected_words_);
    io.pod(mode_);
    io.pod(in_polyline_);
    io.pod(xfer_x_); io.pod(xfer_y_); io.pod(xfer_w_); io.pod(xfer_h_); io.pod(xfer_cx_); io.pod(xfer_cy_);
    io.pod(gpuread_latch_);
    io.pod(gpustat_);
    io.pod(texpage_);
    io.pod(texwin_);
    io.pod(clip_x1_); io.pod(clip_y1_); io.pod(clip_x2_); io.pod(clip_y2_);
    io.pod(draw_off_x_); io.pod(draw_off_y_);
    io.pod(disp_vram_x_); io.pod(disp_vram_y_); io.pod(disp_width_); io.pod(disp_height_);
    io.pod(primitives_drawn_);
    io.vec(uploads_); // Texture replacement: which VRAM areas still hold an untouched upload
    if (!io.saving()) {
        rebuild_upload_cells();
        // Host-only drawing state: nothing queued, and the upscaled picture restarts from the
        // native one (it sharpens again as the game redraws, within a frame or two)
        vram_.resize(static_cast<std::size_t>(VRAM_WIDTH) * VRAM_HEIGHT);
        hr_batch_.clear();
        hr_batch_offs_.clear();
        interp_clear();
        rim_pending_.clear();
        batch_has_tex_ = false;
        tex_cells_.fill(0);
        hr_rim_ = false;
        if (!keep_picture_on_load_) hr_sync_rect(0, 0, VRAM_WIDTH, VRAM_HEIGHT); // Runahead restores its own copy
    }
}

void PsxSpu::serialize(StateIO& io) {
    io.tag(fourcc('S', 'P', 'U', ' '));
    io.vec(ram_);
    io.pod(regs_);
    io.pod(voices_);
    io.pod(main_vol_l_); io.pod(main_vol_r_); io.pod(cd_vol_l_); io.pod(cd_vol_r_);
    io.pod(spucnt_); io.pod(spustat_); io.pod(xfer_ctrl_); io.pod(xfer_addr_reg_); io.pod(xfer_cur_addr_);
    io.pod(irq_addr_reg_); io.pod(endx_mask_); io.pod(irq_pending_);
    io.pod(xa_hist_l1_); io.pod(xa_hist_l2_); io.pod(xa_hist_r1_); io.pod(xa_hist_r2_);
    io.deq(cd_audio_fifo_);
    io.pod(rev_addr_); io.pod(rev_phase_); io.pod(rev_acc_l_); io.pod(rev_acc_r_); io.pod(rev_out_l_); io.pod(rev_out_r_);
    io.pod(cycle_accum_);
    io.pod(bytes_uploaded_); io.pod(key_on_events_); io.pod(xa_sectors_decoded_); io.pod(frames_rendered_);
    if (!io.saving()) {
        ram_.resize(SPU_RAM_SIZE);
        pcm_fifo_.clear(); // Sound already mixed for the host belongs to the old timeline
    }
}

void PsxMdec::serialize(StateIO& io) {
    io.tag(fourcc('M', 'D', 'E', 'C'));
    io.pod(active_cmd_); io.pod(depth_); io.pod(signed_out_); io.pod(set_bit15_); io.pod(iq_color_);
    io.pod(dma0_in_enable_); io.pod(dma1_out_enable_); io.pod(remaining_param_words_); io.pod(status_low16_);
    io.pod(iq_y_); io.pod(iq_uv_); io.pod(scale_table_);
    io.vec(in_fifo_);
    io.vec(out_fifo_);
    u64 pos = out_read_pos_;
    io.pod(pos);
    out_read_pos_ = static_cast<std::size_t>(pos);
    io.pod(macroblocks_decoded_); io.pod(commands_executed_);
}

void PsxCdrom::serialize(StateIO& io) {
    io.tag(fourcc('C', 'D', 'R', 'M'));
    io.pod(index_); io.pod(ie_); io.pod(irq_flag_); io.pod(stat_); io.pod(mode_);
    io.pod(filter_file_); io.pod(filter_chan_); io.pod(muted_);
    io.deq(param_fifo_);
    io.deq(resp_fifo_);
    u32 n = static_cast<u32>(pending_irqs_.size());
    io.pod(n);
    if (!io.saving()) pending_irqs_.assign(n > 64u ? 0u : n, QueuedResponse{});
    for (QueuedResponse& q : pending_irqs_) {
        io.pod(q.irq_type);
        io.vec(q.bytes);
        io.pod(q.delay_cycles);
    }
    io.pod(has_pending_seek_); io.pod(seek_lba_); io.pod(read_lba_); io.pod(last_read_lba_);
    io.pod(reading_); io.pod(read_timer_);
    io.pod(raw_sector_);
    io.vec(data_fifo_);
    io.pod(sector_seq_); io.pod(fifo_sector_seq_); io.pod(bfrd_);
    u64 pos = data_pos_;
    io.pod(pos);
    data_pos_ = static_cast<std::size_t>(pos);
    io.pod(sectors_read_);
}

std::vector<u8> PsxRuntime::save_state(const CpuContext& ctx) {
    StateIO io;
    CpuContext c = ctx;
    serialize_state(io, c);
    return std::move(io.buffer());
}

bool PsxRuntime::load_state(std::vector<u8> data, CpuContext& ctx) {
    // Check the whole file on a copy of nothing: a failed read must not leave a half-loaded
    // machine, so the header is validated first and the rest is only trusted with all tags right
    if (data.size() < 16) return false;
    u32 head[2];
    std::memcpy(head, data.data(), sizeof(head));
    if (head[0] != STATE_MAGIC || head[1] != STATE_VERSION) return false;
    const std::vector<u8> backup = save_state(ctx);
    StateIO io(std::move(data));
    serialize_state(io, ctx);
    if (!io.ok()) {
        StateIO undo(backup); // Damaged or mismatching file: go back to where we were
        serialize_state(undo, ctx);
        return false;
    }
    return true;
}

void PsxRuntime::runahead_snapshot(bool restore) {
    if (restore) {
        pgxp_ = pgxp_snap_;
    } else {
        pgxp_.prune();
        pgxp_snap_ = pgxp_;
    }
    gpu_.picture_snapshot(restore);
}

bool PsxRuntime::load_state_fast(const std::vector<u8>& data, CpuContext& ctx) {
    StateIO io(data);
    fast_load_ = true;
    gpu_.set_keep_picture_on_load(true);
    serialize_state(io, ctx);
    gpu_.set_keep_picture_on_load(false);
    fast_load_ = false;
    return io.ok();
}

void PsxRuntime::serialize_state(StateIO& io, CpuContext& ctx) {
    io.tag(STATE_MAGIC);
    io.tag(STATE_VERSION);
    u32 ram_size = static_cast<u32>(ram_.size());
    io.tag(ram_size);

    io.tag(fourcc('C', 'P', 'U', ' '));
    io.pod(ctx.r); io.pod(ctx.pc); io.pod(ctx.next_pc); io.pod(ctx.hi); io.pod(ctx.lo);
    io.pod(ctx.cop0); io.pod(ctx.cycles); io.pod(ctx.halted);

    io.tag(fourcc('R', 'A', 'M', ' '));
    if (ram_size && !io.saving() && fast_load_) {
        // Runahead: only pages that really differ are written and count as rewritten, so the
        // native code of unchanged pages stays valid (marking all of them is very slow per frame)
        static std::vector<u8> tmp;
        tmp.resize(ram_.size());
        io.raw(tmp.data(), tmp.size());
        if (io.ok()) {
            for (std::size_t off = 0; off < ram_.size(); off += 4096u) {
                const std::size_t len = std::min<std::size_t>(4096u, ram_.size() - off);
                if (std::memcmp(&ram_[off], &tmp[off], len) == 0) continue;
                std::memcpy(&ram_[off], &tmp[off], len);
                u32& e = ram_page_epoch_[off >> 12];
                if (++e == 0) e = 1u;
            }
        }
    } else if (ram_size) {
        io.raw(ram_.data(), ram_.size());
    }
    io.pod(scratchpad_);

    gte_.serialize(io);
    gpu_.serialize(io);
    cdrom_.serialize(io);
    mdec_.serialize(io);
    spu_.serialize(io);

    io.tag(fourcc('H', 'W', ' ', ' '));
    io.pod(dma_);
    io.pod(dma1_transfer_done_); io.pod(dma1_busy_cycles_);
    io.pod(timers_);
    io.pod(sio_stat_); io.pod(sio_mode_); io.pod(sio_ctrl_); io.pod(sio_baud_); io.pod(sio_rx_byte_);
    io.pod(sio_step_); io.pod(sio_selected_); io.pod(sio_is_memcard_);
    io.pod(cpu_overclock_rem_);
    io.pod(pad_analog_); io.pod(pad_config_); io.pod(pad_cmd_); io.pod(pad_resp_); io.pod(pad_resp_len_);
    io.pod(pad_motor_map_); io.pod(pad_motor_small_); io.pod(pad_motor_large_);
    io.pod(sio_ack_delay_); io.pod(sio_ack_pulse_);
    io.pod(dpcr_); io.pod(dicr_); io.pod(i_stat_); io.pod(i_mask_);
    io.pod(last_hw_cycle_); io.pod(vblank_accum_); io.pod(hblank_accum_); io.pod(sysclk8_accum_);
    io.pod(vblank_frames_);

    io.tag(fourcc('B', 'I', 'O', 'S'));
    io.pod(hook_entry_int_addr_); io.pod(in_interrupt_); io.pod(longjmp_token_);
    io.pod(saved_irq_ctx_.r); io.pod(saved_irq_ctx_.pc); io.pod(saved_irq_ctx_.next_pc);
    io.pod(saved_irq_ctx_.hi); io.pod(saved_irq_ctx_.lo); io.pod(saved_irq_ctx_.cop0);
    io.pod(saved_irq_ctx_.cycles); io.pod(saved_irq_ctx_.halted);
    io.pod(int_rp_heads_); io.pod(clear_rcnt_flags_); io.pod(clear_pad_flag_);
    io.pod(events_);
    io.pod(card_chan_); io.pod(memcard_last_write_vb_);
    for (OpenFile& f : open_files_) {
        io.pod(f.used); io.pod(f.slot); io.pod(f.mode); io.pod(f.pos);
        io.str(f.info.name); io.pod(f.info.first_block); io.pod(f.info.size);
    }
    io.str(find_pattern_); io.pod(find_slot_); io.pod(find_next_block_);
    io.pod(rand_seed_);
    io.pod(heap_base_); io.pod(heap_end_); io.pod(heap_cur_);
    io.pod(pending_aot_call_pc_); io.pod(saved_aot_pc_);
    io.tag(fourcc('E', 'N', 'D', ' '));

    if (!io.saving()) {
        // RAM changed under the native-code cache: every page counts as rewritten
        if (!fast_load_) {
            for (u32& e : ram_page_epoch_) {
                if (++e == 0) e = 1u;
            }
        }
        if (!fast_load_) pgxp_.clear(); // Exact vertex positions of the old timeline (runahead: same values)
    }
}

} // namespace vsport
