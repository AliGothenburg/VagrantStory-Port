#include "vsport/win32_audio.hpp"

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace vsport {

#ifdef _WIN32
namespace {

// Queue depth the WASAPI path aims for, in 44.1 kHz frames. It starts low and grows by one
// step after an underrun (a host frame that came late), up to the old WinMM pool size.
constexpr u32 WASAPI_TARGET_MIN  = 1764; // 40 ms
constexpr u32 WASAPI_TARGET_STEP = 441;  // 10 ms
constexpr u32 WASAPI_TARGET_MAX  = 5292; // 120 ms

template <typename T> void release(T*& p) {
    if (p != nullptr) { p->Release(); p = nullptr; }
}

} // namespace

struct Win32AudioOut::Impl {
    // WinMM fallback (used only when WASAPI cannot be opened)
    static constexpr std::size_t NUM_BUFFERS       = 8;
    static constexpr std::size_t FRAMES_PER_BUFFER = 735; // 44100 / 60 = 735 stereo frames (~16.6 ms)

    HWAVEOUT hwave = nullptr;
    std::array<WAVEHDR, NUM_BUFFERS> headers{};
    std::array<std::vector<s16>, NUM_BUFFERS> pcm_storage{};

    // WASAPI shared mode
    IAudioClient* client       = nullptr;
    IAudioRenderClient* render = nullptr;
    u32 buffer_frames  = 0;
    u32 target_frames  = WASAPI_TARGET_MIN;
    bool started       = false;
    bool primed        = false; // Something was written since the start, so an empty queue is an underrun
    bool com_owned     = false;
    u32 sample_rate    = 44100;
    DWORD retry_tick   = 0;     // Device lost (unplugged headphones): when to try reopening

    std::vector<s16> staging;

    bool open_wasapi() {
        IMMDeviceEnumerator* en = nullptr;
        IMMDevice* dev          = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en));
        if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
        release(dev);
        release(en);
        if (FAILED(hr)) { release(client); return false; }

        WAVEFORMATEX wfx{};
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = 2;
        wfx.nSamplesPerSec  = sample_rate;
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = 4;
        wfx.nAvgBytesPerSec = sample_rate * 4u;
        // The mixer converts 44.1 kHz s16 to the device format itself
        const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        const REFERENCE_TIME buffer_100ns = 2000000; // 200 ms room; the queue is kept far below that
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer_100ns, 0, &wfx, nullptr);
        if (SUCCEEDED(hr)) hr = client->GetBufferSize(&buffer_frames);
        if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
        if (FAILED(hr)) { close_wasapi(); return false; }
        target_frames = WASAPI_TARGET_MIN;
        started = primed = false;
        return true;
    }

    void close_wasapi() {
        if (client != nullptr && started) client->Stop();
        release(render);
        release(client);
        started = primed = false;
    }

    bool open_winmm() {
        WAVEFORMATEX wfx{};
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = 2;
        wfx.nSamplesPerSec  = sample_rate;
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = 4;
        wfx.nAvgBytesPerSec = sample_rate * 4u;
        if (waveOutOpen(&hwave, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            hwave = nullptr;
            return false;
        }
        for (std::size_t i = 0; i < NUM_BUFFERS; ++i) {
            pcm_storage[i].assign(FRAMES_PER_BUFFER * 2u, 0);
            auto& hdr          = headers[i];
            hdr                = WAVEHDR{};
            hdr.lpData         = reinterpret_cast<LPSTR>(pcm_storage[i].data());
            hdr.dwBufferLength = static_cast<DWORD>(FRAMES_PER_BUFFER * 2u * sizeof(s16));
            waveOutPrepareHeader(hwave, &hdr, sizeof(WAVEHDR));
            hdr.dwFlags |= WHDR_DONE;
        }
        return true;
    }

    void close_winmm() {
        if (hwave == nullptr) return;
        waveOutReset(hwave);
        for (std::size_t i = 0; i < NUM_BUFFERS; ++i) {
            if ((headers[i].dwFlags & WHDR_PREPARED) != 0) waveOutUnprepareHeader(hwave, &headers[i], sizeof(WAVEHDR));
        }
        waveOutClose(hwave);
        hwave = nullptr;
    }
};

// Copies n frames from staging to dst with mute and volume applied
static void mix_out(s16* dst, const s16* src, std::size_t frames, bool muted, u32 volume) {
    const std::size_t n = frames * 2u;
    if (muted) {
        std::fill(dst, dst + n, static_cast<s16>(0));
    } else if (volume >= 100u) {
        std::memcpy(dst, src, n * sizeof(s16));
    } else {
        for (std::size_t s = 0; s < n; ++s) {
            dst[s] = static_cast<s16>((static_cast<s32>(src[s]) * static_cast<s32>(volume)) / 100);
        }
    }
}
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
    impl_->sample_rate = sample_rate;
    // WASAPI needs COM on this thread; RPC_E_CHANGED_MODE means it is already up in another mode
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    impl_->com_owned  = SUCCEEDED(com);
    if (impl_->open_wasapi()) {
        backend_ = "WASAPI";
    } else if (impl_->open_winmm()) {
        backend_ = "WinMM";
    } else {
        backend_ = nullptr;
        return false;
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
    impl_->close_wasapi();
    impl_->close_winmm();
    if (impl_->com_owned) {
        CoUninitialize();
        impl_->com_owned = false;
    }
#endif
    open_    = false;
    backend_ = nullptr;
}

