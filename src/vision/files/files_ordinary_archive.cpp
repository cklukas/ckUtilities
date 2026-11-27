// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_ordinary_archive.hpp"

#include "directory_cursor.hpp"
#include "files_native_io_helpers.hpp"
#include "native_filesystem.hpp"
#include "opened_entry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <utility>

#if defined(CKTOOLS_HAS_ZLIB)
#include <zlib.h>
#endif

namespace ck::vision {
namespace {

std::uint16_t read_u16_le(const std::byte *p) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(std::to_integer<unsigned char>(p[0])) |
        (static_cast<std::uint16_t>(std::to_integer<unsigned char>(p[1])) << 8U));
}

std::uint32_t read_u32_le(const std::byte *p) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

void write_u16_le(std::vector<std::byte> &out, std::uint16_t v) {
    out.push_back(std::byte{static_cast<unsigned char>(v & 0xFFU)});
    out.push_back(std::byte{static_cast<unsigned char>((v >> 8U) & 0xFFU)});
}

void write_u32_le(std::vector<std::byte> &out, std::uint32_t v) {
    out.push_back(std::byte{static_cast<unsigned char>(v & 0xFFU)});
    out.push_back(std::byte{static_cast<unsigned char>((v >> 8U) & 0xFFU)});
    out.push_back(std::byte{static_cast<unsigned char>((v >> 16U) & 0xFFU)});
    out.push_back(std::byte{static_cast<unsigned char>((v >> 24U) & 0xFFU)});
}

void append_bytes(std::vector<std::byte> &out, std::string_view sv) {
    for (const char ch : sv)
        out.push_back(std::byte{static_cast<unsigned char>(ch)});
}

void append_span(std::vector<std::byte> &out, std::span<const std::byte> data) {
    out.insert(out.end(), data.begin(), data.end());
}

std::string ascii_lower_str(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (const unsigned char ch : sv) {
        if (ch >= 'A' && ch <= 'Z')
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        else
            out.push_back(static_cast<char>(ch));
    }
    return out;
}

bool read_native_file_all(const ck::files::NativeAbsolutePath &path,
                          std::uint64_t max_bytes,
                          std::vector<std::byte> &out,
                          std::string &error_out,
                          std::stop_token stop_token) {
    std::uint64_t sz = 0;
    bool truncated = false;
    if (!detail::read_native_file_bounded(path, max_bytes, out, sz, truncated, error_out, stop_token))
        return false;
    if (truncated) {
        error_out = "File exceeds configured archive byte limit.";
        return false;
    }
    return true;
}

bool write_native_file_staged(const ck::files::NativeAbsolutePath &target_path,
                              std::span<const std::byte> data,
                              bool allow_replace,
                              std::string &error_out,
                              std::stop_token stop_token) {
    return detail::write_native_file_staged(
        target_path, data, allow_replace, ".ck-archive-stage-", error_out, stop_token);
}

#if defined(CKTOOLS_HAS_ZLIB)
bool zlib_inflate_raw(std::span<const std::byte> compressed,
                      std::uint64_t expected_size,
                      std::uint64_t max_bytes,
                      std::uint32_t max_ratio,
                      std::vector<std::byte> &out,
                      std::string &error_out) {
    if (expected_size > max_bytes) {
        error_out = "Decompressed entry exceeds size limit.";
        return false;
    }
    z_stream strm{};
    strm.next_in = reinterpret_cast<Bytef *>(const_cast<std::byte *>(compressed.data()));
    strm.avail_in = static_cast<uInt>(compressed.size());
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        error_out = "zlib inflateInit2 failed.";
        return false;
    }
    out.clear();
    out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(expected_size, 1024 * 1024)));
    std::array<std::byte, 32768> buf{};
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        strm.next_out = reinterpret_cast<Bytef *>(buf.data());
        strm.avail_out = static_cast<uInt>(buf.size());
        rc = inflate(&strm, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            inflateEnd(&strm);
            error_out = "Corrupt deflate stream.";
            return false;
        }
        const std::size_t produced = buf.size() - strm.avail_out;
        if (out.size() + produced > max_bytes) {
            inflateEnd(&strm);
            error_out = "Archive bomb guard: decompressed size exceeded limit.";
            return false;
        }
        if (compressed.size() > 0 && out.size() + produced > 1024 * 1024 &&
            (out.size() + produced) / compressed.size() > max_ratio) {
            inflateEnd(&strm);
            error_out = "Archive bomb guard: compression ratio exceeded limit.";
            return false;
        }
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(produced));
    }
    inflateEnd(&strm);
    return true;
}

bool zlib_deflate_raw(std::span<const std::byte> plain, std::vector<std::byte> &out) {
    z_stream strm{};
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return false;
    strm.next_in = reinterpret_cast<Bytef *>(const_cast<std::byte *>(plain.data()));
    strm.avail_in = static_cast<uInt>(plain.size());
    out.clear();
    std::array<std::byte, 32768> buf{};
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        strm.next_out = reinterpret_cast<Bytef *>(buf.data());
        strm.avail_out = static_cast<uInt>(buf.size());
        rc = deflate(&strm, Z_FINISH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            deflateEnd(&strm);
            return false;
        }
        const std::size_t produced = buf.size() - strm.avail_out;
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(produced));
    }
    deflateEnd(&strm);
    return true;
}

bool zlib_gunzip(std::span<const std::byte> gz,
                 std::uint64_t max_bytes,
                 std::uint32_t max_ratio,
                 std::vector<std::byte> &out,
                 std::string &error_out) {
    z_stream strm{};
    strm.next_in = reinterpret_cast<Bytef *>(const_cast<std::byte *>(gz.data()));
    strm.avail_in = static_cast<uInt>(gz.size());
    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {
        error_out = "zlib gunzip init failed.";
        return false;
    }
    out.clear();
    std::array<std::byte, 32768> buf{};
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        strm.next_out = reinterpret_cast<Bytef *>(buf.data());
        strm.avail_out = static_cast<uInt>(buf.size());
        rc = inflate(&strm, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            inflateEnd(&strm);
            error_out = "Corrupt gzip stream.";
            return false;
        }
        const std::size_t produced = buf.size() - strm.avail_out;
        if (out.size() + produced > max_bytes) {
            inflateEnd(&strm);
            error_out = "Archive bomb guard: gunzip output exceeded limit.";
            return false;
        }
        if (gz.size() > 0 && out.size() + produced > 1024 * 1024 &&
            (out.size() + produced) / gz.size() > max_ratio) {
            inflateEnd(&strm);
            error_out = "Archive bomb guard: gzip ratio exceeded limit.";
            return false;
        }
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(produced));
    }
    inflateEnd(&strm);
    return true;
}

