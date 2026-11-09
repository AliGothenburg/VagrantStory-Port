#include "vsport/d3d11_renderer.hpp"

#ifdef _WIN32
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vsport {

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

namespace {

struct alignas(16) DisplayConstants {
    float disp_origin[2]; // (disp_x, disp_y) in VRAM pixels
    float disp_size[2];   // (disp_w, disp_h) in VRAM pixels
    float view_size[2];   // Viewport (w, h) in screen pixels
    u32   shader_mode;    // 0 = Sharp, 1 = Bilinear, 2 = CRT
    u32   color_24bit;    // 1 = PS1 24-bit RGB888 mode, 0 = 15-bit RGB555
};

constexpr const char* kVramHlslSource = R"(
cbuffer DisplayCB : register(b0) {
    float2 disp_origin;
    float2 disp_size;
    float2 view_size;
    uint   shader_mode;
    uint   color_24bit;
};

Texture2D<uint> VramTex : register(t0);

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID) {
    VSOut o;
    o.uv  = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

float3 ReadVram15(int x, int y) {
    uint px = VramTex.Load(int3(x & 1023, y & 511, 0));
    float r = float(((px      ) & 31u) * 527u + 23u) * (1.0f / 16320.0f);
    float g = float(((px >>  5) & 31u) * 527u + 23u) * (1.0f / 16320.0f);
    float b = float(((px >> 10) & 31u) * 527u + 23u) * (1.0f / 16320.0f);
    return saturate(float3(r, g, b));
}

float3 ReadVram24(int px_idx, int y) {
    int byte_off  = (int(disp_origin.x) * 2) + px_idx * 3;
    int hw0       = (byte_off >> 1) & 1023;
    int hw1       = (hw0 + 1) & 1023;
    uint w0       = VramTex.Load(int3(hw0, y & 511, 0));
    uint w1       = VramTex.Load(int3(hw1, y & 511, 0));
    uint combined = w0 | (w1 << 16);
    uint shift    = uint(byte_off & 1) * 8u;
    uint rgb      = combined >> shift;
    return float3(
        float(rgb & 0xFFu) * (1.0f / 255.0f),
        float((rgb >> 8) & 0xFFu) * (1.0f / 255.0f),
        float((rgb >> 16) & 0xFFu) * (1.0f / 255.0f)
    );
}

float3 SampleDisplayPixel(int dx, int dy) {
    int cx = clamp(dx, 0, max(int(disp_size.x) - 1, 0));
    int cy = clamp(dy, 0, max(int(disp_size.y) - 1, 0));
    if (color_24bit != 0u) {
        return ReadVram24(cx, int(disp_origin.y) + cy);
    }
    return ReadVram15(int(disp_origin.x) + cx, int(disp_origin.y) + cy);
}

float3 SampleBilinear(float2 tex_coord) {
    float2 p  = tex_coord - 0.5f;
    int2   i0 = int2(floor(p));
    float2 f  = frac(p);
    float3 c00 = SampleDisplayPixel(i0.x,     i0.y);
    float3 c10 = SampleDisplayPixel(i0.x + 1, i0.y);
    float3 c01 = SampleDisplayPixel(i0.x,     i0.y + 1);
    float3 c11 = SampleDisplayPixel(i0.x + 1, i0.y + 1);
    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}

float4 PSMain(VSOut pin) : SV_TARGET {
    float2 tex_coord = pin.uv * disp_size;

    if (shader_mode == 0u) {
        // Sharp Sub-Texel Anti-Shimmer Filter
        float2 scale = max(view_size / max(disp_size, float2(1.0f, 1.0f)), float2(1.0f, 1.0f));
        float2 base  = floor(tex_coord);
        float2 fr    = frac(tex_coord);
        float2 edge  = saturate((fr - 0.5f) * scale + 0.5f);
        float2 sharp_tc = base + edge + 0.5f;
        return float4(SampleBilinear(sharp_tc), 1.0f);
    }

    if (shader_mode == 1u) {
        // Smooth Bilinear
        return float4(SampleBilinear(tex_coord), 1.0f);
    }

    // Mode 2: Sony Trinitron / PVM CRT Scanlines + Subtle Aperture Grille
    float2 base_y = float2(tex_coord.x, floor(tex_coord.y) + 0.5f);
    float3 col    = SampleBilinear(float2(tex_coord.x, base_y.y));

    // Scanline beam profile along PS1 horizontal scanlines
    float scan_phase = frac(tex_coord.y) - 0.5f;
    float beam       = exp(-7.5f * scan_phase * scan_phase);
    float luma       = dot(col, float3(0.299f, 0.587f, 0.114f));
    float scan_mul   = lerp(0.68f + 0.32f * beam, 1.0f, luma * 0.45f);

    // Subtle Trinitron RGB aperture grille triad across output pixels
    int triad = int(pin.pos.x) % 3;
    float3 mask = (triad == 0) ? float3(1.04f, 0.97f, 0.97f) :
                  (triad == 1) ? float3(0.97f, 1.04f, 0.97f) :
                                 float3(0.97f, 0.97f, 1.04f);

    // Gentle corner vignette
    float2 vuv = pin.uv * (1.0f - pin.uv);
    float vig  = saturate(pow(max(vuv.x * vuv.y * 16.0f, 0.0f), 0.10f));

    return float4(saturate(col * scan_mul * mask * vig * 1.08f), 1.0f);
}
)";

} // namespace

