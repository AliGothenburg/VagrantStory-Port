#pragma once

#include "vsport/psx_gpu.hpp"
#include "vsport/types.hpp"

#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace vsport {

enum class UpscaleShaderMode : u32 {
    SharpPixel   = 0, // Sub-texel anti-shimmer sharp pixel filter
    SmoothLinear = 1, // Bilinear RGB555 interpolation
    TrinitronCrt = 2  // Sony PVM / Trinitron scanline & aperture grille
};

// Direct3D 11 Hardware-Accelerated Renderer (GPU VRAM upload + HLSL RGB555/RGB888 & CRT post-processing)
class D3D11Renderer {
public:
    D3D11Renderer();
    ~D3D11Renderer();

    D3D11Renderer(const D3D11Renderer&)            = delete;
    D3D11Renderer& operator=(const D3D11Renderer&) = delete;

#ifdef _WIN32
    bool initialize(HWND hwnd, u32 width, u32 height, std::string* err = nullptr);
#endif
    void shutdown();

    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

    void resize(u32 width, u32 height);
    void present_vram(const PsxGpu& gpu, bool full_vram, bool widescreen, bool vsync = true);

    void cycle_shader_mode() noexcept {
        shader_mode_ = static_cast<UpscaleShaderMode>((static_cast<u32>(shader_mode_) + 1u) % 3u);
    }
    [[nodiscard]] UpscaleShaderMode shader_mode() const noexcept { return shader_mode_; }
    [[nodiscard]] const char* shader_mode_name() const noexcept;
    [[nodiscard]] const std::string& adapter_name() const noexcept { return adapter_name_; }

private:
    struct Impl;
    Impl* impl_                    = nullptr;
    bool initialized_              = false;
    UpscaleShaderMode shader_mode_ = UpscaleShaderMode::SharpPixel;
    std::string adapter_name_;
};

} // namespace vsport
