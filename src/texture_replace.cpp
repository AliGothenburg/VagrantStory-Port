// Texture replacement packs (see texture_replace.hpp): key index, lazy PNG loading, texture dumps.
#include "vsport/texture_replace.hpp"

#include <cctype>
#include <cstdio>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#else
#include <cstring>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"
#endif

namespace vsport {

u64 hash_vram_rect(const u16* vram, u32 x, u32 y, u32 w, u32 h) noexcept {
    u64 hsh = 0xCBF29CE484222325ull;
    for (u32 row = 0; row < h; ++row) {
        const u16* p = vram + static_cast<std::size_t>(y + row) * 1024u + x;
        for (u32 i = 0; i < w; ++i) {
            hsh = (hsh ^ (p[i] & 0xFFu)) * 0x100000001B3ull;
            hsh = (hsh ^ (p[i] >> 8)) * 0x100000001B3ull;
        }
    }
    return hsh;
}

#ifdef _WIN32
namespace {
using Microsoft::WRL::ComPtr;

IWICImagingFactory* wic_factory() {
    // COM per thread (the emulation and window threads may both use it); any mode is fine for WIC
    thread_local const bool com = [] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }();
    (void)com;
    thread_local ComPtr<IWICImagingFactory> f;
    if (!f) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f.Get();
}
} // namespace

bool load_png(const std::filesystem::path& path, TexImage& out) {
    IWICImagingFactory* f = wic_factory();
    if (!f) return false;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapSource> conv;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame.Get(), &conv))) {
        return false;
    }
    UINT w = 0, h = 0;
    if (FAILED(conv->GetSize(&w, &h)) || w == 0 || h == 0 || w > 16384 || h > 16384) return false;
    out.w = w;
    out.h = h;
    out.rgba.assign(static_cast<std::size_t>(w) * h, 0u);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4u, w * h * 4u, reinterpret_cast<BYTE*>(out.rgba.data())));
}

bool save_png(const std::filesystem::path& path, const TexImage& img) {
    IWICImagingFactory* f = wic_factory();
    if (!f || img.w == 0 || img.h == 0) return false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(f->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&frame, &props)) ||
        FAILED(frame->Initialize(props.Get())) || FAILED(frame->SetSize(img.w, img.h))) {
        return false;
    }
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;
    std::vector<u32> px = img.rgba;
    if (fmt == GUID_WICPixelFormat32bppBGRA) {
        for (u32& c : px) c = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
    } else if (fmt != GUID_WICPixelFormat32bppRGBA) {
        return false;
    }
    return SUCCEEDED(frame->WritePixels(img.h, img.w * 4u, img.w * img.h * 4u, reinterpret_cast<BYTE*>(px.data()))) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}
#else
// Linux / Android: stb_image (public domain, third_party/stb); RGBA byte order matches TexImage on little endian
bool load_png(const std::filesystem::path& path, TexImage& out) {
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load(path.string().c_str(), &w, &h, &n, 4);
    if (!px) return false;
    const bool ok = w > 0 && h > 0 && w <= 16384 && h <= 16384;
    if (ok) {
        out.w = static_cast<u32>(w);
        out.h = static_cast<u32>(h);
        out.rgba.resize(static_cast<std::size_t>(w) * h);
        std::memcpy(out.rgba.data(), px, out.rgba.size() * 4u);
    }
    stbi_image_free(px);
    return ok;
}

bool save_png(const std::filesystem::path& path, const TexImage& img) {
    if (img.w == 0 || img.h == 0) return false;
    return stbi_write_png(path.string().c_str(), static_cast<int>(img.w), static_cast<int>(img.h), 4, img.rgba.data(),
                          static_cast<int>(img.w * 4u)) != 0;
}
#endif

namespace {
bool parse_hex16(const std::string& s, std::size_t at, u64& out) {
    out = 0;
    for (std::size_t i = at; i < at + 16; ++i) {
        const char c = s[i];
        u32 d;
        if (c >= '0' && c <= '9') d = static_cast<u32>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<u32>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<u32>(c - 'A' + 10);
        else return false;
        out = (out << 4) | d;
    }
    return true;
}
// "<16 hex>-<16 hex>-<0..2>" at the start of a file name
bool parse_key_name(const std::string& stem, u64& up, u64& pal, u32& depth) {
    if (stem.size() < 35 || stem[16] != '-' || stem[33] != '-' || stem[34] < '0' || stem[34] > '2') return false;
    if (stem.size() > 35 && stem[35] != '-' && stem[35] != '_' && stem[35] != ' ') return false;
    depth = static_cast<u32>(stem[34] - '0');
    return parse_hex16(stem, 0, up) && parse_hex16(stem, 17, pal);
}
} // namespace

