// sklad — internal: byte encoding helpers and CRC32. SPDX-License-Identifier: MIT
#ifndef SKLAD_CODING_HPP
#define SKLAD_CODING_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace sklad {

// --- fixed-width little-endian --------------------------------------------

inline void put_fixed32(std::string* dst, std::uint32_t v) {
    char b[4];
    b[0] = static_cast<char>(v & 0xff);
    b[1] = static_cast<char>((v >> 8) & 0xff);
    b[2] = static_cast<char>((v >> 16) & 0xff);
    b[3] = static_cast<char>((v >> 24) & 0xff);
    dst->append(b, 4);
}

inline void put_fixed64(std::string* dst, std::uint64_t v) {
    char b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<char>((v >> (8 * i)) & 0xff);
    dst->append(b, 8);
}

inline std::uint32_t decode_fixed32(const char* p) {
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return std::uint32_t(u[0]) | (std::uint32_t(u[1]) << 8) |
           (std::uint32_t(u[2]) << 16) | (std::uint32_t(u[3]) << 24);
}

inline std::uint64_t decode_fixed64(const char* p) {
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    std::uint64_t r = 0;
    for (int i = 0; i < 8; ++i) r |= std::uint64_t(u[i]) << (8 * i);
    return r;
}

// --- varint ----------------------------------------------------------------

inline void put_varint32(std::string* dst, std::uint32_t v) {
    while (v >= 0x80) {
        dst->push_back(static_cast<char>((v & 0x7f) | 0x80));
        v >>= 7;
    }
    dst->push_back(static_cast<char>(v));
}

inline void put_varint64(std::string* dst, std::uint64_t v) {
    while (v >= 0x80) {
        dst->push_back(static_cast<char>((v & 0x7f) | 0x80));
        v >>= 7;
    }
    dst->push_back(static_cast<char>(v));
}

/// Decode a varint from [*p, limit). On success advances *p and returns true.
inline bool get_varint64(const char** p, const char* limit, std::uint64_t* out) {
    std::uint64_t result = 0;
    int shift = 0;
    const char* q = *p;
    while (q < limit && shift <= 63) {
        auto byte = static_cast<unsigned char>(*q++);
        result |= std::uint64_t(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            *p = q;
            *out = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

inline bool get_varint32(const char** p, const char* limit, std::uint32_t* out) {
    std::uint64_t v = 0;
    if (!get_varint64(p, limit, &v) || v > 0xffffffffULL) return false;
    *out = static_cast<std::uint32_t>(v);
    return true;
}

/// Append a length-prefixed byte string.
inline void put_length_prefixed(std::string* dst, std::string_view s) {
    put_varint32(dst, static_cast<std::uint32_t>(s.size()));
    dst->append(s.data(), s.size());
}

/// Read a length-prefixed byte string from [*p, limit).
inline bool get_length_prefixed(const char** p, const char* limit,
                                std::string_view* out) {
    std::uint32_t len = 0;
    if (!get_varint32(p, limit, &len)) return false;
    if (static_cast<std::uint64_t>(limit - *p) < len) return false;
    *out = std::string_view(*p, len);
    *p += len;
    return true;
}

// --- CRC32 (IEEE 802.3, reflected) ----------------------------------------

std::uint32_t crc32(const char* data, std::size_t len);
inline std::uint32_t crc32(std::string_view s) { return crc32(s.data(), s.size()); }

}  // namespace sklad

#endif  // SKLAD_CODING_HPP
