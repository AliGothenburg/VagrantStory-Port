#include "vsport/cdrom_iso.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iterator>
#include <sstream>

namespace vsport {

namespace {

u32 read_le32(const u8* p) noexcept {
    return static_cast<u32>(p[0]) |
           (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

std::string trim_spaces(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return std::string(sv);
}

std::string normalize_iso_path(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (char ch : raw) {
        if (ch == '\\') ch = '/';
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    // Strip leading cdrom: or slashes
    if (out.rfind("CDROM:", 0) == 0) {
        out.erase(0, 6);
    }
    while (!out.empty() && out.front() == '/') {
        out.erase(out.begin());
    }
    // Strip ISO9660 ";1" version suffix
    const auto semi = out.find(';');
    if (semi != std::string::npos) {
        out.erase(semi);
    }
    while (!out.empty() && out.back() == '.') {
        // Keep extension dots like SLUS_010.40, only strip trailing empty dot "FILENAME."
        break;
    }
    return out;
}

} // namespace

std::filesystem::path DiscImage::resolve_cue_to_bin(const std::filesystem::path& cue_path, std::string* err_out) {
    std::ifstream cue(cue_path);
    if (!cue.is_open()) {
        if (err_out) *err_out = "CUE dosyasi acilamadi: " + cue_path.string();
        return {};
    }

    std::string line;
    while (std::getline(cue, line)) {
        const std::string trimmed = trim_spaces(line);
        std::string upper = trimmed;
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        if (upper.rfind("FILE ", 0) == 0) {
            const auto q1 = trimmed.find('"');
            const auto q2 = (q1 != std::string::npos) ? trimmed.find('"', q1 + 1) : std::string::npos;
            std::string filename;
            if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1 + 1) {
                filename = trimmed.substr(q1 + 1, q2 - q1 - 1);
            } else {
                std::istringstream iss(trimmed.substr(5));
                iss >> filename;
            }
            if (!filename.empty()) {
                auto candidate = cue_path.parent_path() / filename;
                if (std::filesystem::exists(candidate)) {
                    return candidate;
                }
                if (err_out) *err_out = "CUE icindeki BIN dosyasi bulunamadi: " + candidate.string();
                return {};
            }
        }
    }

    if (err_out) *err_out = "CUE icinde gecerli bir FILE satiri bulunamadi.";
    return {};
}

bool DiscImage::open(const std::filesystem::path& path, std::string* err_out) {
    stream_.close();
    entries_.clear();
    volume_id_.clear();
    subq_overrides_.clear();
    sbi_path_.clear();
    track_pregap_offset_ = 0;

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (ext == ".cue") {
        bin_path_ = resolve_cue_to_bin(path, err_out);
        if (bin_path_.empty()) {
            return false;
        }
    } else {
        bin_path_ = path;
    }

    stream_.open(bin_path_, std::ios::binary | std::ios::ate);
    if (!stream_.is_open()) {
        if (err_out) *err_out = "Disk imaji acilamadi: " + bin_path_.string();
        return false;
    }

    file_size_ = static_cast<u64>(stream_.tellg());
    if (file_size_ < 17 * 2048) {
        if (err_out) *err_out = "Disk imaji ISO9660 PVD (LBA 16) icin cok kucuk.";
        return false;
    }

    if (!detect_geometry(err_out)) {
        return false;
    }

    if (!parse_iso9660(err_out)) {
        return false;
    }

    // Pick up "<name>.sbi" next to the .cue or the .bin (Redump naming), if present
    for (const auto& base : {path, bin_path_}) {
        auto candidate = base;
        candidate.replace_extension(".sbi");
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && load_sbi(candidate, nullptr)) {
            break;
        }
    }
    return true;
}

namespace {

std::array<u8, 3> lba_to_bcd_msf(u32 frames) noexcept {
    const auto bcd = [](u32 v) { return static_cast<u8>(((v / 10u) << 4) | (v % 10u)); };
    return {bcd(frames / (60u * 75u)), bcd((frames / 75u) % 60u), bcd(frames % 75u)};
}

std::array<u8, 10> regular_subchannel_q(u32 lba) noexcept {
    // Mode 1 Q for a data track: ctrl 4 (data) / adr 1 (position), track 01, index 01,
    // relative time from the track start (= LBA), absolute time (LBA + 2 s pregap)
    const auto rel = lba_to_bcd_msf(lba);
    const auto abs = lba_to_bcd_msf(lba + 150u);
    return {0x41u, 0x01u, 0x01u, rel[0], rel[1], rel[2], 0x00u, abs[0], abs[1], abs[2]};
}

u32 bcd_msf_to_lba(const u8* msf) noexcept {
    const auto bin = [](u8 b) { return static_cast<u32>((b >> 4) * 10u + (b & 0x0Fu)); };
    const u32 frames = (bin(msf[0]) * 60u + bin(msf[1])) * 75u + bin(msf[2]);
    return frames >= 150u ? frames - 150u : 0u;
}

} // namespace

bool DiscImage::load_sbi(const std::filesystem::path& path, std::string* err_out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        if (err_out) *err_out = "SBI dosyasi acilamadi: " + path.string();
        return false;
    }
    const std::vector<u8> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < 4 || std::memcmp(data.data(), "SBI\0", 4) != 0) {
        if (err_out) *err_out = "Gecersiz SBI imzasi ('SBI\\0' bekleniyordu): " + path.string();
        return false;
    }