std::string TextureReplacer::key_name(u64 up_hash, u64 pal_hash, u32 depth) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%016llx-%016llx-%u", static_cast<unsigned long long>(up_hash),
                  static_cast<unsigned long long>(pal_hash), depth);
    return buf;
}

std::size_t TextureReplacer::scan(const std::filesystem::path& dir) {
    index_.clear();
    loaded_.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return 0;
    auto it = std::filesystem::recursive_directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec);
    for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        const std::filesystem::path& p = it->path();
        if (it->is_directory(ec)) {
            // Folders starting with "_" (e.g. the dump folder) are not part of the pack
            if (p.filename().u8string().rfind(u8"_", 0) == 0) it.disable_recursion_pending();
            continue;
        }
        std::string ext = p.extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".png") continue;
        u64 up = 0, pal = 0;
        u32 depth = 0;
        if (parse_key_name(p.stem().string(), up, pal, depth)) index_[key(up, pal, depth)] = p;
    }
    return index_.size();
}

void TextureReplacer::set_dump_dir(const std::filesystem::path& dir) {
    dump_dir_ = dir;
    seen_.clear();
    if (dir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // Textures dumped in an earlier session are not written again
    for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        u64 up = 0, pal = 0;
        u32 depth = 0;
        if (parse_key_name(it->path().stem().string(), up, pal, depth)) seen_.insert(key(up, pal, depth));
    }
}

const TexImage* TextureReplacer::lookup(const TexUpload& up, u32 depth, u64 pal_hash, const u16* vram, u16 clut) {
    ++stats.lookups;
    const u64 k = key(up.hash, pal_hash, depth);
    if (!index_.empty()) {
        const auto it = loaded_.find(k);
        if (it != loaded_.end()) {
            if (it->second) {
                ++stats.hits;
                return it->second.get();
            }
        } else {
            const auto ix = index_.find(k);
            if (ix != index_.end()) {
                auto img = std::make_unique<TexImage>();
                if (load_png(ix->second, *img)) {
                    ++stats.hits;
                    return (loaded_[k] = std::move(img)).get();
                }
                loaded_[k] = nullptr;
                ++stats.load_failed;
            }
        }
    }
    if (dumping() && seen_.insert(k).second && index_.find(k) == index_.end()) dump(up, depth, pal_hash, vram, clut);
    return nullptr;
}

void TextureReplacer::dump(const TexUpload& up, u32 depth, u64 pal_hash, const u16* vram, u16 clut) {
    const u32 per_word = depth == 0 ? 4u : depth == 1 ? 2u : 1u;
    const u32 bits = 16u / per_word;
    const u32 clut_x = (clut & 0x3Fu) * 16u, clut_y = (clut >> 6) & 0x1FFu;
    TexImage img;
    img.w = up.w * per_word;
    img.h = up.h;
    img.rgba.resize(static_cast<std::size_t>(img.w) * img.h);
    const auto c5to8 = [](u32 c) { return (c * 527u + 23u) >> 6; };
    for (u32 ty = 0; ty < img.h; ++ty) {
        for (u32 tx = 0; tx < img.w; ++tx) {
            const u16 word = vram[static_cast<std::size_t>(up.y + ty) * 1024u + up.x + tx / per_word];
            u16 c = word;
            if (depth < 2u) {
                const u32 idx = (word >> ((tx % per_word) * bits)) & ((1u << bits) - 1u);
                c = vram[static_cast<std::size_t>(clut_y) * 1024u + ((clut_x + idx) & 1023u)];
            }
            img.rgba[static_cast<std::size_t>(ty) * img.w + tx] =
                c == 0 ? 0u : (c5to8(c & 31u) | (c5to8((c >> 5) & 31u) << 8) | (c5to8((c >> 10) & 31u) << 16) | 0xFF000000u);
        }
    }
    if (save_png(dump_dir_ / (key_name(up.hash, pal_hash, depth) + ".png"), img)) ++stats.dumped;
}

} // namespace vsport
