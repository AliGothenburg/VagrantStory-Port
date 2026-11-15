#pragma once

// GameShark / Action Replay style cheat codes ("AABBBBBB CCCC"), applied once per frame.
//
// File format (text):
//   # comment
//   [Name of the cheat]          <- starts a cheat (on)
//   [Name of the cheat] off      <- starts a cheat that is switched off
//   80012345 03E7                <- its code lines
//
// Supported code types (first two hex digits):
//   80 write 16 bit   30 write 8 bit
//   D0 / D1 / D2 / D3: run the next line only if the 16-bit value is == / != / < / > CCCC
//   E0 / E1 / E2 / E3: the same for 8-bit values

#include "vsport/types.hpp"

#include <string>
#include <vector>

namespace vsport {

class PsxRuntime;

class CheatList {
public:
    struct Code {
        u8 type = 0;
        u32 addr = 0; // RAM offset (21 bits)
        u16 value = 0;
    };
    struct Cheat {
        std::string name;
        bool enabled = true;
        std::vector<Code> codes;
    };

    // Parses the text; lines that are not understood are listed in `problems` (if given)
    void parse(const std::string& text, std::vector<std::string>* problems = nullptr);
    // Applies every enabled cheat to the machine's memory
    void apply(PsxRuntime& rt) const;

    [[nodiscard]] const std::vector<Cheat>& cheats() const noexcept { return cheats_; }
    [[nodiscard]] std::size_t enabled_count() const noexcept;

private:
    std::vector<Cheat> cheats_;
};

} // namespace vsport
