// sklad — internal: CRC32 implementation. SPDX-License-Identifier: MIT
#include "coding.hpp"

#include <array>

namespace sklad {

namespace {
// Precomputed reflected CRC32 (polynomial 0xEDB88820) lookup table.
struct crc_table {
    std::array<std::uint32_t, 256> t{};
    crc_table() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88820u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
    }
};
const crc_table g_crc;
}  // namespace

std::uint32_t crc32(const char* data, std::size_t len) {
    std::uint32_t c = 0xFFFFFFFFu;
    const auto* p = reinterpret_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < len; ++i)
        c = g_crc.t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

}  // namespace sklad