bool zlib_gzip(std::span<const std::byte> plain, std::vector<std::byte> &out) {
    z_stream strm{};
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return false;
    strm.next_in = reinterpret_cast<Bytef *>(const_cast<std::byte *>(plain.data()));
    strm.avail_in = static_cast<uInt>(plain.size());
    out.clear();
    std::array<std::byte, 32768> buf{};
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        strm.next_out = reinterpret_cast<Bytef *>(buf.data());
        strm.avail_out = static_cast<uInt>(buf.size());
        rc = deflate(&strm, Z_FINISH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            deflateEnd(&strm);
            return false;
        }
        const std::size_t produced = buf.size() - strm.avail_out;
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(produced));
    }
    deflateEnd(&strm);
    return true;
}
#endif

// ---- TAR helpers ----
bool is_zero_block(const std::byte *blk) noexcept {
    for (std::size_t i = 0; i < 512; ++i) {
        if (blk[i] != std::byte{0})
            return false;
    }
    return true;
}

bool parse_tar_octal(const std::byte *field, std::size_t len, std::uint64_t &out) noexcept {
    out = 0;
    std::size_t i = 0;
    while (i < len && (std::to_integer<char>(field[i]) == ' ' || std::to_integer<char>(field[i]) == '\0'))
        ++i;
    bool any = false;
    while (i < len) {
        const char ch = std::to_integer<char>(field[i]);
        if (ch == ' ' || ch == '\0')
            break;
        if (ch < '0' || ch > '7')
            return false;
        any = true;
        out = (out << 3U) | static_cast<std::uint64_t>(ch - '0');
        ++i;
    }
    return any || len > 0;
}

bool verify_tar_checksum(const std::byte *blk) noexcept {
    std::uint64_t stored = 0;
    if (!parse_tar_octal(blk + 148, 8, stored))
        return false;
    std::uint64_t unsigned_sum = 0;
    for (std::size_t i = 0; i < 512; ++i) {
        if (i >= 148 && i < 156)
            unsigned_sum += static_cast<unsigned char>(' ');
        else
            unsigned_sum += std::to_integer<unsigned char>(blk[i]);
    }
    return unsigned_sum == stored;
}

std::string tar_cstr(const std::byte *field, std::size_t max_len) {
    std::size_t len = 0;
    while (len < max_len && field[len] != std::byte{0})
        ++len;
    return std::string(reinterpret_cast<const char *>(field), len);
}

struct ParsedMember final {
    OrdinaryArchiveEntry entry;
    std::size_t data_offset = 0;
    std::size_t data_length = 0;
};

bool parse_tar_buffer(std::span<const std::byte> data,
                      const OrdinaryArchiveLimits &limits,
                      std::vector<ParsedMember> &members,
                      OrdinaryArchiveCatalog &catalog) {
    std::size_t off = 0;
    std::string pax_path;
    std::string pax_linkpath;
    std::set<std::string> seen_folded;

    while (off + 512 <= data.size()) {
        const std::byte *hdr = data.data() + off;
        if (is_zero_block(hdr))
            break;
        if (!verify_tar_checksum(hdr)) {
            catalog.error_message = "Malformed TAR header checksum.";
            return false;
        }
        std::uint64_t sz = 0;
        if (!parse_tar_octal(hdr + 124, 12, sz)) {
            catalog.error_message = "Malformed TAR entry size.";
            return false;
        }
        if (sz > limits.max_entry_bytes || off + 512 + sz > data.size()) {
            catalog.error_message = "TAR entry size exceeds archive bounds or limit.";
            return false;
        }
        const char typeflag = std::to_integer<char>(hdr[156]);
        const std::size_t data_off = off + 512;
        const std::size_t padded = static_cast<std::size_t>(((sz + 511ULL) / 512ULL) * 512ULL);

        // PAX extended header ('x') for long paths/linkpaths.
        if (typeflag == 'x') {
            std::string_view pax(reinterpret_cast<const char *>(data.data() + data_off),
                                 static_cast<std::size_t>(sz));
            std::size_t pos = 0;
            while (pos < pax.size()) {
                const std::size_t sp = pax.find(' ', pos);
                if (sp == std::string_view::npos)
                    break;
                std::size_t rec_len = 0;
                for (std::size_t k = pos; k < sp; ++k) {
                    if (pax[k] < '0' || pax[k] > '9') {
                        rec_len = 0;
                        break;
                    }
                    rec_len = rec_len * 10 + static_cast<std::size_t>(pax[k] - '0');
                }
                if (rec_len == 0 || pos + rec_len > pax.size() || pos + rec_len <= sp + 1)
                    break;
                std::string_view kv = pax.substr(sp + 1, pos + rec_len - (sp + 1));
                if (!kv.empty() && kv.back() == '\n')
                    kv.remove_suffix(1);
                const std::size_t eq = kv.find('=');
                if (eq != std::string_view::npos) {
                    const std::string_view key = kv.substr(0, eq);
                    const std::string_view val = kv.substr(eq + 1);
                    if (key == "path")
                        pax_path = std::string(val);
                    else if (key == "linkpath")
                        pax_linkpath = std::string(val);
                }
                pos += rec_len;
            }
            off = data_off + padded;
            continue;
        }

        if (members.size() >= limits.max_entries) {
            catalog.error_message = "Archive exceeds maximum entry count.";
            return false;
        }

        std::string name = tar_cstr(hdr, 100);
        const std::string prefix = tar_cstr(hdr + 345, 155);
        if (!prefix.empty())
            name = prefix + "/" + name;
        if (!pax_path.empty()) {
            name = std::move(pax_path);
            pax_path.clear();
        }
        std::string linkname = tar_cstr(hdr + 157, 100);
        if (!pax_linkpath.empty()) {
            linkname = std::move(pax_linkpath);
            pax_linkpath.clear();
        }

        OrdinaryArchiveEntry entry;
        entry.index = members.size();
        entry.path = name;
        entry.uncompressed_size = sz;
        entry.compressed_size = sz;
        entry.link_target = linkname;

        if (typeflag == '5' || (!name.empty() && name.back() == '/'))
            entry.kind = OrdinaryArchiveEntryKind::Directory;
        else if (typeflag == '0' || typeflag == '\0')
            entry.kind = OrdinaryArchiveEntryKind::RegularFile;
        else if (typeflag == '2')
            entry.kind = OrdinaryArchiveEntryKind::Symlink;
        else if (typeflag == '1')
            entry.kind = OrdinaryArchiveEntryKind::HardLink;
        else
            entry.kind = OrdinaryArchiveEntryKind::Unsupported;

        if (entry.kind == OrdinaryArchiveEntryKind::Directory) {
            while (!entry.path.empty() && entry.path.back() == '/')
                entry.path.pop_back();
        }

        std::string reason;
        if (!validate_ordinary_archive_entry_path(entry.path, &reason)) {
            entry.safe_path = false;
            entry.safety_warning = reason;
            catalog.has_unsafe_entries = true;
        } else if ((entry.kind == OrdinaryArchiveEntryKind::Symlink ||
                    entry.kind == OrdinaryArchiveEntryKind::HardLink) &&
                   !validate_ordinary_archive_link_target(entry.path, entry.link_target, &reason)) {
            entry.safe_path = false;
            entry.safety_warning = reason;
            catalog.has_unsafe_entries = true;
        }

        if (!seen_folded.insert(ascii_lower_str(entry.path)).second)
            catalog.has_collisions = true;

        if (catalog.total_uncompressed_bytes + sz > limits.max_total_bytes) {
            catalog.error_message = "Archive total uncompressed size exceeds limit.";
            return false;
        }
        catalog.total_uncompressed_bytes += sz;

        if (entry.kind == OrdinaryArchiveEntryKind::RegularFile)
            entry.crc32 = ordinary_archive_crc32(data.subspan(data_off, static_cast<std::size_t>(sz)));

        members.push_back(ParsedMember{entry, data_off, static_cast<std::size_t>(sz)});
        catalog.entries.push_back(std::move(entry));
        off = data_off + padded;
    }
    return true;
}

