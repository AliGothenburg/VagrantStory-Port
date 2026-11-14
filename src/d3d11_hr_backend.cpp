// Direct3D 11 backend for the upscaled picture: the graphics card draws the queued primitives
// into a 1024*N x 512*N RGBA8 render target (alpha = GP0(E6h) mask bit). The pixel shader decodes
// PS1 textures (4/8/15 bpp, CLUT, texture window) straight from a copy of the native VRAM and
// uses the same integer colour math as the software backend (hr_software.cpp).
//
// Semi-transparency (4 PS1 modes, per texel), the mask bit and the soft rim light are done with
// blend states. Perspective-correct textures use u*q, v*q, q interpolated linearly; sprites that
// texture from their own draw area are drawn in row strips through a copy of the picture.
#include "vsport/d3d11_hr_backend.hpp"

#ifdef _WIN32
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vsport {

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

namespace {

constexpr const char* kHrHlsl = R"(
cbuffer HrCB : register(b0) {
    float2 target_size;   // Render target size in pixels (1024*N, 512*N)
    uint   scale;         // N
    uint   pad0;
};

Texture2D<uint> Vram : register(t0);    // Native 1024x512 VRAM (RGB555 + bit 15)
Texture2D<float4> Self : register(t1);  // Copy of the upscaled picture (self-texturing sprites)

struct VIn {
    float2 pos   : POSITION;   // Upscaled pixel position (+0.5, see the backend)
    float3 pr    : COLOR0;     // Vertex colour 0..255 as planes (see below): red
    float3 pg    : COLOR1;     // green
    float3 pb    : COLOR2;     // blue
    uint   tp    : TEXCOORD1;  // texpage | clut << 16
    uint   win   : TEXCOORD2;  // GP0(E2h) texture window
    uint   flags : TEXCOORD3;  // See FLAG_* in the backend
    int4   clip  : TEXCOORD4;  // Drawing area in upscaled pixels (x1, y1, x2, y2 inclusive)
    int4   rect  : TEXCOORD5;  // Sprite: rx, ry, base u, base v (native). Triangle: plane anchor x, y
    // Triangle texture planes A(px, py) = a.x + a.y * (px - anchor x) + a.z * (py - anchor y),
    // evaluated per pixel (exact like the software backend; interpolated floats are not)
    float3 pu    : TEXCOORD6;  // u
    float3 pv    : TEXCOORD7;  // v
    float3 pq    : TEXCOORD8;  // Perspective: q = relative 1/z
    float3 puq   : TEXCOORD9;  // Perspective: u*q
    float3 pvq   : TEXCOORD10; // Perspective: v*q
    float4 box   : TEXCOORD11; // Perspective: u/v range of the triangle (umin, vmin, umax, vmax)
};
struct VOut {
    float4 pos : SV_Position;
    nointerpolation float3 pr  : COLOR0;
    nointerpolation float3 pg  : COLOR1;
    nointerpolation float3 pb  : COLOR2;
    nointerpolation uint3 tpwf : TEXCOORD1; // tp, win, flags
    nointerpolation int4 clip  : TEXCOORD4;
    nointerpolation int4 rect  : TEXCOORD5;
    nointerpolation float3 pu  : TEXCOORD6;
    nointerpolation float3 pv  : TEXCOORD7;
    nointerpolation float3 pq  : TEXCOORD8;
    nointerpolation float3 puq : TEXCOORD9;
    nointerpolation float3 pvq : TEXCOORD10;
    nointerpolation float4 box : TEXCOORD11;
};

VOut VSMain(VIn i) {
    VOut o;
    // The viewport spans -4096..12288 on both axes (see begin_pass): triangles reaching outside the
    // picture are not clipped (clipping would make new vertices and change which pixels exactly on
    // an edge are drawn), and the power-of-two mapping keeps positions exact
    o.pos   = float4((i.pos.x + 4096.0f) / 8192.0f - 1.0f, 1.0f - (i.pos.y + 4096.0f) / 8192.0f, 0.0f, 1.0f);
    o.pr    = i.pr;
    o.pg    = i.pg;
    o.pb    = i.pb;
    o.tpwf  = uint3(i.tp, i.win, i.flags);
    o.clip  = i.clip;
    o.rect  = i.rect;
    o.pu    = i.pu;
    o.pv    = i.pv;
    o.pq    = i.pq;
    o.puq   = i.puq;
    o.pvq   = i.pvq;
    o.box   = i.box;
    return o;
}

uint px16(int x, int y) { return Vram.Load(int3(x & 1023, y & 511, 0)); }

uint fetch(uint u, uint v, uint tp, uint win) {
    u &= 255u; v &= 255u;
    const uint mx = (win & 31u) * 8u, my = ((win >> 5) & 31u) * 8u;
    const uint ox = ((win >> 10) & 31u) * 8u, oy = ((win >> 15) & 31u) * 8u;
    u = (u & ~mx) | (ox & mx);
    v = (v & ~my) | (oy & my);
    const uint texpage = tp & 0xFFFFu, clut = tp >> 16;
    const int tpx = int((texpage & 15u) * 64u), tpy = int(((texpage >> 4) & 1u) * 256u);
    const uint depth = (texpage >> 7) & 3u;
    const int cx = int((clut & 63u) * 16u), cy = int((clut >> 6) & 511u);
    if (depth == 0u) {
        const uint w = px16(tpx + int(u >> 2), tpy + int(v));
        return px16(cx + int((w >> ((u & 3u) * 4u)) & 15u), cy);
    }
    if (depth == 1u) {
        const uint w = px16(tpx + int(u >> 1), tpy + int(v));
        return px16(cx + int((w >> ((u & 1u) * 8u)) & 255u), cy);
    }
    return px16(tpx + int(u), tpy + int(v));
}

uint c5to8(uint c) { return (c * 527u + 23u) >> 6; }
uint mod_ch(uint t5, uint t8, uint c) { return ((t5 * c) >> 7) == 0u ? 0u : min(255u, (t8 * c) >> 7); }
uint flat_ch(uint c) { return c < 8u ? 0u : c; }

static const uint FLAG_TEXTURED = 1u, FLAG_RECT = 2u, FLAG_FILTER = 4u, FLAG_SET_MASK = 8u, FLAG_SYNC = 16u;
static const uint FLAG_STP_CLEAR_ONLY = 32u, FLAG_STP_SET_ONLY = 64u; // Semi-transparent polygon passes
static const uint FLAG_PERSP = 128u, FLAG_SELF = 256u, FLAG_MASK_READ = 512u;
static const float TEX_EPS = 1.0f / 4096.0f; // Keeps exact integer coordinates from rounding down

float plane(float3 a, float2 d) { return a.x + a.y * d.x + a.z * d.y; }

// Texture position of a triangle pixel: perspective-correct when possible, else affine
float2 tex_pos(VOut i) {
    const float2 d = float2(int(i.pos.x) - i.rect.x, int(i.pos.y) - i.rect.y);
    if ((i.tpwf.z & FLAG_PERSP) != 0u) {
        const float q = plane(i.pq, d);
        if (q > 0.0f) {
            return clamp(float2(plane(i.puq, d), plane(i.pvq, d)) / q + TEX_EPS, i.box.xy, i.box.zw);
        }
    }
    return float2(plane(i.pu, d), plane(i.pv, d)) + TEX_EPS;
}

float4 PSMain(VOut i) : SV_Target {
    const int px = int(i.pos.x), py = int(i.pos.y);
    if (px < i.clip.x || px > i.clip.z || py < i.clip.y || py > i.clip.w) discard;
    // Check mask (see emit_prim): protected pixels, read from the copy made before this draw
    if ((i.tpwf.z & FLAG_MASK_READ) != 0u && Self.Load(int3(px, py, 0)).a >= 0.5f) discard;
    const int s = int(scale);

    if ((i.tpwf.z & FLAG_SYNC) != 0u) {
        // Native -> upscaled pixel replication (fills, uploads)
        const uint n = px16(px / s, py / s);
        return float4(c5to8(n & 31u), c5to8((n >> 5) & 31u), c5to8((n >> 10) & 31u), (n & 0x8000u) != 0u ? 255.0f : 0.0f) / 255.0f;
    }

    // Vertex colour from its planes (as the texture position; constant for sprites and flat shading)
    const float2 cd = float2(px - i.rect.x, py - i.rect.y);
    const uint3 c = uint3(clamp(float3(plane(i.pr, cd), plane(i.pg, cd), plane(i.pb, cd)), 0.0f, 255.0f));
    const float mask_set = (i.tpwf.z & FLAG_SET_MASK) != 0u ? 255.0f : 0.0f;
    if ((i.tpwf.z & FLAG_TEXTURED) == 0u) {
        return float4(flat_ch(c.r), flat_ch(c.g), flat_ch(c.b), mask_set) / 255.0f;
    }

    uint texel;
    uint3 t8;
    bool transparent;
    if ((i.tpwf.z & FLAG_SELF) != 0u) {
        // Texture = this sprite's own draw area: read the upscaled picture (same sub-pixel). The
        // backend draws the sprite in row strips and copies the finished rows first, so rows
        // written earlier by this sprite are seen, as natively.
        const uint texpage = i.tpwf.x & 0xFFFFu;
        const int tpx = int((texpage & 15u) * 64u), tpy = int(((texpage >> 4) & 1u) * 256u);
        const int dx = px / s - i.rect.x, dy = py / s - i.rect.y;
        const int sx = ((tpx + ((i.rect.z + dx) & 255)) * s + px % s) % (1024 * s);
        const int sy = ((tpy + ((i.rect.w + dy) & 255)) * s + py % s) % (512 * s);
        const uint4 sc = uint4(Self.Load(int3(sx, sy, 0)) * 255.0f + 0.5f);
        transparent = (sc.r | sc.g | sc.b) == 0u && sc.a < 128u;
        t8 = sc.rgb;
        texel = (sc.r >> 3) | ((sc.g >> 3) << 5) | ((sc.b >> 3) << 10) | (sc.a >= 128u ? 0x8000u : 0u);
    } else if ((i.tpwf.z & FLAG_RECT) != 0u) {
        // Sprites keep their native texel grid
        const uint u = uint(i.rect.z + (px / s - i.rect.x)), v = uint(i.rect.w + (py / s - i.rect.y));
        texel = fetch(u, v, i.tpwf.x, i.tpwf.y);
        t8 = uint3(c5to8(texel & 31u), c5to8((texel >> 5) & 31u), c5to8((texel >> 10) & 31u));
        transparent = texel == 0u;
    } else if ((i.tpwf.z & FLAG_FILTER) == 0u) {
        const float2 st = tex_pos(i);
        texel = fetch(uint(int(st.x)), uint(int(st.y)), i.tpwf.x, i.tpwf.y);
        t8 = uint3(c5to8(texel & 31u), c5to8((texel >> 5) & 31u), c5to8((texel >> 10) & 31u));
        transparent = texel == 0u;
    } else {
        // Bilinear between the 4 surrounding texels; transparency follows the nearest one and
        // transparent neighbours are left out of the blend (as in the software backend)
        const float2 st = tex_pos(i);
        const float cu = st.x - 0.5f, cv = st.y - 0.5f;
        const float flu = floor(cu), flv = floor(cv);
        const float ax = cu - flu, ay = cv - flv;
        const uint iu = uint(int(flu)), iv = uint(int(flv));
        const uint t00 = fetch(iu, iv, i.tpwf.x, i.tpwf.y), t10 = fetch(iu + 1u, iv, i.tpwf.x, i.tpwf.y);
        const uint t01 = fetch(iu, iv + 1u, i.tpwf.x, i.tpwf.y), t11 = fetch(iu + 1u, iv + 1u, i.tpwf.x, i.tpwf.y);
        texel = (ay < 0.5f) ? ((ax < 0.5f) ? t00 : t10) : ((ax < 0.5f) ? t01 : t11);
        t8 = uint3(c5to8(texel & 31u), c5to8((texel >> 5) & 31u), c5to8((texel >> 10) & 31u));
        transparent = texel == 0u;
        const float w00 = t00 != 0u ? (1.0f - ax) * (1.0f - ay) : 0.0f, w10 = t10 != 0u ? ax * (1.0f - ay) : 0.0f;
        const float w01 = t01 != 0u ? (1.0f - ax) * ay : 0.0f,          w11 = t11 != 0u ? ax * ay : 0.0f;
        const float wsum = w00 + w10 + w01 + w11;
        if (wsum > 0.0f) {
            const float k = (255.0f / 31.0f) / wsum;
            [unroll] for (uint ch = 0u; ch < 3u; ++ch) {
                const uint sh = ch * 5u;
                const float f = (w00 * float((t00 >> sh) & 31u) + w10 * float((t10 >> sh) & 31u) +
                                 w01 * float((t01 >> sh) & 31u) + w11 * float((t11 >> sh) & 31u)) * k;
                t8[ch] = uint(f + 0.5f);
            }
        }
    }
    if (transparent) discard;
    // Semi-transparent polygons are drawn in two passes: opaque texels (STP bit clear), then the
    // semi-transparent ones (STP bit set) with blending
    if ((i.tpwf.z & FLAG_STP_CLEAR_ONLY) != 0u && (texel & 0x8000u) != 0u) discard;
    if ((i.tpwf.z & FLAG_STP_SET_ONLY) != 0u && (texel & 0x8000u) == 0u) discard;

    const uint r = mod_ch(texel & 31u, t8.r, c.r);
    const uint g = mod_ch((texel >> 5) & 31u, t8.g, c.g);
    const uint b = mod_ch((texel >> 10) & 31u, t8.b, c.b);
    return float4(r, g, b, max(mask_set, (texel & 0x8000u) != 0u ? 255.0f : 0.0f)) / 255.0f;
}
)";

struct HrVertex {
    float x, y;
    float pr[3], pg[3], pb[3]; // Colour planes (see the shader)
    u32 tp;
    u32 win;
    u32 flags;
    s32 clip[4];
    s32 rect[4];
    float pu[3], pv[3], pq[3], puq[3], pvq[3]; // Texture planes (see the shader)
    float box[4];
};

constexpr u32 FLAG_TEXTURED = 1u, FLAG_RECT = 2u, FLAG_FILTER = 4u, FLAG_SET_MASK = 8u, FLAG_SYNC = 16u;
constexpr u32 FLAG_STP_CLEAR_ONLY = 32u, FLAG_STP_SET_ONLY = 64u;
constexpr u32 FLAG_PERSP = 128u, FLAG_SELF = 256u, FLAG_MASK_READ = 512u;

// How a run of vertices is blended into the picture
enum BlendMode : u8 {
    BLEND_OPAQUE = 0,
    BLEND_HALF   = 1, // ABR 0: (B + F) / 2
    BLEND_ADD    = 2, // ABR 1: B + F
    BLEND_SUB    = 3, // ABR 2: B - F
    BLEND_QUART  = 4, // ABR 3: B + F / 4
    BLEND_MIX    = 5, // Soft rim light: B + (F - B) * factor
    BLEND_COUNT  = 6
};

struct DrawCmd {
    u32 start = 0, count = 0;
    u8 blend = BLEND_OPAQUE;
    bool check_mask = false;
    float factor = 0.0f;
    u32 copy_first = 0, copy_count = 0; // Picture -> temp_ copies done before this draw (copies_)
};

struct alignas(16) HrConstants {
    float target_w, target_h;
    u32 scale, pad0;
};

class D3D11HrBackend final : public HrBackend {
public:
    bool init(ID3D11Device* shared, std::string* err);

    void set_scale(u32 scale, const u16* native_vram) override;
    void set_options(const HrOptions& o) override { opt_ = o; }
    void draw(const std::vector<HrPrim>& batch, s32 native_y1, s32 native_y2, const u16* native_vram) override;
    void sync(u32 x, u32 y, u32 w, u32 h, const u16* native_vram) override;
    void copy(u32 sx, u32 sy, u32 dx, u32 dy, u32 w, u32 h) override;
    void clear() override;
    const std::vector<u32>& picture() override;
    void* gpu_device() override { return device_.Get(); }
    void* gpu_picture_view() override { return target_srv_.Get(); }

private:
    bool create_targets();
    void upload_vram(const u16* native_vram);
    void begin_pass();
    void push_triangle(const HrPrim& p);
    void push_rect(const HrPrim& p);
    void draw_self_rect(const HrPrim& p);
    void push_quad(const HrVertex& proto, float x0, float y0, float x1, float y1);
    // Appends n vertices as one draw step (merged with the previous one if the state is the same)
    void emit(const HrVertex* v, u32 n, u32 extra_flags, u8 blend, bool check_mask, float factor = 0.0f);
    // Emits a primitive with the right passes for its semi-transparency
    void emit_prim(const HrPrim& p, const HrVertex* v, u32 n);
    void flush_vertices();
    void reset_lists() { verts_.clear(); cmds_.clear(); copies_.clear(); pending_copies_ = 0; }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11Texture2D> target_, temp_, staging_;
    ComPtr<ID3D11RenderTargetView> target_rtv_;
    ComPtr<ID3D11ShaderResourceView> target_srv_, temp_srv_;
    ComPtr<ID3D11Texture2D> vram_tex_;
    ComPtr<ID3D11ShaderResourceView> vram_srv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11InputLayout> layout_;
    ComPtr<ID3D11Buffer> cbuffer_, vbuffer_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11BlendState> blend_[BLEND_COUNT][2]; // [mode][check mask]
    u32 vbuffer_capacity_ = 0;
    std::vector<HrVertex> verts_;
    std::vector<DrawCmd> cmds_;
    std::vector<D3D11_BOX> copies_;   // Regions copied to temp_ (same position) between draws
    u32 pending_copies_ = 0;          // Copies at the end of copies_ for the next draw step
    u8 cur_blend_ = BLEND_OPAQUE; // Blend mode of the primitive being pushed (set by emit_prim)
    bool cur_check_ = false;
    float cur_factor_ = 0.0f;
    std::vector<u32> picture_;
    bool picture_dirty_ = true;
    u32 scale_ = 1;
    HrOptions opt_;
};

bool D3D11HrBackend::init(ID3D11Device* shared, std::string* err) {
    const auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (shared) {
        device_ = shared;
    } else {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3, D3D11_SDK_VERSION,
                                       &device_, nullptr, nullptr);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 3, D3D11_SDK_VERSION,
                                   &device_, nullptr, nullptr);
        }
        if (FAILED(hr)) return fail("D3D11 cihazi olusturulamadi");
    }
    device_->GetImmediateContext(&ctx_);

    ComPtr<ID3DBlob> vs_blob, ps_blob, err_blob;
    if (FAILED(D3DCompile(kHrHlsl, std::strlen(kHrHlsl), "hr.hlsl", nullptr, nullptr, "VSMain", "vs_4_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs_blob, &err_blob)) ||
        FAILED(D3DCompile(kHrHlsl, std::strlen(kHrHlsl), "hr.hlsl", nullptr, nullptr, "PSMain", "ps_4_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps_blob, &err_blob))) {
        if (err && err_blob) *err = static_cast<const char*>(err_blob->GetBufferPointer());
        return false;
    }
    if (FAILED(device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs_)) ||
        FAILED(device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &ps_))) {
        return fail("HLSL shader nesneleri olusturulamadi");
    }
    const D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(HrVertex, x), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(HrVertex, pr), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(HrVertex, pg), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(HrVertex, pb), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, offsetof(HrVertex, tp), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32_UINT, 0, offsetof(HrVertex, win), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 3, DXGI_FORMAT_R32_UINT, 0, offsetof(HrVertex, flags), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_SINT, 0, offsetof(HrVertex, clip), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 5, DXGI_FORMAT_R32G32B32A32_SINT, 0, offsetof(HrVertex, rect), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 6, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(HrVertex, pu), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 7, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(HrVertex, pv), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 8, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(HrVertex, pq), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 9, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(HrVertex, puq), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 10, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(HrVertex, pvq), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 11, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(HrVertex, box), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(device_->CreateInputLayout(il, static_cast<UINT>(std::size(il)), vs_blob->GetBufferPointer(),
                                          vs_blob->GetBufferSize(), &layout_))) {
        return fail("D3D11 girdi duzeni olusturulamadi");
    }

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth      = sizeof(HrConstants);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&cbd, nullptr, &cbuffer_))) return fail("D3D11 sabit tamponu olusturulamadi");

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    if (FAILED(device_->CreateRasterizerState(&rd, &raster_))) return fail("D3D11 rasterizer durumu olusturulamadi");

    // PS1 semi-transparency as blend states. Alpha (= mask bit) always takes the drawn value.
    // "Check mask" (GP0 E6h): the picture's alpha is exactly 0 or 1, so weighting the source by
    // (1 - dest alpha) and keeping the destination where alpha is 1 leaves protected pixels alone
    // (exact for opaque, B+F and B-F; for (B+F)/2 and B+F/4 the source is masked out only).
    for (int mode = 0; mode < BLEND_COUNT; ++mode) {
        for (int check = 0; check < 2; ++check) {
            D3D11_BLEND_DESC bd{};
            D3D11_RENDER_TARGET_BLEND_DESC& t = bd.RenderTarget[0];
            t.BlendEnable           = TRUE;
            t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            t.BlendOp = D3D11_BLEND_OP_ADD;
            t.SrcBlendAlpha  = check ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_ONE;
            t.DestBlendAlpha = check ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_ZERO;
            t.BlendOpAlpha   = D3D11_BLEND_OP_ADD;
            const D3D11_BLEND src_masked = check ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_ONE;
            switch (mode) {
                case BLEND_OPAQUE:
                    t.SrcBlend = src_masked; t.DestBlend = check ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_ZERO;
                    break;
                case BLEND_HALF:  t.SrcBlend = D3D11_BLEND_BLEND_FACTOR; t.DestBlend = D3D11_BLEND_BLEND_FACTOR; break;
                case BLEND_ADD:   t.SrcBlend = src_masked; t.DestBlend = D3D11_BLEND_ONE; break;
                case BLEND_SUB:   t.SrcBlend = src_masked; t.DestBlend = D3D11_BLEND_ONE; t.BlendOp = D3D11_BLEND_OP_REV_SUBTRACT; break;
                case BLEND_QUART: t.SrcBlend = D3D11_BLEND_BLEND_FACTOR; t.DestBlend = D3D11_BLEND_ONE; break;
                case BLEND_MIX:   t.SrcBlend = D3D11_BLEND_BLEND_FACTOR; t.DestBlend = D3D11_BLEND_INV_BLEND_FACTOR; break;
                default: break;
            }
            if (FAILED(device_->CreateBlendState(&bd, &blend_[mode][check]))) return fail("D3D11 karisim durumu olusturulamadi");
        }
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = VRAM_WIDTH;
    td.Height           = VRAM_HEIGHT;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R16_UINT;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DYNAMIC;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &vram_tex_)) ||
        FAILED(device_->CreateShaderResourceView(vram_tex_.Get(), nullptr, &vram_srv_))) {
        return fail("D3D11 VRAM dokusu olusturulamadi");
    }
    return true;
}

