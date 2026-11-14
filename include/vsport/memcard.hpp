#pragma once

#include "vsport/types.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace vsport {

// PlayStation memory card: 128 KB = 16 blocks x 64 frames ("sectors") of 128 bytes.
// Stored as a raw .mcr image (the format DuckStation, ePSXe, Mednafen etc. read and write), so
// saves can be moved between emulators.
class MemoryCard {
public:
    static constexpr u32 SECTOR_SIZE  = 128;
    static constexpr u32 SECTOR_COUNT = 1024;
    static constexpr u32 CARD_SIZE    = SECTOR_SIZE * SECTOR_COUNT;

    MemoryCard();

    // Opens the card image; a missing file is created as a freshly formatted, empty card.
    bool open(const std::filesystem::path& path, std::string* err = nullptr);
    // Writes pending changes to disk (no-op when nothing changed).
    bool flush(std::string* err = nullptr);
    void close();

    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    // Sector access (sector 0..1023). Return false for an out-of-range sector or no card.
    bool read_sector(u32 sector, u8* out) const;
    bool write_sector(u32 sector, const u8* in);

    // Erases the card to the empty, formatted state (directory + broken-sector list).
    void format();

    // Number of used 8 KB save blocks (1..15) according to the directory
    [[nodiscard]] u32 used_blocks() const noexcept;

    // --- File system (what the BIOS "bu" device does on top of the directory) ---
    static constexpr u32 BLOCK_SIZE  = 8192;
    static constexpr u32 BLOCK_COUNT = 15;  // Save blocks 1..15 (block 0 holds the directory)

    struct FileInfo {
        std::string name;
        u32 first_block = 0; // 1..15
        u32 size        = 0; // Bytes (multiple of 8192)
    };

    // First file at or after directory block `from_block` whose name matches `pattern`
    // ('?' = any one character, '*' = any rest). Returns false when there is none.
    bool find_file(const std::string& pattern, u32 from_block, FileInfo& out) const;
    bool create_file(const std::string& name, u32 blocks, FileInfo& out);
    bool erase_file(const std::string& name);
    bool rename_file(const std::string& old_name, const std::string& new_name);
    // Byte access inside a file, following its block chain. Returns bytes transferred.
    u32 read_file(const FileInfo& file, u32 offset, u8* out, u32 length) const;
    u32 write_file(const FileInfo& file, u32 offset, const u8* in, u32 length);

    static bool name_matches(const std::string& pattern, const std::string& name) noexcept;

    [[nodiscard]] const std::vector<u8>& data() const noexcept { return data_; }

private:
    std::vector<u8> data_;
    std::filesystem::path path_;
    bool open_  = false;
    bool dirty_ = false;
};

} // namespace vsport