void append_tar_entry(std::vector<std::byte> &out,
                      std::string_view path,
                      bool is_directory,
                      std::span<const std::byte> payload) {
    std::string tar_name(path);
    if (is_directory && (tar_name.empty() || tar_name.back() != '/'))
        tar_name.push_back('/');

    // Emit PAX header if name > 100 bytes.
    if (tar_name.size() > 100) {
        std::string body = " path=" + tar_name + "\n";
        std::size_t total = body.size() + 1;
        while (std::to_string(total).size() + body.size() != total)
            total = std::to_string(total).size() + body.size();
        std::string pax = std::to_string(total) + body;
        std::array<std::byte, 512> phdr{};
        std::string pax_name = "PaxHeader/longname";
        std::memcpy(phdr.data(), pax_name.data(), pax_name.size());
        std::memcpy(phdr.data() + 100, "0000644\0", 8);
        std::memcpy(phdr.data() + 108, "0000000\0", 8);
        std::memcpy(phdr.data() + 116, "0000000\0", 8);
        char sz_buf[16]{};
        std::snprintf(sz_buf, sizeof(sz_buf), "%011llo", static_cast<unsigned long long>(pax.size()));
        std::memcpy(phdr.data() + 124, sz_buf, 11);
        std::memcpy(phdr.data() + 136, "00000000000\0", 12);
        std::memset(phdr.data() + 148, ' ', 8);
        phdr[156] = std::byte{'x'};
        std::memcpy(phdr.data() + 257, "ustar\0", 6);
        std::memcpy(phdr.data() + 263, "00", 2);
        unsigned int csum = 0;
        for (const std::byte b : phdr)
            csum += std::to_integer<unsigned char>(b);
        char csum_buf[16]{};
        std::snprintf(csum_buf, sizeof(csum_buf), "%06o", csum);
        std::memcpy(phdr.data() + 148, csum_buf, 6);
        phdr[154] = std::byte{0};
        phdr[155] = std::byte{' '};
        append_span(out, phdr);
        append_bytes(out, pax);
        const std::size_t pad = (512 - (pax.size() % 512)) % 512;
        out.insert(out.end(), pad, std::byte{0});
    }

    std::array<std::byte, 512> hdr{};
    const std::size_t copy_name = std::min<std::size_t>(tar_name.size(), 100);
    std::memcpy(hdr.data(), tar_name.data(), copy_name);
    std::memcpy(hdr.data() + 100, is_directory ? "0000755\0" : "0000644\0", 8);
    std::memcpy(hdr.data() + 108, "0000000\0", 8);
    std::memcpy(hdr.data() + 116, "0000000\0", 8);
    char sz_buf[16]{};
    std::snprintf(sz_buf, sizeof(sz_buf), "%011llo",
                  static_cast<unsigned long long>(is_directory ? 0ULL : payload.size()));
    std::memcpy(hdr.data() + 124, sz_buf, 11);
    std::memcpy(hdr.data() + 136, "00000000000\0", 12);
    std::memset(hdr.data() + 148, ' ', 8);
    hdr[156] = std::byte{static_cast<unsigned char>(is_directory ? '5' : '0')};
    std::memcpy(hdr.data() + 257, "ustar\0", 6);
    std::memcpy(hdr.data() + 263, "00", 2);
    unsigned int csum = 0;
    for (const std::byte b : hdr)
        csum += std::to_integer<unsigned char>(b);
    char csum_buf[16]{};
    std::snprintf(csum_buf, sizeof(csum_buf), "%06o", csum);
    std::memcpy(hdr.data() + 148, csum_buf, 6);
    hdr[154] = std::byte{0};
    hdr[155] = std::byte{' '};

    append_span(out, hdr);
    if (!is_directory && !payload.empty()) {
        append_span(out, payload);
        const std::size_t pad = (512 - (payload.size() % 512)) % 512;
        out.insert(out.end(), pad, std::byte{0});
    }
}

// ---- ZIP helpers ----
constexpr std::uint32_t kZipLocalSig = 0x04034B50U;
constexpr std::uint32_t kZipCentralSig = 0x02014B50U;
constexpr std::uint32_t kZipEocdSig = 0x06054B50U;