    // Records: absolute MSF (3 bytes BCD) + type + payload
    //   type 1: full 10-byte Q (no CRC); type 2: relative MSF (3 bytes); type 3: absolute MSF (3 bytes)
    std::unordered_map<u32, std::array<u8, 10>> parsed;
    std::size_t pos = 4;
    while (pos + 4 <= data.size()) {
        const u32 lba  = bcd_msf_to_lba(&data[pos]);
        const u8  type = data[pos + 3];
        const std::size_t len = (type == 1u) ? 10u : (type == 2u || type == 3u) ? 3u : 0u;
        if (len == 0 || pos + 4 + len > data.size()) {
            if (err_out) *err_out = "Bozuk SBI kaydi (konum " + std::to_string(pos) + "): " + path.string();
            return false;
        }
        auto q = parsed.contains(lba) ? parsed[lba] : regular_subchannel_q(lba);
        const u8* payload = &data[pos + 4];
        if (type == 1u) {
            std::memcpy(q.data(), payload, 10);
        } else if (type == 2u) {
            std::memcpy(&q[3], payload, 3);
        } else {
            std::memcpy(&q[7], payload, 3);
        }
        parsed[lba] = q;
        pos += 4 + len;
    }

    subq_overrides_ = std::move(parsed);
    sbi_path_       = path;
    return true;
}

std::array<u8, 10> DiscImage::subchannel_q(u32 lba) const {
    if (const auto it = subq_overrides_.find(lba); it != subq_overrides_.end()) {
        return it->second;
    }
    return regular_subchannel_q(lba);
}

bool DiscImage::detect_geometry(std::string* err_out) {
    // Check LBA 16 in 2352-byte Mode2 XA (+24), 2352-byte Mode1 (+16), or 2048-byte ISO (+0)
    struct Probe {
        DiscSectorFormat fmt;
        u32 stride;
        u32 data_off;
    };
    constexpr std::array<Probe, 3> PROBES = {{
        {DiscSectorFormat::Raw2352Mode2XA, 2352, 24},
        {DiscSectorFormat::Raw2352Mode1,   2352, 16},
        {DiscSectorFormat::Cooked2048,     2048, 0}
    }};

    std::array<u8, 8> sig{};
    for (const auto& p : PROBES) {
        const u64 pvd_pos = static_cast<u64>(16) * p.stride + p.data_off;
        if (pvd_pos + 8 > file_size_) continue;

        stream_.seekg(static_cast<std::streamoff>(pvd_pos), std::ios::beg);
        stream_.read(reinterpret_cast<char*>(sig.data()), 6);
        if (stream_.good() && sig[0] == 0x01 && std::memcmp(&sig[1], "CD001", 5) == 0) {
            format_        = p.fmt;
            total_sectors_ = static_cast<u32>(file_size_ / p.stride);
            return true;
        }
    }

    // Also probe 150-sector (2-second) pregap if present in raw dumps
    for (const auto& p : PROBES) {
        if (p.stride != 2352) continue;
        const u64 pregap_bytes = 150ull * 2352ull;
        const u64 pvd_pos      = pregap_bytes + static_cast<u64>(16) * p.stride + p.data_off;
        if (pvd_pos + 8 > file_size_) continue;

        stream_.seekg(static_cast<std::streamoff>(pvd_pos), std::ios::beg);
        stream_.read(reinterpret_cast<char*>(sig.data()), 6);
        if (stream_.good() && sig[0] == 0x01 && std::memcmp(&sig[1], "CD001", 5) == 0) {
            format_              = p.fmt;
            track_pregap_offset_ = static_cast<u32>(pregap_bytes);
            total_sectors_       = static_cast<u32>((file_size_ - pregap_bytes) / p.stride);
            return true;
        }
    }

    if (err_out) *err_out = "LBA 16 uzerinde gecerli bir ISO9660 'CD001' Primary Volume Descriptor bulunamadi.";
    return false;
}