u32 Win32AudioOut::queue_ms() const noexcept {
#ifdef _WIN32
    if (impl_->client != nullptr) return impl_->target_frames * 1000u / impl_->sample_rate;
    if (impl_->hwave != nullptr) return static_cast<u32>(Impl::NUM_BUFFERS * Impl::FRAMES_PER_BUFFER * 1000u / impl_->sample_rate);
#endif
    return 0;
}

void Win32AudioOut::pump_from_spu(PsxSpu& spu) {
#ifdef _WIN32
    Impl& m = *impl_;
    // Device lost earlier: try to reopen once a second, meanwhile keep the SPU queue trimmed
    if (open_ && m.client == nullptr && m.hwave == nullptr && GetTickCount() - m.retry_tick >= 1000u) {
        m.retry_tick = GetTickCount();
        if (m.open_wasapi()) backend_ = "WASAPI";
    }

    if (m.client != nullptr) {
        UINT32 padding = 0;
        HRESULT hr     = m.client->GetCurrentPadding(&padding);
        if (SUCCEEDED(hr)) {
            if (m.started && m.primed && padding == 0 && m.target_frames < WASAPI_TARGET_MAX) {
                m.target_frames += WASAPI_TARGET_STEP; // Ran dry: allow a little more queue
            }
            const u32 target = std::min(m.target_frames, m.buffer_frames);
            u32 want         = padding < target ? target - padding : 0u;
            want             = std::min<u32>(want, static_cast<u32>(spu.buffered_frames()));
            if (want > 0) {
                BYTE* dst = nullptr;
                hr        = m.render->GetBuffer(want, &dst);
                if (SUCCEEDED(hr)) {
                    const std::size_t got = spu.pop_samples(m.staging, want);
                    mix_out(reinterpret_cast<s16*>(dst), m.staging.data(), got, muted_, volume_percent_);
                    if (got < want) {
                        std::fill(reinterpret_cast<s16*>(dst) + got * 2u, reinterpret_cast<s16*>(dst) + want * 2u, s16{0});
                    }
                    hr = m.render->ReleaseBuffer(want, 0);
                    m.primed = true;
                }
            }
            if (SUCCEEDED(hr) && !m.started && m.primed) {
                hr = m.client->Start();
                m.started = SUCCEEDED(hr);
            }
        }
        if (FAILED(hr)) { // AUDCLNT_E_DEVICE_INVALIDATED and the like: reopen later
            m.close_wasapi();
            m.retry_tick = GetTickCount();
        }
        // Sound left in the SPU queue is latency too. It only grows when the host runs faster
        // than the sound card (fast-forward, clock drift), so drop the part beyond two frames.
        const std::size_t keep = static_cast<std::size_t>(PsxSpu::SAMPLE_RATE / 30u);
        if (spu.buffered_frames() > keep + m.target_frames) {
            spu.pop_samples(m.staging, spu.buffered_frames() - keep);
        }
        return;
    }

    if (m.hwave == nullptr) {
        // Keep SPU queue trimmed even if no speaker device is open
        if (spu.buffered_frames() > PsxSpu::SAMPLE_RATE) {
            spu.pop_samples(m.staging, spu.buffered_frames() - (PsxSpu::SAMPLE_RATE / 2u));
        }
        return;
    }

    for (std::size_t i = 0; i < Impl::NUM_BUFFERS; ++i) {
        auto& hdr = m.headers[i];
        if ((hdr.dwFlags & WHDR_DONE) == 0) continue;
        if (spu.buffered_frames() < Impl::FRAMES_PER_BUFFER) break;

        const std::size_t got = spu.pop_samples(m.staging, Impl::FRAMES_PER_BUFFER);
        if (got == 0) break;
        mix_out(m.pcm_storage[i].data(), m.staging.data(), got, muted_, volume_percent_);
        if (got < Impl::FRAMES_PER_BUFFER) {
            std::fill(m.pcm_storage[i].begin() + static_cast<std::ptrdiff_t>(got * 2u), m.pcm_storage[i].end(), s16{0});
        }
        hdr.dwBufferLength = static_cast<DWORD>(Impl::FRAMES_PER_BUFFER * 2u * sizeof(s16));
        waveOutWrite(m.hwave, &hdr, sizeof(WAVEHDR));
    }

    // Prevent audio drift accumulation if host ran faster than 60fps
    if (spu.buffered_frames() > Impl::FRAMES_PER_BUFFER * Impl::NUM_BUFFERS) {
        const std::size_t excess = spu.buffered_frames() - (Impl::FRAMES_PER_BUFFER * 4u);
        spu.pop_samples(m.staging, excess);
    }
#else
    (void)spu;
#endif
}

} // namespace vsport
