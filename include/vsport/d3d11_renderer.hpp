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
    // aspect: picture width / height (4:3 = 1.333...); 0 = stretch to fill the whole window
    void present_vram(const PsxGpu& gpu, bool full_vram, float aspect, bool vsync = true);

    void cycle_shader_mode() noexcept {
        shader_mode_ = static_cast<UpscaleShaderMode>((static_cast<u32>(shader_mode_) + 1u) % 3u);
    }
    [[nodiscard]] UpscaleShaderMode shader_mode() const noexcept { return shader_mode_; }
    void set_shader_mode(UpscaleShaderMode m) noexcept { shader_mode_ = m; }
    // Integer scaling: the picture height is a whole multiple of the PS1 display height
    // (no uneven pixel rows), with black borders filling the rest of the window
    void set_integer_scaling(bool on) noexcept { integer_scaling_ = on; }
    [[nodiscard]] bool integer_scaling() const noexcept { return integer_scaling_; }
    [[nodiscard]] const char* shader_mode_name() const noexcept;
    [[nodiscard]] const std::string& adapter_name() const noexcept { return adapter_name_; }
    // ID3D11Device* of the window (opaque), to share with a graphics card HrBackend; null if not initialized
    [[nodiscard]] void* device_handle() const noexcept;
    // Diagnostics: the next present_vram() saves the finished window picture to a PPM file
    void request_backbuffer_dump(const std::string& path) { dump_path_ = path; }

private:
    struct Impl;
    Impl* impl_                    = nullptr;
    bool initialized_              = false;
    UpscaleShaderMode shader_mode_ = UpscaleShaderMode::SharpPixel;
    bool integer_scaling_          = false;
    std::string adapter_name_;
    std::string dump_path_;
};

} // namespace vsport
