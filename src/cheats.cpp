#include "vsport/cheats.hpp"

#include "vsport/runtime.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace vsport {

namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool is_hex(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool supported(u8 type) {
    return type == 0x80 || type == 0x30 || (type >= 0xD0 && type <= 0xD3) || (type >= 0xE0 && type <= 0xE3);
}

} // namespace

void CheatList::parse(const std::string& text, std::vector<std::string>* problems) {
    cheats_.clear();
    std::istringstream in(text);
    std::string raw;
    int line_no = 0;
    const auto problem = [&](const std::string& what) {
        if (problems) problems->push_back("satir " + std::to_string(line_no) + ": " + what);
    };
    while (std::getline(in, raw)) {
        ++line_no;
        if (line_no == 1 && raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF) raw.erase(0, 3); // UTF-8 BOM
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            const std::size_t close = line.find(']');
            if (close == std::string::npos) {
                problem("']' eksik");
                continue;
            }
            Cheat c;
            c.name = trim(line.substr(1, close - 1));
            std::string rest = trim(line.substr(close + 1));
            for (char& ch : rest) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            c.enabled = !(rest == "off" || rest == "kapali");
            cheats_.push_back(std::move(c));
            continue;
        }
        // Code line: 8 hex digits, 4 hex digits (a space or '-' between them is optional)
        std::string hex;
        for (char ch : line) {
            if (!std::isspace(static_cast<unsigned char>(ch)) && ch != '-') hex += ch;
        }
        if (hex.size() != 12 || !is_hex(hex)) {
            problem("kod anlasilamadi: " + line);
            continue;
        }
        Code code;
        code.type  = static_cast<u8>(std::strtoul(hex.substr(0, 2).c_str(), nullptr, 16));
        code.addr  = static_cast<u32>(std::strtoul(hex.substr(2, 6).c_str(), nullptr, 16)) & 0x1FFFFFu;
        code.value = static_cast<u16>(std::strtoul(hex.substr(8, 4).c_str(), nullptr, 16));
        if (!supported(code.type)) {
            problem("desteklenmeyen kod turu: " + hex.substr(0, 2));
            continue;
        }
        if (cheats_.empty()) {
            Cheat c;
            c.name = "(adsiz)";
            cheats_.push_back(std::move(c));
        }
        cheats_.back().codes.push_back(code);
    }
}

std::size_t CheatList::enabled_count() const noexcept {
    std::size_t n = 0;
    for (const Cheat& c : cheats_) {
        if (c.enabled && !c.codes.empty()) ++n;
    }
    return n;
}

void CheatList::apply(PsxRuntime& rt) const {
    for (const Cheat& cheat : cheats_) {
        if (!cheat.enabled) continue;
        bool skip_next = false;
        for (const Code& c : cheat.codes) {
            if (skip_next) {
                skip_next = false;
                continue;
            }
            const u32 vaddr = 0x80000000u | c.addr;
            if (c.type == 0x80) {
                if (rt.read16(vaddr) != c.value) rt.write16(vaddr, c.value);
            } else if (c.type == 0x30) {
                if (rt.read8(vaddr) != static_cast<u8>(c.value)) rt.write8(vaddr, static_cast<u8>(c.value));
            } else {
                // Conditions: the next code line runs only when the comparison holds
                const bool wide = c.type >= 0xD0 && c.type <= 0xD3;
                const u32 cur = wide ? rt.read16(vaddr) : rt.read8(vaddr);
                const u32 ref = wide ? c.value : (c.value & 0xFFu);
                bool hold = false;
                switch (c.type & 3u) {
                    case 0: hold = cur == ref; break;
                    case 1: hold = cur != ref; break;
                    case 2: hold = cur < ref; break;
                    default: hold = cur > ref; break;
                }
                skip_next = !hold;
            }
        }
    }
}

} // namespace vsport
