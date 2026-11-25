#include "vsport/cdrom_iso.hpp"
#include "vsport/cfg_lifter.hpp"
#include "vsport/psx_exe.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

using namespace vsport;

namespace {

std::string sanitize_identifier(std::string_view name) {
    std::string out;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else {
            out.push_back('_');
        }
    }
    return out;
}

bool ends_with_ci(std::string_view str, std::string_view suffix) {
    if (suffix.size() > str.size()) return false;
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        const char a = static_cast<char>(std::toupper(static_cast<unsigned char>(str[str.size() - suffix.size() + i])));
        const char b = static_cast<char>(std::toupper(static_cast<unsigned char>(suffix[i])));
        if (a != b) return false;
    }
    return true;
}

bool write_text_file(const std::filesystem::path& path, const std::string& content) {
    if (std::filesystem::exists(path)) {
        std::ifstream existing(path, std::ios::binary | std::ios::ate);
        if (existing.is_open()) {
            const auto sz = static_cast<std::size_t>(existing.tellg());
            if (sz == content.size()) {
                std::string old_buf(sz, '\0');
                existing.seekg(0);
                existing.read(old_buf.data(), static_cast<std::streamsize>(sz));
                if (existing.good() && old_buf == content) {
                    return true; // Preserve timestamp for unchanged generated files
                }
            }
        }
    }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path disc_path;
    std::filesystem::path input_bin;
    std::filesystem::path out_dir = "generated_recomp";
    u32 manual_base = 0;
    u32 max_funcs   = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--disc" || arg == "-d") && i + 1 < argc) {
            disc_path = argv[++i];
        } else if ((arg == "--input" || arg == "-i") && i + 1 < argc) {
            input_bin = argv[++i];
        } else if ((arg == "--out-dir" || arg == "-o") && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (arg == "--base" && i + 1 < argc) {
            manual_base = static_cast<u32>(std::strtoul(argv[++i], nullptr, 0));
        } else if (arg == "--max-funcs" && i + 1 < argc) {
            max_funcs = static_cast<u32>(std::strtoul(argv[++i], nullptr, 0));
        }
    }

    if (disc_path.empty() && input_bin.empty()) {
        std::cout << "Usage:\n"
                  << "  1) Generate C++ for the whole disc image:\n"
                  << "     vs_recomp --disc <VagrantStory.cue|.bin|.iso> [--out-dir generated_recomp] [--max-funcs N]\n"
                  << "  2) Generate C++ from one PS-X EXE or .PRG file:\n"
                  << "     vs_recomp --input <SLUS_010.40|BATTLE.PRG> [--base 0x80068800] [--out-dir generated_recomp]\n";
        return 1;
    }

    if (!input_bin.empty()) {
        std::ifstream f(input_bin, std::ios::binary | std::ios::ate);
        if (!f.is_open()) {
            std::cerr << "[ERROR] Could not open the input file: " << input_bin.string() << "\n";
            return 1;
        }
        const auto sz = static_cast<std::size_t>(f.tellg());
        std::vector<u8> raw(sz);
        f.seekg(0);
        f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(sz));

        PsxExeImage exe{};
        const std::string stem = sanitize_identifier(input_bin.filename().string());
        LifterOptions opts{stem, true, max_funcs};

        if (parse_psx_exe(raw, exe)) {
            const auto funcs = discover_functions(exe.header.t_addr, exe.payload, exe.header.pc0);
            const std::string cpp = emit_recompiled_module_cpp(funcs, exe.header.t_addr, exe.payload, opts);
            const auto out_file = out_dir / (stem + ".cpp");
            write_text_file(out_file, cpp);
            std::printf("[OK] PS-X EXE translated -> %s (%zu functions)\n", out_file.string().c_str(), funcs.size());
            return 0;
        }

        const PrgAnalysis prg = analyze_vs_prg(input_bin.filename().string(), raw);
        const u32 base = (manual_base != 0) ? manual_base : (prg.inferred_base_addr != 0 ? prg.inferred_base_addr : 0x80068800u);

        const auto funcs = discover_functions(base, raw);
        const std::string cpp = emit_recompiled_module_cpp(funcs, base, raw, opts);
        const auto out_file = out_dir / (stem + ".cpp");
        write_text_file(out_file, cpp);
        std::printf("[OK] PRG overlay translated -> %s (RAM: 0x%08X, %zu functions)\n",
                    out_file.string().c_str(), base, funcs.size());
        return 0;
    }

    // Full disc extraction & recompilation mode
    DiscImage disc;
    std::string err;
    if (!disc.open(disc_path, &err)) {
        std::cerr << "[ERROR] Could not open the disc: " << err << "\n";
        return 1;
    }

    const auto cnf = disc.parse_system_cnf();
    if (!cnf.has_value()) {
        std::cerr << "[ERROR] SYSTEM.CNF not found.\n";
        return 1;
    }

    // 1. Recompile Resident Boot Executable (SLUS_010.40 / SLES_027.54 / SLPS_023.77)
    const auto boot_raw = disc.extract_file(cnf->boot_path);
    PsxExeImage boot_exe{};
    if (!parse_psx_exe(boot_raw, boot_exe, &err)) {
        std::cerr << "[ERROR] Could not parse the boot EXE: " << err << "\n";
        return 1;
    }

    std::vector<std::string> generated_mods;
    std::set<std::string> seen_mod_ids;
    std::size_t total_lifted_funcs = 0;

    {
        const std::string mod_id = sanitize_identifier(cnf->boot_path);
        LifterOptions opts{mod_id, true, max_funcs};
        const auto funcs = discover_functions(boot_exe.header.t_addr, boot_exe.payload, boot_exe.header.pc0);
        const std::string cpp = emit_recompiled_module_cpp(funcs, boot_exe.header.t_addr, boot_exe.payload, opts);
        const auto out_file = out_dir / (mod_id + ".cpp");
        write_text_file(out_file, cpp);
        generated_mods.push_back(mod_id);
        seen_mod_ids.insert(mod_id);
        total_lifted_funcs += funcs.size();
        std::printf("[OK] Core (%s) -> %s (%zu functions)\n",
                    cnf->boot_path.c_str(), out_file.string().c_str(), funcs.size());
    }

    // 2. Discover & Recompile ALL .PRG Overlays Across the Entire Disc
    //    (BATTLE.PRG, INITBTL.PRG, TITLE.PRG, ENDING.PRG, SCREFF2.PRG, MAINMENU.PRG, MENU0..MENUF.PRG)
    std::vector<IsoFileEntry> prg_entries;
    for (const auto& e : disc.files()) {
        if (!e.is_dir && ends_with_ci(e.path, ".PRG")) {
            prg_entries.push_back(e);
        }
    }
    std::sort(prg_entries.begin(), prg_entries.end(),
              [](const IsoFileEntry& a, const IsoFileEntry& b) { return a.path < b.path; });

    for (const auto& entry : prg_entries) {
        const auto bytes = disc.extract_entry(entry);
        if (bytes.size() < 4u) {
            std::printf("[SKIPPED] Empty PRG placeholder (%s, %zu B)\n", entry.path.c_str(), bytes.size());
            continue;
        }

        const PrgAnalysis prg = analyze_vs_prg(entry.path, bytes);
        if (prg.inferred_base_addr == 0) {
            std::printf("[WARNING] Base KSEG0 address not resolved, skipped: %s\n", entry.path.c_str());
            continue;
        }

        std::string mod_id = sanitize_identifier(prg.name);
        if (seen_mod_ids.contains(mod_id)) {
            mod_id = sanitize_identifier(entry.path);
        }

        LifterOptions opts{mod_id, true, max_funcs};
        const auto funcs = discover_functions(prg.inferred_base_addr, bytes);
        if (funcs.empty()) {
            std::printf("[SKIPPED] No functions found (%s)\n", entry.path.c_str());
            continue;
        }

        const std::string cpp = emit_recompiled_module_cpp(funcs, prg.inferred_base_addr, bytes, opts);
        const auto out_file = out_dir / (mod_id + ".cpp");
        write_text_file(out_file, cpp);
        generated_mods.push_back(mod_id);
        seen_mod_ids.insert(mod_id);
        total_lifted_funcs += funcs.size();
        std::printf("[OK] Overlay (%-18s) -> %-28s (RAM: 0x%08X, %4zu functions)\n",
                    entry.path.c_str(), out_file.string().c_str(), prg.inferred_base_addr, funcs.size());
    }

    // 3. Emit unified AOT module registry so vs_host links & registers all lifted modules automatically
    {
        std::string hdr = "#pragma once\n#include \"vsport/runtime.hpp\"\n\nnamespace vsport {\n"
                          "std::size_t register_all_generated_modules(PsxRuntime& rt);\n"
                          "} // namespace vsport\n";
        write_text_file(out_dir / "vs_aot_registry.hpp", hdr);

        std::string reg_cpp = "// Auto-generated AOT Registry by vs_recomp\n"
                              "#include \"vs_aot_registry.hpp\"\n\n";
        for (const auto& m : generated_mods) {
            reg_cpp += "namespace vsport::generated_" + m + " { void register_all(PsxRuntime& rt); }\n";
        }
        reg_cpp += "\nnamespace vsport {\nstd::size_t register_all_generated_modules(PsxRuntime& rt) {\n";
        for (const auto& m : generated_mods) {
            reg_cpp += "    generated_" + m + "::register_all(rt);\n";
        }
        reg_cpp += "    return " + std::to_string(total_lifted_funcs) + "u;\n}\n} // namespace vsport\n";
        write_text_file(out_dir / "vs_aot_registry.cpp", reg_cpp);
        std::printf("[OK] AOT registry -> %s (%zu modules, %zu C++ functions in all)\n",
                    (out_dir / "vs_aot_registry.cpp").string().c_str(), generated_mods.size(), total_lifted_funcs);
    }

    return 0;
}
