#pragma once

#include "vsport/psx_spu.hpp"
#include "vsport/types.hpp"

#include <span>

namespace vsport {

// Low-latency native Windows waveOut (WinMM) 44.1 kHz 16-bit Stereo Audio Streamer
class Win32AudioOut {
public:
    Win32AudioOut();
    ~Win32AudioOut();

    Win32AudioOut(const Win32AudioOut&)            = delete;
    Win32AudioOut& operator=(const Win32AudioOut&) = delete;

    bool initialize(u32 sample_rate = PsxSpu::SAMPLE_RATE);
    void shutdown();

    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] bool is_muted() const noexcept { return muted_; }
    void set_muted(bool m) noexcept { muted_ = m; }
    void toggle_mute() noexcept { muted_ = !muted_; }
    // Output volume in percent (0..100)
    void set_volume(u32 percent) noexcept { volume_percent_ = percent > 100u ? 100u : percent; }
    [[nodiscard]] u32 volume() const noexcept { return volume_percent_; }

    // Pulls synthesized stereo frames from PsxSpu and queues them to Windows audio hardware
    void pump_from_spu(PsxSpu& spu);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool open_  = false;
    bool muted_ = false;
    u32 volume_percent_ = 100;
};

} // namespace vsport