bool D3D11HrBackend::create_targets() {
    target_rtv_.Reset();
    target_srv_.Reset();
    target_.Reset();
    temp_srv_.Reset();
    temp_.Reset();
    staging_.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = VRAM_WIDTH * scale_;
    td.Height           = VRAM_HEIGHT * scale_;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &target_)) ||
        FAILED(device_->CreateRenderTargetView(target_.Get(), nullptr, &target_rtv_)) ||
        FAILED(device_->CreateShaderResourceView(target_.Get(), nullptr, &target_srv_))) {
        return false;
    }
    // Temporary copy: for overlapping copies and as the texture of self-texturing sprites
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &temp_)) ||
        FAILED(device_->CreateShaderResourceView(temp_.Get(), nullptr, &temp_srv_))) {
        return false;
    }
    td.BindFlags = 0;
    td.Usage          = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(device_->CreateTexture2D(&td, nullptr, &staging_));
}

void D3D11HrBackend::set_scale(u32 scale, const u16* native_vram) {
    scale_ = std::max(1u, scale);
    picture_dirty_ = true;
    if (scale_ <= 1u) {
        target_rtv_.Reset(); target_srv_.Reset(); target_.Reset(); temp_srv_.Reset(); temp_.Reset(); staging_.Reset();
        return;
    }
    if (!create_targets()) {
        scale_ = 1;
        return;
    }
    clear();
    sync(0, 0, VRAM_WIDTH, VRAM_HEIGHT, native_vram);
}