struct D3D11Renderer::Impl {
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    context;
    ComPtr<IDXGISwapChain>         swapchain;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Texture2D>        vram_tex;
    ComPtr<ID3D11ShaderResourceView> vram_srv;
    ComPtr<ID3D11VertexShader>     vs;
    ComPtr<ID3D11PixelShader>      ps;
    ComPtr<ID3D11Buffer>           cbuffer;
    u32 backbuf_w = 0;
    u32 backbuf_h = 0;

    bool create_rtv() {
        rtv.Reset();
        ComPtr<ID3D11Texture2D> backbuf;
        if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuf)))) {
            return false;
        }
        return SUCCEEDED(device->CreateRenderTargetView(backbuf.Get(), nullptr, &rtv));
    }
};
#else
struct D3D11Renderer::Impl {};
#endif

D3D11Renderer::D3D11Renderer() : impl_(new Impl()) {}

D3D11Renderer::~D3D11Renderer() {
    shutdown();
    delete impl_;
}

const char* D3D11Renderer::shader_mode_name() const noexcept {
    switch (shader_mode_) {
        case UpscaleShaderMode::SharpPixel:   return "Keskin Piksel (Anti-Shimmer)";
        case UpscaleShaderMode::SmoothLinear: return "Yumusak Bilinear";
        case UpscaleShaderMode::TrinitronCrt: return "Trinitron CRT";
    }
    return "Keskin Piksel";
}

