#include "vsport/memcard.hpp"
#include "vsport/i18n.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace vsport {

namespace {

// Frame checksum: XOR of bytes 0..126 stored in byte 127
void set_frame_checksum(u8* frame) noexcept {
    u8 x = 0;
    for (u32 i = 0; i < 127; ++i) x ^= frame[i];
    frame[127] = x;
}

} // namespace

MemoryCard::MemoryCard() : data_(CARD_SIZE, 0) {}

void MemoryCard::format() {
    std::fill(data_.begin(), data_.end(), static_cast<u8>(0));
    u8* card = data_.data();

    // Frame 0: header "MC"
    card[0] = 'M';
    card[1] = 'C';
    set_frame_checksum(card);

    // Frames 1..15: directory, all blocks free (0xA0), no next block (0xFFFF)
    for (u32 f = 1; f <= 15; ++f) {
        u8* fr = card + f * SECTOR_SIZE;
        fr[0] = 0xA0;
        fr[8] = 0xFF;
        fr[9] = 0xFF;
        set_frame_checksum(fr);
    }
    // Frames 16..35: broken sector list, all unused (sector 0xFFFFFFFF)
    for (u32 f = 16; f <= 35; ++f) {
        u8* fr = card + f * SECTOR_SIZE;
        fr[0] = fr[1] = fr[2] = fr[3] = 0xFF;
        fr[8] = 0xFF;
        fr[9] = 0xFF;
        set_frame_checksum(fr);
    }
    // Frame 63: write-test frame, a copy of the header
    std::memcpy(card + 63 * SECTOR_SIZE, card, SECTOR_SIZE);
    dirty_ = true;
}

bool MemoryCard::open(const std::filesystem::path& path, std::string* err) {
    close();
    path_ = path;
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        std::ifstream in(path, std::ios::binary);
        std::vector<u8> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // Accept raw 128 KB images, and DexDrive-style images with a 3904-byte header
        if (buf.size() == CARD_SIZE) {
            data_ = std::move(buf);
        } else if (buf.size() == CARD_SIZE + 3904u) {
            data_.assign(buf.begin() + 3904, buf.end());
        } else {
            if (err) *err = txt("The memory card file is not 128 KB: ", "Hafıza kartı dosyası 128 KB değil: ") + path.string();
            return false;
        }
        dirty_ = false;
    } else {
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), ec);
        }
        format();
        if (!flush(err)) {
            return false;
        }
    }
    open_ = true;
    return true;
}

bool MemoryCard::flush(std::string* err) {
    if (!dirty_ || path_.empty()) return true;
    // Write to a temporary file first so a crash mid-write never destroys the saves
    auto tmp = path_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data_.data()), static_cast<std::streamsize>(data_.size()));
        if (!out.good()) {
            if (err) *err = txt("Could not write the memory card: ", "Hafıza kartı yazılamadı: ") + tmp.string();
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path_, ec);
    if (ec) {
        if (err) *err = txt("Could not save the memory card: ", "Hafıza kartı kaydedilemedi: ") + path_.string() + " (" + ec.message() + ")";
        return false;
    }
    dirty_ = false;
    return true;
}

void MemoryCard::close() {
    if (open_) flush();
    open_ = false;
}

bool MemoryCard::read_sector(u32 sector, u8* out) const {
    if (!open_ || sector >= SECTOR_COUNT) return false;
    std::memcpy(out, data_.data() + sector * SECTOR_SIZE, SECTOR_SIZE);
    return true;
}