void D3D11HrBackend::clear() {
    if (!target_rtv_) return;
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ctx_->ClearRenderTargetView(target_rtv_.Get(), zero);
    picture_dirty_ = true;
}

void D3D11HrBackend::upload_vram(const u16* native_vram) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(vram_tex_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    auto* dst = static_cast<u8*>(m.pData);
    for (u32 y = 0; y < VRAM_HEIGHT; ++y) {
        std::memcpy(dst + static_cast<std::size_t>(y) * m.RowPitch, native_vram + y * VRAM_WIDTH, VRAM_WIDTH * sizeof(u16));
    }
    ctx_->Unmap(vram_tex_.Get(), 0);
}

void D3D11HrBackend::begin_pass() {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx_->Map(cbuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        auto* c = static_cast<HrConstants*>(m.pData);
        c->target_w = static_cast<float>(VRAM_WIDTH * scale_);
        c->target_h = static_cast<float>(VRAM_HEIGHT * scale_);
        c->scale    = scale_;
        c->pad0     = 0;
        ctx_->Unmap(cbuffer_.Get(), 0);
    }
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = -4096.0f; // Larger than the picture (see the vertex shader); pixels outside are not written
    vp.TopLeftY = -4096.0f;
    vp.Width    = 16384.0f;
    vp.Height   = 16384.0f;
    vp.MaxDepth = 1.0f;
    ctx_->OMSetRenderTargets(1, target_rtv_.GetAddressOf(), nullptr);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    ctx_->OMSetDepthStencilState(nullptr, 0);
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetState(raster_.Get());
    ctx_->IASetInputLayout(layout_.Get());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, cbuffer_.GetAddressOf());
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, cbuffer_.GetAddressOf());
    ID3D11ShaderResourceView* srvs[2] = {vram_srv_.Get(), temp_srv_.Get()};
    ctx_->PSSetShaderResources(0, 2, srvs);
}

