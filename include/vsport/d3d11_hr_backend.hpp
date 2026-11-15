#pragma once

#include "vsport/hr_backend.hpp"

#include <memory>
#include <string>
#include <vector>

namespace vsport {

// Draws the upscaled picture on the graphics card with Direct3D 11.
// device: an ID3D11Device* to share (the window renderer's, so it can present the picture
// directly); null = create an own device (headless runs: hardware, falling back to WARP).
// Returns null (and sets err) if Direct3D 11 is not available.
[[nodiscard]] std::unique_ptr<HrBackend> make_d3d11_hr_backend(void* device, std::string* err = nullptr);

// Graphics cards (hardware only, UTF-8 names), most powerful first
[[nodiscard]] std::vector<std::string> list_graphics_adapters();

// Graphics card chosen in the settings (by name); empty = automatic (the most powerful one)
void set_preferred_adapter(const std::string& name);

// The chosen graphics card, else the most powerful one (IDXGIAdapter*, opaque; the caller
// Releases it). On laptops with two cards Windows would otherwise give the integrated one.
// Null if it cannot be determined (then Direct3D's default adapter is used).
[[nodiscard]] void* acquire_high_performance_adapter();

} // namespace vsport
