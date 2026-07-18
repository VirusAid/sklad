// sklad — internal: the in-memory write buffer. SPDX-License-Identifier: MIT
//
// A MemTable holds recent writes in a sorted map. Reads consult it before any
// on-disk SSTable, so it always reflects the newest value (or a tombstone) for
// a key. When it grows past the configured size it is flushed to an SSTable.
#ifndef SKLAD_MEMTABLE_HPP
#define SKLAD_MEMTABLE_HPP

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

namespace sklad {

/// Whether a stored entry is a live value or a deletion marker (tombstone).
enum class value_kind : unsigned char { value = 1, tombstone = 0 };

class mem_table {
public:
    struct entry {
        value_kind kind;
        std::string value;  // empty when kind == tombstone
    };
    using map_type = std::map<std::string, entry, std::less<>>;

    void put(std::string_view key, std::string_view value) {
        apply(key, value_kind::value, value);
    }
    void del(std::string_view key) { apply(key, value_kind::tombstone, {}); }

    enum class result { not_present, found_value, found_tombstone };

    /// Look up the newest state of a key held by this MemTable.
    result get(std::string_view key, std::string* value) const {
        auto it = map_.find(key);
        if (it == map_.end()) return result::not_present;
        if (it->second.kind == value_kind::tombstone) return result::found_tombstone;
        if (value) *value = it->second.value;
        return result::found_value;
    }

    std::size_t approx_bytes() const noexcept { return bytes_; }
    bool empty() const noexcept { return map_.empty(); }
    std::size_t size() const noexcept { return map_.size(); }
    const map_type& data() const noexcept { return map_; }

    void clear() {
        map_.clear();
        bytes_ = 0;
    }

private:
    void apply(std::string_view key, value_kind kind, std::string_view value) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            // Rough accounting: key + value + fixed overhead per entry.
            bytes_ += key.size() + value.size() + 16;
            map_.emplace(std::string(key), entry{kind, std::string(value)});
        } else {
            bytes_ -= it->second.value.size();
            bytes_ += value.size();
            it->second.kind = kind;
            it->second.value.assign(value.data(), value.size());
        }
    }

    map_type map_;
    std::size_t bytes_ = 0;
};

}  // namespace sklad

#endif  // SKLAD_MEMTABLE_HPP