bool DiscImage::read_sector_2048(u32 lba, u8* out_buf) const {
    if (!stream_.is_open() || lba >= total_sectors_) {
        return false;
    }

    u64 stride   = 2048;
    u64 data_off = 0;
    if (format_ == DiscSectorFormat::Raw2352Mode2XA) {
        stride   = 2352;
        data_off = 24;
    } else if (format_ == DiscSectorFormat::Raw2352Mode1) {
        stride   = 2352;
        data_off = 16;
    }

    const u64 offset = static_cast<u64>(track_pregap_offset_) + static_cast<u64>(lba) * stride + data_off;
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    stream_.read(reinterpret_cast<char*>(out_buf), 2048);
    return stream_.gcount() == 2048;
}

bool DiscImage::read_sector_raw2352(u32 lba, u8* out_buf) const {
    if (!stream_.is_open() || lba >= total_sectors_) {
        return false;
    }

    if (format_ == DiscSectorFormat::Cooked2048) {
        std::memset(out_buf, 0, 2352);
        // Synthesize sync + Mode2 Form1 header around 2048-byte user data
        std::memset(out_buf + 1, 0xFF, 10);
        out_buf[15] = 2; // Mode 2
        return read_sector_2048(lba, out_buf + 24);
    }

    const u64 offset = static_cast<u64>(track_pregap_offset_) + static_cast<u64>(lba) * 2352ull;
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    stream_.read(reinterpret_cast<char*>(out_buf), 2352);
    return stream_.gcount() == 2352;
}

bool DiscImage::parse_iso9660(std::string* err_out) {
    std::array<u8, 2048> pvd{};
    if (!read_sector_2048(16, pvd.data())) {
        if (err_out) *err_out = "PVD sektoru okunamadi.";
        return false;
    }

    volume_id_ = trim_spaces(std::string_view(reinterpret_cast<const char*>(&pvd[40]), 32));

    // Root directory record starts at offset 156 (34 bytes)
    const u8* root_rec  = &pvd[156];
    const u32 root_lba  = read_le32(&root_rec[2]);
    const u32 root_size = read_le32(&root_rec[10]);

    if (root_lba == 0 || root_size == 0) {
        if (err_out) *err_out = "ISO9660 kok dizin kaydi gecersiz.";
        return false;
    }

    walk_directory(root_lba, root_size, "", 0);
    return true;
}