bool MemoryCard::write_sector(u32 sector, const u8* in) {
    if (!open_ || sector >= SECTOR_COUNT) return false;
    u8* dst = data_.data() + sector * SECTOR_SIZE;
    if (std::memcmp(dst, in, SECTOR_SIZE) != 0) {
        std::memcpy(dst, in, SECTOR_SIZE);
        dirty_ = true;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// File system. Directory frame n (1..15) describes save block n:
//   +00 u32 state: 0x51 first block of a file, 0x52 middle, 0x53 last, 0xA0 free, 0xA1..A3 deleted
//   +04 u32 file size in bytes (first block only)
//   +08 u16 next block of the file (0..14 = block 1..15), 0xFFFF = end
//   +0A     file name, ASCII, up to 20 characters, 0-terminated (first block only)
//   +7F     XOR checksum of bytes 0..126
// ---------------------------------------------------------------------------------------------

namespace {

constexpr u8 STATE_FIRST = 0x51, STATE_MIDDLE = 0x52, STATE_LAST = 0x53, STATE_FREE = 0xA0;

u32 rd32(const u8* p) noexcept { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24); }
void wr32(u8* p, u32 v) noexcept { p[0] = u8(v); p[1] = u8(v >> 8); p[2] = u8(v >> 16); p[3] = u8(v >> 24); }

std::string frame_name(const u8* frame) {
    std::string n;
    for (u32 i = 0; i < 21 && frame[0x0A + i] != 0; ++i) n.push_back(static_cast<char>(frame[0x0A + i]));
    return n;
}

} // namespace

bool MemoryCard::name_matches(const std::string& pattern, const std::string& name) noexcept {
    std::size_t p = 0, n = 0;
    while (p < pattern.size()) {
        if (pattern[p] == '*') return true; // BIOS wildcard: matches the rest
        if (n >= name.size()) return false;
        if (pattern[p] != '?' && pattern[p] != name[n]) return false;
        ++p;
        ++n;
    }
    return n == name.size();
}

bool MemoryCard::find_file(const std::string& pattern, u32 from_block, FileInfo& out) const {
    if (!open_) return false;
    for (u32 b = std::max<u32>(1u, from_block); b <= BLOCK_COUNT; ++b) {
        const u8* fr = data_.data() + b * SECTOR_SIZE;
        if (fr[0] != STATE_FIRST) continue;
        const std::string name = frame_name(fr);
        if (!name_matches(pattern, name)) continue;
        out.name        = name;
        out.first_block = b;
        out.size        = rd32(fr + 4);
        return true;
    }
    return false;
}

bool MemoryCard::create_file(const std::string& name, u32 blocks, FileInfo& out) {
    if (!open_ || name.empty() || name.size() > 20 || blocks == 0 || blocks > BLOCK_COUNT) return false;
    FileInfo existing;
    if (find_file(name, 1, existing) && existing.name == name) return false; // Already exists
    std::vector<u32> free_blocks;
    for (u32 b = 1; b <= BLOCK_COUNT && free_blocks.size() < blocks; ++b) {
        const u8 st = data_[b * SECTOR_SIZE];
        if (st == STATE_FREE || (st >= 0xA1 && st <= 0xA3)) free_blocks.push_back(b); // Free or deleted
    }
    if (free_blocks.size() < blocks) return false;

    for (std::size_t i = 0; i < free_blocks.size(); ++i) {
        u8* fr = data_.data() + free_blocks[i] * SECTOR_SIZE;
        std::fill(fr, fr + SECTOR_SIZE, static_cast<u8>(0));
        fr[0] = (i == 0) ? STATE_FIRST : (i + 1 == free_blocks.size() ? STATE_LAST : STATE_MIDDLE);
        if (blocks == 1) fr[0] = STATE_FIRST;
        if (i == 0) {
            wr32(fr + 4, blocks * BLOCK_SIZE);
            std::memcpy(fr + 0x0A, name.data(), name.size());
        }
        const u16 next = (i + 1 < free_blocks.size()) ? static_cast<u16>(free_blocks[i + 1] - 1) : 0xFFFFu;
        fr[8] = static_cast<u8>(next & 0xFF);
        fr[9] = static_cast<u8>(next >> 8);
        set_frame_checksum(fr);
        // A new file starts out zeroed
        std::fill(data_.begin() + free_blocks[i] * BLOCK_SIZE, data_.begin() + (free_blocks[i] + 1) * BLOCK_SIZE, static_cast<u8>(0));
    }
    dirty_ = true;
    out.name        = name;
    out.first_block = free_blocks[0];
    out.size        = blocks * BLOCK_SIZE;
    return true;
}

bool MemoryCard::erase_file(const std::string& name) {
    FileInfo f;
    if (!find_file(name, 1, f) || f.name != name) return false;
    u32 b = f.first_block;
    for (u32 guard = 0; guard < BLOCK_COUNT && b >= 1 && b <= BLOCK_COUNT; ++guard) {
        u8* fr = data_.data() + b * SECTOR_SIZE;
        // Deleted first/middle/last block (0xA1..0xA3) keeps the chain for "undelete" tools
        if (fr[0] >= STATE_FIRST && fr[0] <= STATE_LAST) fr[0] = static_cast<u8>(fr[0] + 0x50);
        set_frame_checksum(fr);
        const u16 next = static_cast<u16>(fr[8] | (fr[9] << 8));
        if (next == 0xFFFFu) break;
        b = next + 1u;
    }
    dirty_ = true;
    return true;
}

bool MemoryCard::rename_file(const std::string& old_name, const std::string& new_name) {
    if (new_name.empty() || new_name.size() > 20) return false;
    FileInfo f, clash;
    if (!find_file(old_name, 1, f) || f.name != old_name) return false;
    if (find_file(new_name, 1, clash) && clash.name == new_name) return false;
    u8* fr = data_.data() + f.first_block * SECTOR_SIZE;
    std::fill(fr + 0x0A, fr + 0x0A + 21, static_cast<u8>(0));
    std::memcpy(fr + 0x0A, new_name.data(), new_name.size());
    set_frame_checksum(fr);
    dirty_ = true;
    return true;
}

namespace {

// Block holding byte `offset` of a file, following the directory chain (0 when out of range)
u32 block_for_offset(const std::vector<u8>& card, u32 first_block, u32 offset) {
    u32 b = first_block;
    for (u32 hops = offset / MemoryCard::BLOCK_SIZE; hops > 0; --hops) {
        const u8* fr = card.data() + b * MemoryCard::SECTOR_SIZE;
        const u16 next = static_cast<u16>(fr[8] | (fr[9] << 8));
        if (next == 0xFFFFu || next >= MemoryCard::BLOCK_COUNT) return 0;
        b = next + 1u;
    }
    return b;
}

} // namespace

u32 MemoryCard::read_file(const FileInfo& file, u32 offset, u8* out, u32 length) const {
    if (!open_ || offset >= file.size) return 0;
    length = std::min(length, file.size - offset);
    u32 done = 0;
    while (done < length) {
        const u32 pos   = offset + done;
        const u32 block = block_for_offset(data_, file.first_block, pos);
        if (block == 0) break;
        const u32 in_block = pos % BLOCK_SIZE;
        const u32 chunk    = std::min(length - done, BLOCK_SIZE - in_block);
        std::memcpy(out + done, data_.data() + block * BLOCK_SIZE + in_block, chunk);
        done += chunk;
    }
    return done;
}

u32 MemoryCard::write_file(const FileInfo& file, u32 offset, const u8* in, u32 length) {
    if (!open_ || offset >= file.size) return 0;
    length = std::min(length, file.size - offset);
    u32 done = 0;
    while (done < length) {
        const u32 pos   = offset + done;
        const u32 block = block_for_offset(data_, file.first_block, pos);
        if (block == 0) break;
        const u32 in_block = pos % BLOCK_SIZE;
        const u32 chunk    = std::min(length - done, BLOCK_SIZE - in_block);
        std::memcpy(data_.data() + block * BLOCK_SIZE + in_block, in + done, chunk);
        done += chunk;
    }
    if (done > 0) dirty_ = true;
    return done;
}

u32 MemoryCard::used_blocks() const noexcept {
    u32 used = 0;
    for (u32 f = 1; f <= 15; ++f) {
        const u8 state = data_[f * SECTOR_SIZE];
        if (state == 0x51 || state == 0x52 || state == 0x53) ++used; // first / middle / last block
    }
    return used;
}

} // namespace vsport