bool parse_zip_buffer(std::span<const std::byte> data,
                      const OrdinaryArchiveLimits &limits,
                      std::vector<ParsedMember> &members,
                      OrdinaryArchiveCatalog &catalog) {
    if (data.size() < 22) {
        catalog.error_message = "ZIP archive is too small (missing EOCD).";
        return false;
    }
    const std::size_t min_eocd = data.size() > 65557 ? data.size() - 65557 : 0;
    std::size_t eocd_pos = std::string::npos;
    for (std::size_t pos = data.size() - 22 + 1; pos-- > min_eocd;) {
        if (read_u32_le(data.data() + pos) == kZipEocdSig) {
            const std::uint16_t comment_len = read_u16_le(data.data() + pos + 20);
            if (pos + 22 + comment_len == data.size()) {
                eocd_pos = pos;
                break;
            }
        }
    }
    if (eocd_pos == std::string::npos) {
        catalog.error_message = "Malformed ZIP: End of Central Directory record not found.";
        return false;
    }
    const std::uint16_t entry_count = read_u16_le(data.data() + eocd_pos + 10);
    const std::uint32_t cd_size = read_u32_le(data.data() + eocd_pos + 12);
    const std::uint32_t cd_offset = read_u32_le(data.data() + eocd_pos + 16);
    if (entry_count > limits.max_entries) {
        catalog.error_message = "ZIP archive exceeds maximum entry limit.";
        return false;
    }
    if (static_cast<std::uint64_t>(cd_offset) + cd_size > eocd_pos) {
        catalog.error_message = "Malformed ZIP: Central Directory extends past EOCD.";
        return false;
    }

    std::size_t pos = cd_offset;
    std::set<std::string> seen_folded;
    for (std::uint16_t idx = 0; idx < entry_count; ++idx) {
        if (pos + 46 > cd_offset + cd_size) {
            catalog.error_message = "Malformed ZIP: truncated Central Directory header.";
            return false;
        }
        const std::byte *cd = data.data() + pos;
        if (read_u32_le(cd) != kZipCentralSig) {
            catalog.error_message = "Malformed ZIP: invalid Central Directory signature.";
            return false;
        }
        const std::uint16_t flags = read_u16_le(cd + 8);
        const std::uint16_t method = read_u16_le(cd + 10);
        const std::uint32_t crc = read_u32_le(cd + 16);
        const std::uint32_t comp_size = read_u32_le(cd + 20);
        const std::uint32_t uncomp_size = read_u32_le(cd + 24);
        const std::uint16_t name_len = read_u16_le(cd + 28);
        const std::uint16_t extra_len = read_u16_le(cd + 30);
        const std::uint16_t comment_len = read_u16_le(cd + 32);
        const std::uint32_t ext_attr = read_u32_le(cd + 38);
        const std::uint32_t local_off = read_u32_le(cd + 42);
        if (pos + 46 + name_len + extra_len + comment_len > cd_offset + cd_size) {
            catalog.error_message = "Malformed ZIP: Central Directory entry out of bounds.";
            return false;
        }
        std::string name(reinterpret_cast<const char *>(cd + 46), name_len);
        pos += 46 + name_len + extra_len + comment_len;

        if (uncomp_size > limits.max_entry_bytes) {
            catalog.error_message = "Archive bomb guard: ZIP entry uncompressed size exceeds limit.";
            return false;
        }
        if (comp_size > 0 && uncomp_size > 1024 * 1024 &&
            uncomp_size / comp_size > limits.max_compression_ratio) {
            catalog.error_message = "Archive bomb guard: ZIP compression ratio exceeds limit.";
            return false;
        }
        if (catalog.total_uncompressed_bytes + uncomp_size > limits.max_total_bytes) {
            catalog.error_message = "Archive bomb guard: total uncompressed bytes exceed limit.";
            return false;
        }
        catalog.total_uncompressed_bytes += uncomp_size;

        if (local_off + 30 > data.size() || read_u32_le(data.data() + local_off) != kZipLocalSig) {
            catalog.error_message = "Malformed ZIP: invalid local file header.";
            return false;
        }
        const std::uint16_t lh_name_len = read_u16_le(data.data() + local_off + 26);
        const std::uint16_t lh_extra_len = read_u16_le(data.data() + local_off + 28);
        const std::size_t data_off = local_off + 30 + lh_name_len + lh_extra_len;
        if (data_off + comp_size > data.size()) {
            catalog.error_message = "Malformed ZIP: local entry data out of bounds.";
            return false;
        }

        OrdinaryArchiveEntry entry;
        entry.index = idx;
        entry.path = name;
        entry.uncompressed_size = uncomp_size;
        entry.compressed_size = comp_size;
        entry.crc32 = crc;
        entry.compression_method = method;
        entry.encrypted = (flags & 0x0001U) != 0;

        const std::uint32_t unix_mode = (ext_attr >> 16U) & 0xF000U;
        if (!name.empty() && name.back() == '/') {
            entry.kind = OrdinaryArchiveEntryKind::Directory;
            while (!entry.path.empty() && entry.path.back() == '/')
                entry.path.pop_back();
        } else if (unix_mode == 0xA000U) {
            entry.kind = OrdinaryArchiveEntryKind::Symlink;
        } else {
            entry.kind = OrdinaryArchiveEntryKind::RegularFile;
        }

        std::string reason;
        if (!validate_ordinary_archive_entry_path(entry.path, &reason)) {
            entry.safe_path = false;
            entry.safety_warning = reason;
            catalog.has_unsafe_entries = true;
        }
        if (!seen_folded.insert(ascii_lower_str(entry.path)).second)
            catalog.has_collisions = true;

        members.push_back(ParsedMember{entry, data_off, comp_size});
        catalog.entries.push_back(std::move(entry));
    }
    return true;
}