#ifdef _WIN32
bool D3D11Renderer::initialize(HWND hwnd, u32 width, u32 height, std::string* err) {
    shutdown();

    width  = std::max<u32>(1u, width);
    height = std::max<u32>(1u, height);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                        = 2;
    sd.BufferDesc.Width                   = width;
    sd.BufferDesc.Height                  = height;
    sd.BufferDesc.Format                  = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = hwnd;
    sd.SampleDesc.Count                   = 1;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL chosen{};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 3, D3D11_SDK_VERSION, &sd,
        &impl_->swapchain, &impl_->device, &chosen, &impl_->context);
    if (FAILED(hr)) {
        // Fallback to WARP high-speed SIMD software D3D11 rasterizer if headless GPU
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, 3, D3D11_SDK_VERSION, &sd,
            &impl_->swapchain, &impl_->device, &chosen, &impl_->context);
    }
    if (FAILED(hr)) {
        if (err) *err = "D3D11CreateDeviceAndSwapChain basarisiz oldu.";
        return false;
    }

    // Query GPU adapter description
    {
        ComPtr<IDXGIDevice> dxgi_dev;
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(impl_->device.As(&dxgi_dev)) && SUCCEEDED(dxgi_dev->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC ad{};
            if (SUCCEEDED(adapter->GetDesc(&ad))) {
                char utf8[256]{};
                WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
                adapter_name_ = utf8;
            }
        }
    }

    if (!impl_->create_rtv()) {
        if (err) *err = "D3D11 RenderTargetView olusturulamadi.";
        shutdown();
        return false;
    }

    // Create 1024x512 R16_UINT dynamic VRAM texture
    D3D11_TEXTURE2D_DESC td{};
    td.Width              = PsxGpu::VRAM_WIDTH;
    td.Height             = PsxGpu::VRAM_HEIGHT;
    td.MipLevels          = 1;
    td.ArraySize          = 1;
    td.Format             = DXGI_FORMAT_R16_UINT;
    td.SampleDesc.Count   = 1;
    td.Usage              = D3D11_USAGE_DYNAMIC;
    td.BindFlags          = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags     = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(impl_->device->CreateTexture2D(&td, nullptr, &impl_->vram_tex)) ||
        FAILED(impl_->device->CreateShaderResourceView(impl_->vram_tex.Get(), nullptr, &impl_->vram_srv))) {
        if (err) *err = "1024x512 R16_UINT VRAM dokusu olusturulamadi.";
        shutdown();
        return false;
    }

    // Compile embedded HLSL Vertex & Pixel Shaders
    ComPtr<ID3DBlob> vs_blob, ps_blob, err_blob;
    hr = D3DCompile(kVramHlslSource, std::strlen(kVramHlslSource), "vram.hlsl",
                    nullptr, nullptr, "VSMain", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &vs_blob, &err_blob);
    if (FAILED(hr)) {
        if (err && err_blob) *err = static_cast<const char*>(err_blob->GetBufferPointer());
        shutdown();
        return false;
    }

    hr = D3DCompile(kVramHlslSource, std::strlen(kVramHlslSource), "vram.hlsl",
                    nullptr, nullptr, "PSMain", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &ps_blob, &err_blob);
    if (FAILED(hr)) {
        if (err && err_blob) *err = static_cast<const char*>(err_blob->GetBufferPointer());
        shutdown();
        return false;
    }

    if (FAILED(impl_->device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &impl_->vs)) ||
        FAILED(impl_->device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &impl_->ps))) {
        if (err) *err = "HLSL shader nesneleri olusturulamadi.";
        shutdown();
        return false;
    }

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth      = sizeof(DisplayConstants);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(impl_->device->CreateBuffer(&cbd, nullptr, &impl_->cbuffer))) {
        if (err) *err = "D3D11 ConstantBuffer olusturulamadi.";
        shutdown();
        return false;
    }

    impl_->backbuf_w = width;
    impl_->backbuf_h = height;
    initialized_     = true;
    return true;
}
#endif

void D3D11Renderer::shutdown() {
#ifdef _WIN32
    if (impl_->context) {
        impl_->context->ClearState();
    }
    impl_->cbuffer.Reset();
    impl_->ps.Reset();
    impl_->vs.Reset();
    impl_->vram_srv.Reset();
    impl_->vram_tex.Reset();
    impl_->rtv.Reset();
    impl_->swapchain.Reset();
    impl_->context.Reset();
    impl_->device.Reset();
#endif
    initialized_ = false;
}

