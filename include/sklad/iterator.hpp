// sklad — an embedded LSM-tree key-value storage engine for C++17.
// SPDX-License-Identifier: MIT
//
// iterator.hpp — an ordered cursor over the live key/value pairs. Keys are
// visited in ascending lexicographic order; deleted keys and shadowed older
// versions are skipped. The iterator reflects a consistent snapshot taken when
// it was created.
#ifndef SKLAD_ITERATOR_HPP
#define SKLAD_ITERATOR_HPP

#include <string>
#include <string_view>

namespace sklad {

class iterator {
public:
    virtual ~iterator() = default;

    /// Position at the first key >= target.
    virtual void seek(std::string_view target) = 0;
    /// Position at the first key.
    virtual void seek_to_first() = 0;
    /// True while positioned at a valid entry.
    virtual bool valid() const = 0;
    /// Advance to the next key.
    virtual void next() = 0;

    /// Current key/value. Valid only while valid() is true.
    virtual std::string_view key() const = 0;
    virtual std::string_view value() const = 0;
};

}  // namespace sklad

#endif  // SKLAD_ITERATOR_HPP