bool decode_member_bytes(OrdinaryArchiveFormat format,
                         std::span<const std::byte> archive_buf,
                         const ParsedMember &member,
                         const OrdinaryArchiveLimits &limits,
                         std::vector<std::byte> &out,
                         std::string &error_out) {
    if (member.entry.encrypted) {
        error_out = "Encrypted ZIP entries are not supported.";
        return false;
    }
    if (member.data_offset + member.data_length > archive_buf.size()) {
        error_out = "Archive member data out of bounds.";
        return false;
    }
    const auto slice = archive_buf.subspan(member.data_offset, member.data_length);
    if (format == OrdinaryArchiveFormat::Tar || format == OrdinaryArchiveFormat::TarGz) {
        out.assign(slice.begin(), slice.end());
        return true;
    }
    if (format == OrdinaryArchiveFormat::Zip) {
        if (member.entry.compression_method == 0) {
            out.assign(slice.begin(), slice.end());
        } else if (member.entry.compression_method == 8) {
#if defined(CKTOOLS_HAS_ZLIB)
            if (!zlib_inflate_raw(slice, member.entry.uncompressed_size,
                                  limits.max_entry_bytes, limits.max_compression_ratio,
                                  out, error_out))
                return false;
#else
            (void)limits;
            error_out = "Deflate ZIP decompression requires zlib support in this build.";
            return false;
#endif
        } else {
            error_out = "Unsupported ZIP compression method.";
            return false;
        }
        if (ordinary_archive_crc32(out) != member.entry.crc32) {
            error_out = "ZIP entry CRC-32 verification failed.";
            return false;
        }
        return true;
    }
    error_out = "Unsupported archive format.";
    return false;
}

bool build_zip_buffer(const std::vector<std::pair<std::string, std::pair<bool, std::vector<std::byte>>>> &entries,
                      bool use_deflate,
                      std::vector<std::byte> &out) {
    out.clear();
    struct CdRecord final {
        std::string name;
        bool is_dir = false;
        std::uint16_t method = 0;
        std::uint32_t crc = 0;
        std::uint32_t comp_size = 0;
        std::uint32_t uncomp_size = 0;
        std::uint32_t local_offset = 0;
    };
    std::vector<CdRecord> records;
    records.reserve(entries.size());

    for (const auto &[raw_path, body] : entries) {
        const bool is_dir = body.first;
        const auto &plain = body.second;
        std::string zip_name = raw_path;
        if (is_dir && (zip_name.empty() || zip_name.back() != '/'))
            zip_name.push_back('/');

        std::uint16_t method = 0;
        std::vector<std::byte> stored;
        std::span<const std::byte> payload = plain;
        if (!is_dir && use_deflate && !plain.empty()) {
#if defined(CKTOOLS_HAS_ZLIB)
            if (zlib_deflate_raw(plain, stored) && stored.size() < plain.size()) {
                method = 8;
                payload = stored;
            }
#endif
        }
        const std::uint32_t crc = is_dir ? 0U : ordinary_archive_crc32(plain);
        const std::uint32_t local_offset = static_cast<std::uint32_t>(out.size());

        write_u32_le(out, kZipLocalSig);
        write_u16_le(out, 20); // version needed
        write_u16_le(out, 0x0800U); // UTF-8 filename flag
        write_u16_le(out, method);
        write_u16_le(out, 0); // mod time
        write_u16_le(out, 0); // mod date
        write_u32_le(out, crc);
        write_u32_le(out, static_cast<std::uint32_t>(payload.size()));
        write_u32_le(out, static_cast<std::uint32_t>(plain.size()));
        write_u16_le(out, static_cast<std::uint16_t>(zip_name.size()));
        write_u16_le(out, 0); // extra len
        append_bytes(out, zip_name);
        if (!is_dir)
            append_span(out, payload);

        records.push_back(CdRecord{
            std::move(zip_name),
            is_dir,
            method,
            crc,
            static_cast<std::uint32_t>(payload.size()),
            static_cast<std::uint32_t>(plain.size()),
            local_offset,
        });
    }

    const std::uint32_t cd_offset = static_cast<std::uint32_t>(out.size());
    for (const auto &rec : records) {
        write_u32_le(out, kZipCentralSig);
        write_u16_le(out, 0x0314U); // UNIX + 2.0
        write_u16_le(out, 20);
        write_u16_le(out, 0x0800U);
        write_u16_le(out, rec.method);
        write_u16_le(out, 0);
        write_u16_le(out, 0);
        write_u32_le(out, rec.crc);
        write_u32_le(out, rec.comp_size);
        write_u32_le(out, rec.uncomp_size);
        write_u16_le(out, static_cast<std::uint16_t>(rec.name.size()));
        write_u16_le(out, 0);
        write_u16_le(out, 0);
        write_u16_le(out, 0);
        write_u16_le(out, 0);
        write_u32_le(out, rec.is_dir ? (0x41ED0010U) : (0x81A40000U));
        write_u32_le(out, rec.local_offset);
        append_bytes(out, rec.name);
    }
    const std::uint32_t cd_size = static_cast<std::uint32_t>(out.size() - cd_offset);

    write_u32_le(out, kZipEocdSig);
    write_u16_le(out, 0);
    write_u16_le(out, 0);
    write_u16_le(out, static_cast<std::uint16_t>(records.size()));
    write_u16_le(out, static_cast<std::uint16_t>(records.size()));
    write_u32_le(out, cd_size);
    write_u32_le(out, cd_offset);
    write_u16_le(out, 0);
    return true;
}

bool load_and_parse_archive(const ck::files::NativeAbsolutePath &archive_path,
                            const OrdinaryArchiveLimits &limits,
                            std::vector<std::byte> &backing_buf,
                            std::vector<ParsedMember> &members,
                            OrdinaryArchiveCatalog &catalog,
                            std::stop_token stop_token) {
    std::string err;
    std::vector<std::byte> raw;
    if (!read_native_file_all(archive_path, limits.max_archive_bytes, raw, err, stop_token)) {
        catalog.error_message = err;
        return false;
    }
    const auto split_arc = detail::split_native_path(archive_path);
    const std::string leaf = split_arc ? split_arc->leaf.display_name() : std::string{};
    catalog.format = detect_ordinary_archive_format(leaf, raw);
    if (catalog.format == OrdinaryArchiveFormat::VolumeArchiveCkArv01) {
        catalog.error_message = "File is a CKARV01 volume archive, not an ordinary ZIP/TAR archive.";
        return false;
    }
    if (catalog.format == OrdinaryArchiveFormat::Unknown) {
        catalog.error_message = "Unrecognized or unsupported archive format.";
        return false;
    }
    if (catalog.format == OrdinaryArchiveFormat::TarGz) {
#if defined(CKTOOLS_HAS_ZLIB)
        if (!zlib_gunzip(raw, limits.max_total_bytes, limits.max_compression_ratio, backing_buf, err)) {
            catalog.error_message = err;
            return false;
        }
        return parse_tar_buffer(backing_buf, limits, members, catalog);
#else
        catalog.error_message = "Gzip-compressed TAR archives require zlib support in this build.";
        return false;
#endif
    }
    backing_buf = std::move(raw);
    if (catalog.format == OrdinaryArchiveFormat::Tar)
        return parse_tar_buffer(backing_buf, limits, members, catalog);
    if (catalog.format == OrdinaryArchiveFormat::Zip)
        return parse_zip_buffer(backing_buf, limits, members, catalog);
    catalog.error_message = "Unsupported archive format.";
    return false;
}