void D3D11HrBackend::flush_vertices() {
    if (verts_.empty()) return;
    const u32 bytes = static_cast<u32>(verts_.size() * sizeof(HrVertex));
    if (verts_.empty() || cmds_.empty()) { reset_lists(); return; }
    if (bytes > vbuffer_capacity_) {
        u32 cap = std::max(vbuffer_capacity_, 1u << 20);
        while (cap < bytes) cap *= 2;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = cap;
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        vbuffer_.Reset();
        if (FAILED(device_->CreateBuffer(&bd, nullptr, &vbuffer_))) { vbuffer_capacity_ = 0; reset_lists(); return; }
        vbuffer_capacity_ = cap;
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(vbuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { reset_lists(); return; }
    std::memcpy(m.pData, verts_.data(), bytes);
    ctx_->Unmap(vbuffer_.Get(), 0);
    const UINT stride = sizeof(HrVertex), offset = 0;
    ctx_->IASetVertexBuffers(0, 1, vbuffer_.GetAddressOf(), &stride, &offset);
    for (const DrawCmd& c : cmds_) {
        for (u32 k = 0; k < c.copy_count; ++k) {
            const D3D11_BOX& b = copies_[c.copy_first + k];
            ctx_->CopySubresourceRegion(temp_.Get(), 0, b.left, b.top, 0, target_.Get(), 0, &b);
        }
        float f = 0.5f;                             // (B + F) / 2
        if (c.blend == BLEND_QUART) f = 0.25f;      // B + F / 4
        if (c.blend == BLEND_MIX)   f = c.factor;   // Soft rim light
        const float factor[4] = {f, f, f, f};
        if (c.blend == BLEND_OPAQUE && !c.check_mask) {
            ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        } else {
            ctx_->OMSetBlendState(blend_[c.blend][c.check_mask ? 1 : 0].Get(), factor, 0xFFFFFFFFu);
        }
        ctx_->Draw(c.count, c.start);
    }
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    reset_lists();
    picture_dirty_ = true;
}

namespace {
void make_quad(const HrVertex& proto, float x0, float y0, float x1, float y1, HrVertex out[6]) {
    // +0.5: Direct3D tests pixel centres, the PS1 rules (and the software backend) pixel corners
    HrVertex a = proto, b = proto, c = proto, d = proto;
    a.x = x0 + 0.5f; a.y = y0 + 0.5f;
    b.x = x1 + 0.5f; b.y = y0 + 0.5f;
    c.x = x0 + 0.5f; c.y = y1 + 0.5f;
    d.x = x1 + 0.5f; d.y = y1 + 0.5f;
    out[0] = a; out[1] = b; out[2] = c; out[3] = b; out[4] = d; out[5] = c;
}
} // namespace

void D3D11HrBackend::push_quad(const HrVertex& proto, float x0, float y0, float x1, float y1) {
    HrVertex q[6];
    make_quad(proto, x0, y0, x1, y1, q);
    emit(q, 6, 0u, BLEND_OPAQUE, false);
}

void D3D11HrBackend::emit(const HrVertex* v, u32 n, u32 extra_flags, u8 blend, bool check_mask, float factor) {
    const u32 start = static_cast<u32>(verts_.size());
    for (u32 i = 0; i < n; ++i) {
        verts_.push_back(v[i]);
        verts_.back().flags |= extra_flags;
    }
    // Merge with the previous step when the state is the same (order is kept inside a draw)
    // (never into or after a step that needs fresh copies: its copy holds only its own area)
    if (!cmds_.empty() && pending_copies_ == 0 && cmds_.back().copy_count == 0) {
        DrawCmd& last = cmds_.back();
        if (last.blend == blend && last.check_mask == check_mask && last.factor == factor && last.start + last.count == start) {
            last.count += n;
            return;
        }
    }
    DrawCmd c;
    c.start = start; c.count = n; c.blend = blend; c.check_mask = check_mask; c.factor = factor;
    c.copy_first = static_cast<u32>(copies_.size()) - pending_copies_;
    c.copy_count = pending_copies_;
    pending_copies_ = 0;
    cmds_.push_back(c);
}

void D3D11HrBackend::emit_prim(const HrPrim& p, const HrVertex* v, u32 n) {
    const u8 abr_mode = static_cast<u8>(BLEND_HALF + ((p.texpage >> 5) & 3u));
    // "Check mask" with (B+F)/2 or B+F/4: the blend state cannot keep protected pixels (their
    // destination would still be scaled), so the shader reads the mask from a copy of the picture
    // taken just before this primitive and skips them
    const bool mask_read = p.mcheck && (abr_mode == BLEND_HALF || abr_mode == BLEND_QUART);
    const auto copy_bbox = [&]() {
        float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
        for (u32 i = 1; i < n; ++i) {
            x0 = std::min(x0, v[i].x); x1 = std::max(x1, v[i].x);
            y0 = std::min(y0, v[i].y); y1 = std::max(y1, v[i].y);
        }
        const s32 bx0 = std::max({static_cast<s32>(std::floor(x0)), v[0].clip[0], 0});
        const s32 by0 = std::max({static_cast<s32>(std::floor(y0)), v[0].clip[1], 0});
        const s32 bx1 = std::min({static_cast<s32>(std::ceil(x1)), v[0].clip[2] + 1, static_cast<s32>(VRAM_WIDTH * scale_)});
        const s32 by1 = std::min({static_cast<s32>(std::ceil(y1)), v[0].clip[3] + 1, static_cast<s32>(VRAM_HEIGHT * scale_)});
        if (bx0 >= bx1 || by0 >= by1) return;
        D3D11_BOX b{};
        b.left = static_cast<UINT>(bx0); b.right = static_cast<UINT>(bx1);
        b.top = static_cast<UINT>(by0); b.bottom = static_cast<UINT>(by1);
        b.back = 1;
        copies_.push_back(b);
        ++pending_copies_;
    };
    if (cur_blend_ == BLEND_MIX) {
        emit(v, n, 0u, BLEND_MIX, false, cur_factor_);
    } else if (!p.semi) {
        emit(v, n, 0u, BLEND_OPAQUE, p.mcheck);
    } else if (!p.textured) {
        if (mask_read) {
            copy_bbox();
            emit(v, n, FLAG_MASK_READ, abr_mode, false);
        } else {
            emit(v, n, 0u, abr_mode, p.mcheck);
        }
    } else {
        // Per-texel semi-transparency: the two passes cover disjoint pixels
        emit(v, n, FLAG_STP_CLEAR_ONLY, BLEND_OPAQUE, p.mcheck);
        if (mask_read) {
            copy_bbox();
            emit(v, n, FLAG_STP_SET_ONLY | FLAG_MASK_READ, abr_mode, false);
        } else {
            emit(v, n, FLAG_STP_SET_ONLY, abr_mode, p.mcheck);
        }
    }
}

void D3D11HrBackend::push_triangle(const HrPrim& p) {
    const s32 s = static_cast<s32>(scale_);
    const GpuVertex* v = p.v;
    const s32 mnx = std::min({v[0].x, v[1].x, v[2].x}), mxx = std::max({v[0].x, v[1].x, v[2].x});
    const s32 mny = std::min({v[0].y, v[1].y, v[2].y}), mxy = std::max({v[0].y, v[1].y, v[2].y});
    if ((mxx - mnx) >= 1024 || (mxy - mny) >= 512) return; // Same rejection as the PS1
    HrVertex proto{};
    proto.tp    = static_cast<u32>(p.texpage) | (static_cast<u32>(p.clut) << 16);
    proto.win   = p.texwin;
    proto.flags = (p.textured ? FLAG_TEXTURED : 0u) | ((p.textured && opt_.texture_filter) ? FLAG_FILTER : 0u) |
                  (p.mset ? FLAG_SET_MASK : 0u);
    proto.clip[0] = p.cx1 * s; proto.clip[1] = p.cy1 * s;
    proto.clip[2] = (p.cx2 + 1) * s - 1; proto.clip[3] = (p.cy2 + 1) * s - 1;
    s32 X[3], Y[3];
    for (int i = 0; i < 3; ++i) {
        // Exact GTE position when known (as the software backend: floor to an upscaled pixel)
        X[i] = (v[i].exact ? static_cast<s32>(std::floor(v[i].fx * static_cast<float>(s))) : v[i].x * s) + p.ox[i];
        Y[i] = (v[i].exact ? static_cast<s32>(std::floor(v[i].fy * static_cast<float>(s))) : v[i].y * s) + p.oy[i];
    }
    const s64 det = static_cast<s64>(X[1] - X[0]) * (Y[2] - Y[0]) - static_cast<s64>(X[2] - X[0]) * (Y[1] - Y[0]);
    if (det == 0) return; // No area (as the software backend)
    // Colour and texture planes in double precision, anchored at vertex 2 (see the shader)
    const double inv = 1.0 / static_cast<double>(det);
    const auto make_plane = [&](const double a[3], float out[3]) {
        const double dx = ((a[1] - a[0]) * (Y[2] - Y[0]) - (a[2] - a[0]) * (Y[1] - Y[0])) * inv;
        const double dy = ((a[2] - a[0]) * (X[1] - X[0]) - (a[1] - a[0]) * (X[2] - X[0])) * inv;
        out[0] = static_cast<float>(a[2]); out[1] = static_cast<float>(dx); out[2] = static_cast<float>(dy);
    };
    proto.rect[0] = X[2]; proto.rect[1] = Y[2];
    {
        const GpuVertex* cv[3] = {&v[0], p.shaded ? &v[1] : &v[0], p.shaded ? &v[2] : &v[0]};
        const double rs[3] = {double(cv[0]->r), double(cv[1]->r), double(cv[2]->r)};
        const double gs[3] = {double(cv[0]->g), double(cv[1]->g), double(cv[2]->g)};
        const double bs[3] = {double(cv[0]->b), double(cv[1]->b), double(cv[2]->b)};
        make_plane(rs, proto.pr);
        make_plane(gs, proto.pg);
        make_plane(bs, proto.pb);
    }
    if (p.textured) {
        const double us[3] = {static_cast<double>(v[0].u), static_cast<double>(v[1].u), static_cast<double>(v[2].u)};
        const double vs[3] = {static_cast<double>(v[0].v), static_cast<double>(v[1].v), static_cast<double>(v[2].v)};
        make_plane(us, proto.pu);
        make_plane(vs, proto.pv);
        // Perspective-correct texturing (as the software backend): with exact vertices and their
        // depth, u*q, v*q and q = 1/z vary linearly across the screen. q is taken relative to
        // the nearest vertex so it stays near 1.
        bool persp = opt_.perspective;
        for (int i = 0; i < 3 && persp; ++i) persp = v[i].exact && v[i].fz > 0.0f;
        if (persp) {
            proto.flags |= FLAG_PERSP;
            const double zmin = std::min({v[0].fz, v[1].fz, v[2].fz});
            double q[3], uq[3], vq[3];
            for (int i = 0; i < 3; ++i) {
                q[i] = zmin / static_cast<double>(v[i].fz);
                uq[i] = us[i] * q[i];
                vq[i] = vs[i] * q[i];
            }
            make_plane(q, proto.pq);
            make_plane(uq, proto.puq);
            make_plane(vq, proto.pvq);
            float umin = static_cast<float>(std::min({v[0].u, v[1].u, v[2].u}));
            float vmin = static_cast<float>(std::min({v[0].v, v[1].v, v[2].v}));
            float umax = static_cast<float>(std::max({v[0].u, v[1].u, v[2].u})) + 0.999f;
            float vmax = static_cast<float>(std::max({v[0].v, v[1].v, v[2].v})) + 0.999f;
            // A nearly constant coordinate stays exact (see TEX_EPS in the shader)
            if (umax - umin < 1.0f) umax = umin + 2.0f / 4096.0f;
            if (vmax - vmin < 1.0f) vmax = vmin + 2.0f / 4096.0f;
            proto.box[0] = umin; proto.box[1] = vmin; proto.box[2] = umax; proto.box[3] = vmax;
        }
    }
    HrVertex tri[3];
    for (int i = 0; i < 3; ++i) {
        HrVertex& o = tri[i];
        o = proto;
        o.x = static_cast<float>(X[i]) + 0.5f;
        o.y = static_cast<float>(Y[i]) + 0.5f;
    }
    emit_prim(p, tri, 3);
}

void D3D11HrBackend::push_rect(const HrPrim& p) {
    const s32 s = static_cast<s32>(scale_);
    HrVertex proto{};
    proto.tp    = static_cast<u32>(p.texpage) | (static_cast<u32>(p.clut) << 16);
    proto.win   = p.texwin;
    proto.flags = FLAG_RECT | (p.textured ? FLAG_TEXTURED : 0u) | (p.mset ? FLAG_SET_MASK : 0u);
    proto.pr[0] = p.cr; proto.pg[0] = p.cg; proto.pb[0] = p.cb; // Constant colour planes
    proto.clip[0] = p.cx1 * s; proto.clip[1] = p.cy1 * s;
    proto.clip[2] = (p.cx2 + 1) * s - 1; proto.clip[3] = (p.cy2 + 1) * s - 1;
    proto.rect[0] = p.rx; proto.rect[1] = p.ry; proto.rect[2] = p.bu; proto.rect[3] = p.bv;
    HrVertex q[6];
    make_quad(proto, static_cast<float>(p.rx * s), static_cast<float>(p.ry * s),
              static_cast<float>((p.rx + p.rw) * s), static_cast<float>((p.ry + p.rh) * s), q);
    emit_prim(p, q, 6);
}

void D3D11HrBackend::draw_self_rect(const HrPrim& p) {
    // The sprite's texture is its own draw area (the game's trail/blur effect, e.g. a copy shifted
    // by 2 rows). A pixel shader cannot read the picture it draws to, so the sprite is drawn in
    // strips of native rows: before each strip, the rows it reads are copied to temp_ (read by the
    // shader). A strip ends before a row whose source is a row of the same strip that is written
    // first; sources below (written later) correctly see the old picture, as natively.
    // All strips go out in one vertex upload, with the copies between their draw steps.
    const s32 s = static_cast<s32>(scale_);
    const s32 y_lo = std::max(p.ry, p.cy1), y_hi = std::min(p.ry + p.rh, p.cy2 + 1); // [y_lo, y_hi)
    if (y_lo >= y_hi || p.rw <= 0) return;
    const s32 tp_x = static_cast<s32>((p.texpage & 15u) * 64u);
    const s32 tp_y = static_cast<s32>(((p.texpage >> 4) & 1u) * 256u);
    // Columns read: the texture page (u wraps inside it), or all if it runs past the VRAM edge
    const UINT col0 = tp_x + 256 <= 1024 ? static_cast<UINT>(tp_x * s) : 0u;
    const UINT col1 = tp_x + 256 <= 1024 ? static_cast<UINT>((tp_x + 256) * s) : VRAM_WIDTH * scale_;
    const auto src_row = [&](s32 y) { return (tp_y + ((p.bv + (y - p.ry)) & 0xFF)) & 511; };
    const auto in_strip = [](s32 r, s32 a, s32 b) { return ((r - a) & 511) < (b - a); }; // r in [a, b) mod 512
    for (s32 a = y_lo; a < y_hi;) {
        s32 b = a + 1;
        while (b < y_hi && !in_strip(src_row(b), a, b)) ++b;
        // Copy the strip's source rows, merged in runs
        for (s32 y = a; y < b;) {
            const s32 r0 = src_row(y);
            s32 n = 1;
            while (y + n < b && src_row(y + n) == r0 + n && r0 + n < 512) ++n;
            D3D11_BOX box{};
            box.left = col0; box.right = col1;
            box.top = static_cast<UINT>(r0 * s); box.bottom = static_cast<UINT>((r0 + n) * s);
            box.back = 1;
            copies_.push_back(box);
            ++pending_copies_;
            y += n;
        }
        HrVertex proto{};
        proto.tp    = static_cast<u32>(p.texpage) | (static_cast<u32>(p.clut) << 16);
        proto.win   = p.texwin;
        proto.flags = FLAG_RECT | FLAG_TEXTURED | FLAG_SELF | (p.mset ? FLAG_SET_MASK : 0u);
        proto.pr[0] = p.cr; proto.pg[0] = p.cg; proto.pb[0] = p.cb; // Constant colour planes
        proto.clip[0] = p.cx1 * s; proto.clip[1] = p.cy1 * s;
        proto.clip[2] = (p.cx2 + 1) * s - 1; proto.clip[3] = (p.cy2 + 1) * s - 1;
        proto.rect[0] = p.rx; proto.rect[1] = p.ry; proto.rect[2] = p.bu; proto.rect[3] = p.bv;
        HrVertex q[6];
        make_quad(proto, static_cast<float>(p.rx * s), static_cast<float>(a * s),
                  static_cast<float>((p.rx + p.rw) * s), static_cast<float>(b * s), q);
        emit_prim(p, q, 6);
        a = b;
    }
}

void D3D11HrBackend::draw(const std::vector<HrPrim>& batch, s32, s32, const u16* native_vram) {
    if (batch.empty() || scale_ <= 1u || !target_rtv_) return;
    upload_vram(native_vram);
    begin_pass();
    verts_.reserve(batch.size() * 6);
    for (const HrPrim& p : batch) {
        if (p.is_rect && p.self_tex && p.textured) {
            draw_self_rect(p);
        } else if (p.is_rect) {
            push_rect(p);
        } else if (p.rim && opt_.soft_rim) {
            // Soft rim light: the bright copy at the game's full shift and at half of it, partly
            // transparent (as the software backend; p.ox/oy hold the "sharp" offset)
            HrPrim q = p;
            cur_blend_ = BLEND_MIX;
            for (int i = 0; i < 3; ++i) { q.ox[i] = 0; q.oy[i] = 0; }
            cur_factor_ = 0.30f;
            push_triangle(q);
            for (int i = 0; i < 3; ++i) { q.ox[i] = p.ox[i] / 2; q.oy[i] = p.oy[i] / 2; }
            cur_factor_ = 0.40f;
            push_triangle(q);
            cur_blend_ = BLEND_OPAQUE;
        } else {
            push_triangle(p);
        }
    }
    flush_vertices();
}

void D3D11HrBackend::sync(u32 x, u32 y, u32 w, u32 h, const u16* native_vram) {
    if (scale_ <= 1u || !target_rtv_ || w == 0 || h == 0) return;
    upload_vram(native_vram);
    begin_pass();
    const s32 s = static_cast<s32>(scale_);
    // Split at the VRAM edges (the rectangle wraps around like the native VRAM)
    for (u32 yo = 0; yo < h;) {
        const u32 ny = (y + yo) & 511u, hh = std::min(h - yo, VRAM_HEIGHT - ny);
        for (u32 xo = 0; xo < w;) {
            const u32 nx = (x + xo) & 1023u, ww = std::min(w - xo, VRAM_WIDTH - nx);
            HrVertex proto{};
            proto.flags = FLAG_SYNC;
            proto.clip[0] = static_cast<s32>(nx) * s; proto.clip[1] = static_cast<s32>(ny) * s;
            proto.clip[2] = static_cast<s32>(nx + ww) * s - 1; proto.clip[3] = static_cast<s32>(ny + hh) * s - 1;
            push_quad(proto, static_cast<float>(nx * s), static_cast<float>(ny * s),
                      static_cast<float>((nx + ww) * s), static_cast<float>((ny + hh) * s));
            xo += ww;
        }
        yo += hh;
    }
    flush_vertices();
}

void D3D11HrBackend::copy(u32 sx, u32 sy, u32 dx, u32 dy, u32 w, u32 h) {
    if (scale_ <= 1u || !target_ || w == 0 || h == 0) return;
    const u32 s = scale_;
    // Pieces in which neither the source nor the destination wraps around the VRAM edge
    const auto cuts = [](u32 a, u32 b, u32 len, u32 limit) {
        std::vector<u32> c{0, len};
        if (a + len > limit) c.push_back(limit - a);
        if (b + len > limit) c.push_back(limit - b);
        std::sort(c.begin(), c.end());
        c.erase(std::unique(c.begin(), c.end()), c.end());
        return c;
    };
    const std::vector<u32> xs = cuts(sx, dx, w, VRAM_WIDTH), ys = cuts(sy, dy, h, VRAM_HEIGHT);
    // Through a temporary texture: source and destination may overlap
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t j = 0; j + 1 < ys.size(); ++j) {
            for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
                const u32 ox = xs[i], oy = ys[j], pw = xs[i + 1] - ox, ph = ys[j + 1] - oy;
                const u32 srcx = (sx + ox) & 1023u, srcy = (sy + oy) & 511u;
                const u32 dstx = (dx + ox) & 1023u, dsty = (dy + oy) & 511u;
                D3D11_BOX box{};
                box.back = 1;
                if (pass == 0) {
                    box.left = srcx * s; box.top = srcy * s; box.right = (srcx + pw) * s; box.bottom = (srcy + ph) * s;
                    ctx_->CopySubresourceRegion(temp_.Get(), 0, srcx * s, srcy * s, 0, target_.Get(), 0, &box);
                } else {
                    box.left = srcx * s; box.top = srcy * s; box.right = (srcx + pw) * s; box.bottom = (srcy + ph) * s;
                    ctx_->CopySubresourceRegion(target_.Get(), 0, dstx * s, dsty * s, 0, temp_.Get(), 0, &box);
                }
            }
        }
    }
    picture_dirty_ = true;
}

