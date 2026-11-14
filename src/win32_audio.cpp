#include "vsport/win32_audio.hpp"

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace vsport {

#ifdef _WIN32
struct Win32AudioOut::Impl {
    static constexpr std::size_t NUM_BUFFERS       = 8;
    static constexpr std::size_t FRAMES_PER_BUFFER = 735; // 44100 / 60 = 735 stereo frames (~16.6 ms)

    HWAVEOUT hwave = nullptr;
    std::array<WAVEHDR, NUM_BUFFERS> headers{};
    std::array<std::vector<s16>, NUM_BUFFERS> pcm_storage{};
    std::vector<s16> staging;
};
#else
struct Win32AudioOut::Impl {};
#endif

Win32AudioOut::Win32AudioOut() : impl_(new Impl()) {}

Win32AudioOut::~Win32AudioOut() {
    shutdown();
    delete impl_;
}

bool Win32AudioOut::initialize(u32 sample_rate) {
    shutdown();
#ifdef _WIN32
    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = sample_rate;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = 4;
    wfx.nAvgBytesPerSec = sample_rate * 4u;

    if (waveOutOpen(&impl_->hwave, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        impl_->hwave = nullptr;
        return false;
    }

    for (std::size_t i = 0; i < Impl::NUM_BUFFERS; ++i) {
        impl_->pcm_storage[i].assign(Impl::FRAMES_PER_BUFFER * 2u, 0);
        auto& hdr         = impl_->headers[i];
        hdr               = WAVEHDR{};
        hdr.lpData        = reinterpret_cast<LPSTR>(impl_->pcm_storage[i].data());
        hdr.dwBufferLength = static_cast<DWORD>(Impl::FRAMES_PER_BUFFER * 2u * sizeof(s16));
        waveOutPrepareHeader(impl_->hwave, &hdr, sizeof(WAVEHDR));
        hdr.dwFlags |= WHDR_DONE;
    }

    open_ = true;
    return true;
#else
    (void)sample_rate;
    return false;
#endif
}

void Win32AudioOut::shutdown() {
#ifdef _WIN32
    if (impl_->hwave != nullptr) {
        waveOutReset(impl_->hwave);
        for (std::size_t i = 0; i < Impl::NUM_BUFFERS; ++i) {
            if ((impl_->headers[i].dwFlags & WHDR_PREPARED) != 0) {
                waveOutUnprepareHeader(impl_->hwave, &impl_->headers[i], sizeof(WAVEHDR));
            }
        }
        waveOutClose(impl_->hwave);
        impl_->hwave = nullptr;
    }
#endif
    open_ = false;
}

void Win32AudioOut::pump_from_spu(PsxSpu& spu) {
#ifdef _WIN32
    if (!open_ || impl_->hwave == nullptr) {
        // Keep SPU queue trimmed even if no speaker device is open
        if (spu.buffered_frames() > PsxSpu::SAMPLE_RATE) {
            spu.pop_samples(impl_->staging, spu.buffered_frames() - (PsxSpu::SAMPLE_RATE / 2u));
        }
        return;
    }

    for (std::size_t i = 0; i < Impl::NUM_BUFFERS; ++i) {
        auto& hdr = impl_->headers[i];
        if ((hdr.dwFlags & WHDR_DONE) == 0) {
            continue;
        }
        if (spu.buffered_frames() < Impl::FRAMES_PER_BUFFER) {
            break;
        }

        const std::size_t got = spu.pop_samples(impl_->staging, Impl::FRAMES_PER_BUFFER);
        if (got == 0) {
            break;
        }

        if (muted_) {
            std::fill(impl_->pcm_storage[i].begin(), impl_->pcm_storage[i].end(), static_cast<s16>(0));
        } else {
            std::memcpy(impl_->pcm_storage[i].data(), impl_->staging.data(), got * 2u * sizeof(s16));
            if (volume_percent_ < 100u) {
                for (std::size_t s = 0; s < got * 2u; ++s) {
                    impl_->pcm_storage[i][s] = static_cast<s16>(
                        (static_cast<s32>(impl_->pcm_storage[i][s]) * static_cast<s32>(volume_percent_)) / 100);
                }
            }
            if (got < Impl::FRAMES_PER_BUFFER) {
                std::fill(impl_->pcm_storage[i].begin() + static_cast<std::ptrdiff_t>(got * 2u),
                          impl_->pcm_storage[i].end(), static_cast<s16>(0));
            }
        }

        hdr.dwBufferLength = static_cast<DWORD>(Impl::FRAMES_PER_BUFFER * 2u * sizeof(s16));
        waveOutWrite(impl_->hwave, &hdr, sizeof(WAVEHDR));
    }

    // Prevent audio drift accumulation if host ran faster than 60fps
    if (spu.buffered_frames() > Impl::FRAMES_PER_BUFFER * Impl::NUM_BUFFERS) {
        const std::size_t excess = spu.buffered_frames() - (Impl::FRAMES_PER_BUFFER * 4u);
        spu.pop_samples(impl_->staging, excess);
    }
#else
    (void)spu;
#endif
}

} // namespace vsport