std::optional<ck::files::NativeName> native_name_from_utf8(ck::files::NameEncoding enc,
                                                           std::string_view part) {
    return detail::native_name_from_utf8(enc, part);
}

} // namespace

std::uint32_t ordinary_archive_crc32(std::span<const std::byte> data) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::byte b : data) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<unsigned char>(b));
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = (crc & 1U) ? 0xEDB88320U : 0U;
            crc = (crc >> 1U) ^ mask;
        }
    }
    return ~crc;
}

bool validate_ordinary_archive_entry_path(std::string_view entry_path,
                                          std::string *reason_out) {
    if (entry_path.empty()) {
        if (reason_out) *reason_out = "Empty archive entry path.";
        return false;
    }
    if (entry_path.front() == '/' || entry_path.front() == '\\') {
        if (reason_out) *reason_out = "Absolute path is forbidden in archive entry.";
        return false;
    }
    if (entry_path.size() >= 2 && std::isalpha(static_cast<unsigned char>(entry_path[0])) &&
        entry_path[1] == ':') {
        if (reason_out) *reason_out = "Drive-letter path is forbidden in archive entry.";
        return false;
    }
    for (const char ch : entry_path) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u == 0 || u < 0x20U || ch == '\\') {
            if (reason_out) *reason_out = "Control character, NUL, or backslash forbidden in archive entry.";
            return false;
        }
    }
    std::size_t pos = 0;
    while (pos <= entry_path.size()) {
        const std::size_t slash = entry_path.find('/', pos);
        const std::string_view seg = slash == std::string_view::npos
            ? entry_path.substr(pos)
            : entry_path.substr(pos, slash - pos);
        if (seg.empty() || seg == "." || seg == "..") {
            if (reason_out) *reason_out = "Path traversal or empty segment forbidden in archive entry.";
            return false;
        }
        if (slash == std::string_view::npos)
            break;
        pos = slash + 1;
    }
    return true;
}

bool validate_ordinary_archive_link_target(std::string_view entry_path,
                                           std::string_view link_target,
                                           std::string *reason_out) {
    if (link_target.empty()) {
        if (reason_out) *reason_out = "Empty link target.";
        return false;
    }
    if (link_target.front() == '/' || link_target.front() == '\\' ||
        (link_target.size() >= 2 && std::isalpha(static_cast<unsigned char>(link_target[0])) &&
         link_target[1] == ':')) {
        if (reason_out) *reason_out = "Absolute link target escapes archive extraction root.";
        return false;
    }
    // Count parent depth of entry_path.
    int depth = 0;
    for (const char ch : entry_path) {
        if (ch == '/')
            ++depth;
    }
    int cur = depth;
    std::size_t pos = 0;
    while (pos <= link_target.size()) {
        const std::size_t slash = link_target.find('/', pos);
        const std::string_view seg = slash == std::string_view::npos
            ? link_target.substr(pos)
            : link_target.substr(pos, slash - pos);
        if (seg == "..") {
            --cur;
            if (cur < 0) {
                if (reason_out) *reason_out = "Relative link target escapes archive extraction root.";
                return false;
            }
        } else if (!seg.empty() && seg != ".") {
            ++cur;
        }
        if (slash == std::string_view::npos)
            break;
        pos = slash + 1;
    }
    return true;
}

OrdinaryArchiveFormat detect_ordinary_archive_format(
    std::string_view filename_hint,
    std::span<const std::byte> magic_prefix) noexcept {
    // Check CKARV01 magic first so it is never confused with ZIP or TAR.
    if (magic_prefix.size() >= 8 &&
        std::memcmp(magic_prefix.data(), "CKARV01\0", 8) == 0)
        return OrdinaryArchiveFormat::VolumeArchiveCkArv01;
    const std::string lower = ascii_lower_str(filename_hint);
    if (lower.ends_with(".ckarchive"))
        return OrdinaryArchiveFormat::VolumeArchiveCkArv01;

    if (magic_prefix.size() >= 4) {
        const std::uint32_t sig = read_u32_le(magic_prefix.data());
        if (sig == kZipLocalSig || sig == kZipEocdSig || sig == 0x08074B50U)
            return OrdinaryArchiveFormat::Zip;
    }
    if (magic_prefix.size() >= 2 &&
        std::to_integer<unsigned char>(magic_prefix[0]) == 0x1FU &&
        std::to_integer<unsigned char>(magic_prefix[1]) == 0x8BU)
        return OrdinaryArchiveFormat::TarGz;
    if (magic_prefix.size() >= 263 &&
        std::memcmp(magic_prefix.data() + 257, "ustar", 5) == 0)
        return OrdinaryArchiveFormat::Tar;

    if (lower.ends_with(".zip"))
        return OrdinaryArchiveFormat::Zip;
    if (lower.ends_with(".tar.gz") || lower.ends_with(".tgz"))
        return OrdinaryArchiveFormat::TarGz;
    if (lower.ends_with(".tar"))
        return OrdinaryArchiveFormat::Tar;
    return OrdinaryArchiveFormat::Unknown;
}

OrdinaryArchiveCatalog list_ordinary_archive(
    const ck::files::NativeAbsolutePath &archive_path,
    const OrdinaryArchiveLimits &limits,
    std::stop_token stop_token) {
    OrdinaryArchiveCatalog catalog;
    std::vector<std::byte> backing;
    std::vector<ParsedMember> members;
    (void)load_and_parse_archive(archive_path, limits, backing, members, catalog, stop_token);
    return catalog;
}