void D3D11Renderer::resize(u32 width, u32 height) {
#ifdef _WIN32
    if (!initialized_ || width == 0 || height == 0) return;
    if (width == impl_->backbuf_w && height == impl_->backbuf_h) return;

    impl_->context->OMSetRenderTargets(0, nullptr, nullptr);
    impl_->rtv.Reset();
    if (SUCCEEDED(impl_->swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) {
        impl_->backbuf_w = width;
        impl_->backbuf_h = height;
        impl_->create_rtv();
    }
#else
    (void)width;
    (void)height;
#endif
}

void D3D11Renderer::present_vram(const PsxGpu& gpu, bool full_vram, bool widescreen, bool vsync) {
#ifdef _WIN32
    if (!initialized_ || !impl_->rtv) return;

    // 1. Upload 1024x512 RGB555 VRAM directly to GPU R16_UINT texture
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(impl_->context->Map(impl_->vram_tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        const u16* src      = gpu.vram().data();
        u8* dst             = static_cast<u8*>(mapped.pData);
        const std::size_t row_bytes = PsxGpu::VRAM_WIDTH * sizeof(u16);
        for (u32 y = 0; y < PsxGpu::VRAM_HEIGHT; ++y) {
            std::memcpy(dst + y * mapped.RowPitch, src + y * PsxGpu::VRAM_WIDTH, row_bytes);
        }
        impl_->context->Unmap(impl_->vram_tex.Get(), 0);
    }

    // 2. Compute aspect-preserving viewport (4:3, 16:9 widescreen, or 2:1 full VRAM)
    const float win_w = static_cast<float>(std::max<u32>(1u, impl_->backbuf_w));
    const float win_h = static_cast<float>(std::max<u32>(1u, impl_->backbuf_h));
    const float target_aspect = full_vram ? 2.0f : (widescreen ? (16.0f / 9.0f) : (4.0f / 3.0f));

    float vp_w = win_w;
    float vp_h = win_w / target_aspect;
    if (vp_h > win_h) {
        vp_h = win_h;
        vp_w = win_h * target_aspect;
    }
    const float vp_x = std::floor((win_w - vp_w) * 0.5f);
    const float vp_y = std::floor((win_h - vp_h) * 0.5f);

    // 3. Update constant buffer
    D3D11_MAPPED_SUBRESOURCE cb_map{};
    if (SUCCEEDED(impl_->context->Map(impl_->cbuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cb_map))) {
        auto* cb = static_cast<DisplayConstants*>(cb_map.pData);
        cb->disp_origin[0] = full_vram ? 0.0f : static_cast<float>(gpu.display_vram_x());
        cb->disp_origin[1] = full_vram ? 0.0f : static_cast<float>(gpu.display_vram_y());
        cb->disp_size[0]   = full_vram ? static_cast<float>(PsxGpu::VRAM_WIDTH)  : static_cast<float>(std::max<u32>(1u, gpu.display_width()));
        cb->disp_size[1]   = full_vram ? static_cast<float>(PsxGpu::VRAM_HEIGHT) : static_cast<float>(std::max<u32>(1u, gpu.display_height()));
        cb->view_size[0]   = vp_w;
        cb->view_size[1]   = vp_h;
        cb->shader_mode    = static_cast<u32>(shader_mode_);
        cb->color_24bit    = (!full_vram && gpu.is_24bit_display()) ? 1u : 0u;
        impl_->context->Unmap(impl_->cbuffer.Get(), 0);
    }

    // 4. Clear letterbox/pillarbox & draw full-screen triangle
    const float clear_col[4] = { 0.02f, 0.015f, 0.02f, 1.0f };
    impl_->context->ClearRenderTargetView(impl_->rtv.Get(), clear_col);
    impl_->context->OMSetRenderTargets(1, impl_->rtv.GetAddressOf(), nullptr);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = vp_x;
    vp.TopLeftY = vp_y;
    vp.Width    = vp_w;
    vp.Height   = vp_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    impl_->context->RSSetViewports(1, &vp);

    impl_->context->IASetInputLayout(nullptr);
    impl_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    impl_->context->VSSetShader(impl_->vs.Get(), nullptr, 0);
    impl_->context->PSSetShader(impl_->ps.Get(), nullptr, 0);
    impl_->context->PSSetConstantBuffers(0, 1, impl_->cbuffer.GetAddressOf());
    impl_->context->PSSetShaderResources(0, 1, impl_->vram_srv.GetAddressOf());

    impl_->context->Draw(3, 0);

    ID3D11ShaderResourceView* null_srv = nullptr;
    impl_->context->PSSetShaderResources(0, 1, &null_srv);
    impl_->swapchain->Present(vsync ? 1u : 0u, 0u);
#else
    (void)gpu;
    (void)full_vram;
    (void)widescreen;
    (void)vsync;
#endif
}

} // namespace vsport
