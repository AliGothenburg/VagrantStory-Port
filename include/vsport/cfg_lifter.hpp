#pragma once

#include "vsport/types.hpp"

#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vsport {

struct DiscoveredFunction {
    u32 start_addr = 0; // KSEG0 start address (e.g. 0x80010000)
    u32 end_addr   = 0; // Exclusive KSEG0 end address
    std::set<u32> local_labels;
};

struct LifterOptions {
    std::string module_id = "slus_010_40";
    bool emit_comments    = true;
    u32 max_functions     = 0; // 0 = unlimited
};

// Discovers functions and local branch labels inside a PSX code segment
[[nodiscard]] std::vector<DiscoveredFunction> discover_functions(
    u32 base_kseg0,
    std::span<const u8> code_bytes,
    u32 entry_pc = 0);

// Translates a single MIPS function into compilable C++20 code (handling branch delay slots & GTE)
[[nodiscard]] std::string lift_function_to_cpp(
    const DiscoveredFunction& fn,
    u32 base_kseg0,
    std::span<const u8> code_bytes,
    bool emit_comments = true);

// Emits a complete C++ translation unit (.cpp) + registration function for a module
[[nodiscard]] std::string emit_recompiled_module_cpp(
    std::span<const DiscoveredFunction> funcs,
    u32 base_kseg0,
    std::span<const u8> code_bytes,
    const LifterOptions& opts = {});

} // namespace vsport