void DiscImage::walk_directory(u32 dir_lba, u32 dir_size, const std::string& prefix, int depth) {
    if (depth > 8) return; // Guard against malformed cyclic records

    const u32 sector_count = (dir_size + 2047u) / 2048u;
    std::array<u8, 2048> sec{};

    for (u32 s = 0; s < sector_count; ++s) {
        if (!read_sector_2048(dir_lba + s, sec.data())) break;

        u32 pos = 0;
        while (pos < 2048) {
            const u8 rec_len = sec[pos];
            if (rec_len == 0) {
                break; // ISO9660 directory records do not straddle sector boundaries; rest is 0-padded
            }
            if (pos + rec_len > 2048 || rec_len < 34) {
                break;
            }

            const u32 extent_lba  = read_le32(&sec[pos + 2]);
            const u32 data_len    = read_le32(&sec[pos + 10]);
            const u8  file_flags  = sec[pos + 25];
            const u8  name_len    = sec[pos + 32];
            const bool is_dir     = (file_flags & 0x02) != 0;

            if (name_len > 0 && pos + 33 + name_len <= pos + rec_len) {
                // Skip '.' (0x00) and '..' (0x01)
                if (!(name_len == 1 && (sec[pos + 33] == 0x00 || sec[pos + 33] == 0x01))) {
                    std::string raw_name(reinterpret_cast<const char*>(&sec[pos + 33]), name_len);
                    const auto semi = raw_name.find(';');
                    if (semi != std::string::npos) {
                        raw_name.erase(semi);
                    }
                    // Strip trailing dot for extensionless ISO9660 filenames (e.g. "SYSTEM." -> "SYSTEM")
                    if (!raw_name.empty() && raw_name.back() == '.') {
                        raw_name.pop_back();
                    }

                    // Check CD-ROM XA System Use extension ("XA" signature at system use area)
                    bool is_xa_str = false;
                    const u32 sys_off = 33u + name_len + ((name_len % 2 == 0) ? 1u : 0u);
                    if (sys_off + 14 <= rec_len && sec[pos + sys_off + 6] == 'X' && sec[pos + sys_off + 7] == 'A') {
                        const u16 xa_attr = static_cast<u16>((sec[pos + sys_off + 4] << 8) | sec[pos + sys_off + 5]);
                        if ((xa_attr & 0x1000u) != 0) { // Mode 2 Form 2 flag
                            is_xa_str = true;
                        }
                    }

                    std::string full_path = prefix.empty() ? raw_name : (prefix + "/" + raw_name);

                    IsoFileEntry entry{};
                    entry.path       = full_path;
                    entry.lba        = extent_lba;
                    entry.size_bytes = data_len;
                    entry.is_dir     = is_dir;
                    entry.is_xa_str  = is_xa_str;
                    entries_.push_back(entry);

                    if (is_dir && extent_lba != dir_lba) {
                        walk_directory(extent_lba, data_len, full_path, depth + 1);
                    }
                }
            }

            pos += rec_len;
        }
    }
}

std::optional<IsoFileEntry> DiscImage::find_file(std::string_view iso_path) const {
    const std::string target = normalize_iso_path(iso_path);
    for (const auto& e : entries_) {
        if (normalize_iso_path(e.path) == target) {
            return e;
        }
    }
    return std::nullopt;
}

std::vector<u8> DiscImage::extract_entry(const IsoFileEntry& entry) const {
    if (entry.is_dir || entry.size_bytes == 0) {
        return {};
    }

    std::vector<u8> out(entry.size_bytes);
    const u32 sector_count = (entry.size_bytes + 2047u) / 2048u;
    std::array<u8, 2048> temp{};

    u32 written = 0;
    for (u32 s = 0; s < sector_count; ++s) {
        if (!read_sector_2048(entry.lba + s, temp.data())) {
            out.resize(written);
            return out;
        }
        const u32 to_copy = std::min<u32>(2048u, entry.size_bytes - written);
        std::memcpy(out.data() + written, temp.data(), to_copy);
        written += to_copy;
    }
    return out;
}

std::vector<u8> DiscImage::extract_file(std::string_view iso_path) const {
    const auto found = find_file(iso_path);
    if (!found.has_value()) {
        return {};
    }
    return extract_entry(*found);
}

std::optional<SystemCnfInfo> DiscImage::parse_system_cnf() const {
    const auto data = extract_file("SYSTEM.CNF");
    if (data.empty()) {
        return std::nullopt;
    }

    SystemCnfInfo info{};
    std::istringstream iss(std::string(reinterpret_cast<const char*>(data.data()), data.size()));
    std::string line;

    while (std::getline(iss, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim_spaces(std::string_view(line).substr(0, eq));
        std::string val = trim_spaces(std::string_view(line).substr(eq + 1));
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        if (key == "BOOT") {
            info.boot_raw  = val;
            info.boot_path = normalize_iso_path(val);
        } else if (key == "TCB") {
            info.tcb = static_cast<u32>(std::strtoul(val.c_str(), nullptr, 0));
        } else if (key == "EVENT") {
            info.event = static_cast<u32>(std::strtoul(val.c_str(), nullptr, 0));
        } else if (key == "STACK") {
            info.stack = static_cast<u32>(std::strtoul(val.c_str(), nullptr, 16));
        }
    }

    return info;
}

} // namespace vsport
