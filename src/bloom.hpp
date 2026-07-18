// sklad — internal: bloom filter for fast "definitely absent" SSTable checks.
// SPDX-License-Identifier: MIT
//
// A bloom filter never yields a false negative: if it says a key is absent, the
// SSTable truly lacks it, so we skip reading the file. False positives are
// possible (a wasted lookup) but rare at ~10 bits/key.
#ifndef SKLAD_BLOOM_HPP
#define SKLAD_BLOOM_HPP

#include <string>
#include <string_view>
#include <vector>

namespace sklad {

class bloom {
public:
    /// Build a serialized filter covering `keys`. bits_per_key trades size for
    /// accuracy (~10 ≈ 1% false-positive rate). Returns an empty string if
    /// bits_per_key <= 0.
    static std::string build(const std::vector<std::string_view>& keys,
                             int bits_per_key);

    /// True if `key` may be present. An empty filter conservatively returns
    /// true (never skip).
    static bool may_contain(std::string_view filter, std::string_view key);
};

}  // namespace sklad

#endif  // SKLAD_BLOOM_HPP
