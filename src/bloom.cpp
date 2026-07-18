// sklad — internal: bloom filter implementation. SPDX-License-Identifier: MIT
#include "bloom.hpp"

#include <cmath>
#include <cstdint>

namespace sklad {

namespace {
// A small, fast 32-bit hash (FNV-1a) used as the base for double hashing.
std::uint32_t hash32(std::string_view s) {
    std::uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}
}  // namespace

std::string bloom::build(const std::vector<std::string_view>& keys, int bits_per_key) {
    std::string out;
    if (bits_per_key <= 0) return out;

    // Choose the number of hash functions k that minimizes false positives.
    int k = static_cast<int>(bits_per_key * 0.69);  // ln(2) ≈ 0.69
    if (k < 1) k = 1;
    if (k > 30) k = 30;

    std::size_t bits = keys.size() * static_cast<std::size_t>(bits_per_key);
    if (bits < 64) bits = 64;  // avoid tiny, dense filters
    const std::size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;

    out.assign(bytes, '\0');
    for (std::string_view key : keys) {
        std::uint32_t h = hash32(key);
        const std::uint32_t delta = (h >> 17) | (h << 15);  // rotate right 17
        for (int j = 0; j < k; ++j) {
            const std::size_t pos = h % bits;
            out[pos / 8] = static_cast<char>(
                static_cast<unsigned char>(out[pos / 8]) | (1u << (pos % 8)));
            h += delta;
        }
    }
    out.push_back(static_cast<char>(k));  // remember k in the trailing byte
    return out;
}

bool bloom::may_contain(std::string_view filter, std::string_view key) {
    if (filter.size() < 2) return true;  // empty/invalid: never skip
    const std::size_t bytes = filter.size() - 1;
    const std::size_t bits = bytes * 8;
    const int k = static_cast<unsigned char>(filter[filter.size() - 1]);
    if (k < 1 || k > 30) return true;  // corrupt header: be safe

    std::uint32_t h = hash32(key);
    const std::uint32_t delta = (h >> 17) | (h << 15);
    for (int j = 0; j < k; ++j) {
        const std::size_t pos = h % bits;
        if ((static_cast<unsigned char>(filter[pos / 8]) & (1u << (pos % 8))) == 0)
            return false;
        h += delta;
    }
    return true;
}

}  // namespace sklad