std::optional<std::vector<std::byte>> read_ordinary_archive_entry(
    const ck::files::NativeAbsolutePath &archive_path,
    std::size_t entry_index,
    std::string *error_out,
    const OrdinaryArchiveLimits &limits,
    std::stop_token stop_token) {
    OrdinaryArchiveCatalog catalog;
    std::vector<std::byte> backing;
    std::vector<ParsedMember> members;
    if (!load_and_parse_archive(archive_path, limits, backing, members, catalog, stop_token)) {
        if (error_out) *error_out = catalog.error_message;
        return std::nullopt;
    }
    if (entry_index >= members.size()) {
        if (error_out) *error_out = "Archive entry index out of range.";
        return std::nullopt;
    }
    if (!members[entry_index].entry.safe_path) {
        if (error_out) *error_out = members[entry_index].entry.safety_warning;
        return std::nullopt;
    }
    if (members[entry_index].entry.kind != OrdinaryArchiveEntryKind::RegularFile) {
        if (error_out) *error_out = "Archive entry is not a regular file.";
        return std::nullopt;
    }
    std::vector<std::byte> out;
    std::string err;
    if (!decode_member_bytes(catalog.format, backing, members[entry_index], limits, out, err)) {
        if (error_out) *error_out = err;
        return std::nullopt;
    }
    return out;
}

OrdinaryArchiveExtractResult extract_ordinary_archive(
    const ck::files::NativeAbsolutePath &archive_path,
    const ck::files::NativeAbsolutePath &destination_dir,
    std::span<const std::size_t> selected_indices,
    bool allow_overwrite,
    const OrdinaryArchiveLimits &limits,
    std::stop_token stop_token) {
    OrdinaryArchiveExtractResult result;
    if (!destination_dir.is_valid()) {
        result.error_message = "Invalid extraction destination directory.";
        return result;
    }
    OrdinaryArchiveCatalog catalog;
    std::vector<std::byte> backing;
    std::vector<ParsedMember> members;
    if (!load_and_parse_archive(archive_path, limits, backing, members, catalog, stop_token)) {
        result.error_message = catalog.error_message;
        return result;
    }
    if (catalog.has_collisions && !allow_overwrite) {
        result.error_message = "Archive contains colliding member names; extraction refused without explicit overwrite.";
        return result;
    }

    std::set<std::size_t> wanted(selected_indices.begin(), selected_indices.end());
    ck::files::NativeFileSystem fs;
    auto root_check = fs.open_native_directory(destination_dir);
    if (!root_check.value()) {
        result.error_message = "Unable to open extraction destination directory.";
        return result;
    }

    for (const auto &member : members) {
        if (stop_token.stop_requested()) {
            result.cancelled = true;
            return result;
        }
        if (!wanted.empty() && !wanted.contains(member.entry.index))
            continue;
        if (!member.entry.safe_path) {
            ++result.rejected_unsafe;
            continue;
        }
        if (member.entry.kind != OrdinaryArchiveEntryKind::RegularFile &&
            member.entry.kind != OrdinaryArchiveEntryKind::Directory) {
            ++result.rejected_unsafe;
            continue;
        }

        // Split validated relative path into native components.
        std::vector<ck::files::NativeName> parts;
        std::size_t pos = 0;
        bool parts_ok = true;
        while (pos <= member.entry.path.size()) {
            const std::size_t slash = member.entry.path.find('/', pos);
            const std::string_view seg = slash == std::string_view::npos
                ? std::string_view(member.entry.path).substr(pos)
                : std::string_view(member.entry.path).substr(pos, slash - pos);
            auto nn = native_name_from_utf8(destination_dir.encoding(), seg);
            if (!nn) {
                parts_ok = false;
                break;
            }
            parts.push_back(std::move(*nn));
            if (slash == std::string_view::npos)
                break;
            pos = slash + 1;
        }
        if (!parts_ok || parts.empty()) {
            ++result.rejected_unsafe;
            continue;
        }

        // Ensure intermediate directories exist.
        ck::files::NativeAbsolutePath cur = destination_dir;
        const std::size_t dir_count =
            member.entry.kind == OrdinaryArchiveEntryKind::Directory ? parts.size() : parts.size() - 1;
        bool dir_ok = true;
        for (std::size_t i = 0; i < dir_count; ++i) {
            auto dir = fs.open_native_directory(cur);
            if (!dir.value()) {
                dir_ok = false;
                break;
            }
            (void)(*dir.value())->create_child_directory(parts[i]);
            auto next = cur.child_address(parts[i]);
            if (!next.value()) {
                dir_ok = false;
                break;
            }
            cur = *next.value();
        }
        if (!dir_ok) {
            result.error_message = "Failed creating extraction subdirectory.";
            return result;
        }
        if (member.entry.kind == OrdinaryArchiveEntryKind::Directory) {
            ++result.extracted_directories;
            continue;
        }

        auto target_path = cur.child_address(parts.back());
        if (!target_path.value()) {
            ++result.rejected_unsafe;
            continue;
        }

        std::vector<std::byte> payload;
        std::string err;
        if (!decode_member_bytes(catalog.format, backing, member, limits, payload, err)) {
            result.error_message = err;
            return result;
        }
        if (!write_native_file_staged(*target_path.value(), payload, allow_overwrite, err, stop_token)) {
            if (!allow_overwrite && err.find("already exists") != std::string::npos) {
                ++result.skipped_conflicts;
                continue;
            }
            result.error_message = err;
            return result;
        }
        ++result.extracted_files;
    }
    return result;
}