const std::vector<u32>& D3D11HrBackend::picture() {
    if (scale_ <= 1u || !target_) {
        picture_.clear();
        return picture_;
    }
    if (!picture_dirty_) return picture_;
    const u32 w = VRAM_WIDTH * scale_, h = VRAM_HEIGHT * scale_;
    picture_.resize(static_cast<std::size_t>(w) * h);
    ctx_->CopyResource(staging_.Get(), target_.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        for (u32 y = 0; y < h; ++y) {
            const u32* src = reinterpret_cast<const u32*>(static_cast<const u8*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch);
            u32* dst = &picture_[static_cast<std::size_t>(y) * w];
            for (u32 x = 0; x < w; ++x) {
                const u32 c = src[x]; // RGBA8 in memory = 0xAABBGGRR
                dst[x] = (c & 0x00FFFFFFu) | ((c >> 24) >= 128u ? 0x01000000u : 0u);
            }
        }
        ctx_->Unmap(staging_.Get(), 0);
    }
    picture_dirty_ = false;
    return picture_;
}

} // namespace

std::unique_ptr<HrBackend> make_d3d11_hr_backend(void* device, std::string* err) {
    auto b = std::make_unique<D3D11HrBackend>();
    if (!b->init(static_cast<ID3D11Device*>(device), err)) return nullptr;
    return b;
}

#else

std::unique_ptr<HrBackend> make_d3d11_hr_backend(void*, std::string* err) {
    if (err) *err = "Direct3D 11 yalnizca Windows'ta var";
    return nullptr;
}

#endif

} // namespace vsport
