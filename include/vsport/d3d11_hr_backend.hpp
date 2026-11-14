#pragma once

#include "vsport/hr_backend.hpp"

#include <memory>
#include <string>

namespace vsport {

// Draws the upscaled picture on the graphics card with Direct3D 11.
// device: an ID3D11Device* to share (the window renderer's, so it can present the picture
// directly); null = create an own device (headless runs: hardware, falling back to WARP).
// Returns null (and sets err) if Direct3D 11 is not available.
[[nodiscard]] std::unique_ptr<HrBackend> make_d3d11_hr_backend(void* device, std::string* err = nullptr);

} // namespace vsport