OrdinaryArchiveMutationResult create_ordinary_archive(
    const ck::files::NativeAbsolutePath &archive_path,
    OrdinaryArchiveFormat format,
    std::span<const OrdinaryArchiveInputItem> items,
    bool compress_zip_deflate,
    const OrdinaryArchiveLimits &limits,
    std::stop_token stop_token) {
    OrdinaryArchiveMutationResult result;
    if (format != OrdinaryArchiveFormat::Zip &&
        format != OrdinaryArchiveFormat::Tar &&
        format != OrdinaryArchiveFormat::TarGz) {
        result.error_message = "Unsupported output archive format.";
        return result;
    }
    if (items.size() > limits.max_entries) {
        result.error_message = "Input item count exceeds archive limit.";
        return result;
    }

    std::vector<std::pair<std::string, std::pair<bool, std::vector<std::byte>>>> collected;
    collected.reserve(items.size());
    std::uint64_t total_bytes = 0;

    for (const auto &item : items) {
        if (stop_token.stop_requested()) {
            result.cancelled = true;
            return result;
        }
        std::string reason;
        if (!validate_ordinary_archive_entry_path(item.archive_path, &reason)) {
            result.error_message = "Invalid archive member path '" + item.archive_path + "': " + reason;
            return result;
        }
        std::vector<std::byte> bytes;
        if (!item.is_directory) {
            if (!item.source_path) {
                result.error_message = "Missing source path for archive entry.";
                return result;
            }
            std::string err;
            if (!read_native_file_all(*item.source_path, limits.max_entry_bytes, bytes, err, stop_token)) {
                result.error_message = err;
                return result;
            }
            if (total_bytes + bytes.size() > limits.max_total_bytes) {
                result.error_message = "Total input size exceeds archive limit.";
                return result;
            }
            total_bytes += bytes.size();
        }
        collected.emplace_back(item.archive_path, std::make_pair(item.is_directory, std::move(bytes)));
    }

    std::vector<std::byte> archive_bytes;
    if (format == OrdinaryArchiveFormat::Zip) {
        build_zip_buffer(collected, compress_zip_deflate, archive_bytes);
    } else {
        std::vector<std::byte> tar_buf;
        for (const auto &[path, body] : collected)
            append_tar_entry(tar_buf, path, body.first, body.second);
        tar_buf.insert(tar_buf.end(), 1024, std::byte{0}); // Two 512-byte EOF blocks.
        if (format == OrdinaryArchiveFormat::TarGz) {
#if defined(CKTOOLS_HAS_ZLIB)
            if (!zlib_gzip(tar_buf, archive_bytes)) {
                result.error_message = "Failed gzip-compressing TAR archive.";
                return result;
            }
#else
            result.error_message = "TAR.GZ creation requires zlib support in this build.";
            return result;
#endif
        } else {
            archive_bytes = std::move(tar_buf);
        }
    }

    std::string err;
    if (!write_native_file_staged(archive_path, archive_bytes, false, err, stop_token)) {
        result.error_message = err;
        return result;
    }
    result.ok = true;
    result.entries_written = collected.size();
    result.archive_bytes = archive_bytes.size();
    return result;
}

OrdinaryArchiveMutationResult modify_ordinary_archive(
    const ck::files::NativeAbsolutePath &archive_path,
    std::span<const OrdinaryArchiveInputItem> add_items,
    std::span<const std::size_t> remove_indices,
    const OrdinaryArchiveLimits &limits,
    std::stop_token stop_token) {
    OrdinaryArchiveMutationResult result;
    OrdinaryArchiveCatalog catalog;
    std::vector<std::byte> backing;
    std::vector<ParsedMember> members;
    if (!load_and_parse_archive(archive_path, limits, backing, members, catalog, stop_token)) {
        result.error_message = catalog.error_message;
        return result;
    }

    std::set<std::size_t> to_remove(remove_indices.begin(), remove_indices.end());
    std::set<std::string> replacing_paths;
    for (const auto &item : add_items)
        replacing_paths.insert(item.archive_path);

    std::vector<std::pair<std::string, std::pair<bool, std::vector<std::byte>>>> rebuilt;
    for (const auto &member : members) {
        if (stop_token.stop_requested()) {
            result.cancelled = true;
            return result;
        }
        if (to_remove.contains(member.entry.index) || replacing_paths.contains(member.entry.path))
            continue;
        if (!member.entry.safe_path)
            continue;
        if (member.entry.kind == OrdinaryArchiveEntryKind::Directory) {
            rebuilt.emplace_back(member.entry.path, std::make_pair(true, std::vector<std::byte>{}));
        } else if (member.entry.kind == OrdinaryArchiveEntryKind::RegularFile) {
            std::vector<std::byte> bytes;
            std::string err;
            if (!decode_member_bytes(catalog.format, backing, member, limits, bytes, err)) {
                result.error_message = err;
                return result;
            }
            rebuilt.emplace_back(member.entry.path, std::make_pair(false, std::move(bytes)));
        }
    }

    for (const auto &item : add_items) {
        if (stop_token.stop_requested()) {
            result.cancelled = true;
            return result;
        }
        std::string reason;
        if (!validate_ordinary_archive_entry_path(item.archive_path, &reason)) {
            result.error_message = "Invalid archive member path: " + reason;
            return result;
        }
        std::vector<std::byte> bytes;
        if (!item.is_directory) {
            if (!item.source_path) {
                result.error_message = "Missing source path for archive entry.";
                return result;
            }
            std::string err;
            if (!read_native_file_all(*item.source_path, limits.max_entry_bytes, bytes, err, stop_token)) {
                result.error_message = err;
                return result;
            }
        }
        rebuilt.emplace_back(item.archive_path, std::make_pair(item.is_directory, std::move(bytes)));
    }

    std::vector<std::byte> out_bytes;
    if (catalog.format == OrdinaryArchiveFormat::Zip) {
        build_zip_buffer(rebuilt, true, out_bytes);
    } else {
        std::vector<std::byte> tar_buf;
        for (const auto &[path, body] : rebuilt)
            append_tar_entry(tar_buf, path, body.first, body.second);
        tar_buf.insert(tar_buf.end(), 1024, std::byte{0});
        if (catalog.format == OrdinaryArchiveFormat::TarGz) {
#if defined(CKTOOLS_HAS_ZLIB)
            if (!zlib_gzip(tar_buf, out_bytes)) {
                result.error_message = "Failed gzip-compressing updated TAR archive.";
                return result;
            }
#else
            result.error_message = "TAR.GZ update requires zlib support in this build.";
            return result;
#endif
        } else {
            out_bytes = std::move(tar_buf);
        }
    }

    std::string err;
    if (!write_native_file_staged(archive_path, out_bytes, true, err, stop_token)) {
        result.error_message = err;
        return result;
    }
    result.ok = true;
    result.entries_written = rebuilt.size();
    result.archive_bytes = out_bytes.size();
    return result;
}

} // namespace ck::vision
