#pragma once

#include "vsport/types.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vsport {

enum class DiscSectorFormat {
    Cooked2048,    // Standard .iso (2048 bytes/sector)
    Raw2352Mode1,  // Raw CD Mode 1 (2352 bytes/sector, user data at +16)
    Raw2352Mode2XA // PS1 CD-ROM XA Mode 2 (2352 bytes/sector, Form1 user data at +24)
};

struct IsoFileEntry {
    std::string path;       // Full ISO path, e.g. "SLUS_010.40" or "MAP/MAP001.MPD"
    u32 lba        = 0;     // Starting Logical Block Address
    u32 size_bytes = 0;     // File length in bytes
    bool is_dir    = false;
    bool is_xa_str = false; // Mode 2 Form 2 streaming sector (STR/XA audio)
};

struct SystemCnfInfo {
    std::string boot_raw;   // e.g. "cdrom:\\SLUS_010.40;1"
    std::string boot_path;  // Normalized ISO path, e.g. "SLUS_010.40"
    u32 tcb   = 4;
    u32 event = 10;
    u32 stack = 0x801FFF00u;
};

class DiscImage {
public:
    DiscImage() = default;

    // Opens a .cue, .bin, or .iso file and parses ISO9660 filesystem
    bool open(const std::filesystem::path& path, std::string* err_out = nullptr);

    [[nodiscard]] bool is_open() const noexcept { return stream_.is_open(); }
    [[nodiscard]] DiscSectorFormat sector_format() const noexcept { return format_; }
    [[nodiscard]] u32 total_sectors() const noexcept { return total_sectors_; }
    [[nodiscard]] const std::string& volume_id() const noexcept { return volume_id_; }
    [[nodiscard]] const std::filesystem::path& binary_path() const noexcept { return bin_path_; }
    [[nodiscard]] const std::vector<IsoFileEntry>& files() const noexcept { return entries_; }

    // Reads a single 2048-byte Mode1 / Mode2-Form1 sector by LBA
    bool read_sector_2048(u32 lba, u8* out_buf) const;

    // Reads a raw 2352-byte sector (or synthesizes if source is 2048-byte ISO)
    bool read_sector_raw2352(u32 lba, u8* out_buf) const;

    // Finds a file entry by case-insensitive ISO path (e.g. "BATTLE.PRG" or "/MENU/MAINMENU.PRG")
    [[nodiscard]] std::optional<IsoFileEntry> find_file(std::string_view iso_path) const;

    // Extracts full file contents into memory
    [[nodiscard]] std::vector<u8> extract_file(std::string_view iso_path) const;
    [[nodiscard]] std::vector<u8> extract_entry(const IsoFileEntry& entry) const;

    // Parses SYSTEM.CNF from the disc
    [[nodiscard]] std::optional<SystemCnfInfo> parse_system_cnf() const;

    // Subchannel Q data. A .bin image stores only sector data; protections such as LibCrypt
    // (Vagrant Story PAL) rely on deliberately altered Q data on the original disc. A Redump
    // .sbi file records those sectors. open() loads "<image name>.sbi" automatically when it
    // sits next to the .cue/.bin; load_sbi() loads one from anywhere.
    bool load_sbi(const std::filesystem::path& path, std::string* err_out = nullptr);
    [[nodiscard]] std::size_t subq_override_count() const noexcept { return subq_overrides_.size(); }
    // True for sectors listed in the .sbi: their Q data was altered on the disc and fails the
    // Q CRC, so a real drive ignores it (see PsxCdrom GetlocP)
    [[nodiscard]] bool has_subq_override(u32 lba) const noexcept { return subq_overrides_.contains(lba); }
    [[nodiscard]] const std::filesystem::path& sbi_path() const noexcept { return sbi_path_; }

    // 10-byte Q subchannel for a sector (BCD, no CRC): ctrl/adr, track, index, relative MSF,
    // zero, absolute MSF. Uses the .sbi data when present, else the regular track-1 position.
    [[nodiscard]] std::array<u8, 10> subchannel_q(u32 lba) const;

private:
    bool detect_geometry(std::string* err_out);
    bool parse_iso9660(std::string* err_out);
    void walk_directory(u32 dir_lba, u32 dir_size, const std::string& prefix, int depth);

    static std::filesystem::path resolve_cue_to_bin(const std::filesystem::path& cue_path, std::string* err_out);

    mutable std::ifstream stream_;
    std::filesystem::path bin_path_;
    DiscSectorFormat format_ = DiscSectorFormat::Cooked2048;
    u64 file_size_           = 0;
    u32 total_sectors_       = 0;
    u32 track_pregap_offset_ = 0; // Byte offset to LBA 0 in track file
    std::string volume_id_;
    std::vector<IsoFileEntry> entries_;
    std::unordered_map<u32, std::array<u8, 10>> subq_overrides_; // LBA -> Q from .sbi
    std::filesystem::path sbi_path_;
};

} // namespace vsport
