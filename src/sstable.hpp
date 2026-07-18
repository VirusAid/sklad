// sklad — internal: on-disk sorted string tables. SPDX-License-Identifier: MIT
//
// An SSTable is an immutable file of key/value entries sorted by key. File
// layout:
//   [data]   entries in ascending key order, each:
//              [kind:1][klen varint][key][vlen varint][value]
//            kind = 1 value, 0 tombstone (a recorded deletion).
//   [bloom]  serialized bloom filter over all keys (may be empty)
//   [index]  [count fixed32] then count * { klen varint, key, offset fixed64 }
//   [footer] bloom_off, bloom_size, index_off, index_size, magic (5 x fixed64)
//
// The reader loads the whole file into memory, so lookups and iteration are
// pure in-memory parsing with no per-entry disk seeks.
#ifndef SKLAD_SSTABLE_HPP
#define SKLAD_SSTABLE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "memtable.hpp"
#include "sklad/status.hpp"

namespace sklad {

class sstable_builder {
public:
    /// Append an entry. Keys MUST be added in strictly ascending order.
    void add(std::string_view key, value_kind kind, std::string_view value);

    std::size_t entry_count() const noexcept { return keys_.size(); }

    /// Serialize and write the table to `path`, building a bloom filter with
    /// `bloom_bits_per_key` bits per key (0 disables it). fsyncs the file.
    status finish(const std::string& path, int bloom_bits_per_key);

private:
    std::string data_;
    std::vector<std::string> keys_;
    std::vector<std::uint64_t> offsets_;
};

class sstable_reader {
public:
    /// Load and validate an SSTable file.
    status open(const std::string& path);

    enum class lookup { absent, value, tombstone };

    /// Look up `key` within this table only. `absent` means the table does not
    /// mention the key at all (an older table may still have it).
    lookup get(std::string_view key, std::string* value) const;

    // --- sequential access for the merging iterator ---
    std::size_t data_size() const noexcept { return data_size_; }
    /// Data offset of the first entry whose key >= target, or data_size() if
    /// none. Entries after it are in ascending key order.
    std::size_t seek_offset(std::string_view target) const;
    /// Parse the entry at byte offset `off`. On success fills the views (which
    /// point into this reader's buffer) and sets `*next` to the following
    /// offset. Returns false at/after the end of the data section.
    bool parse_entry(std::size_t off, std::string_view* key, value_kind* kind,
                     std::string_view* value, std::size_t* next) const;

    std::size_t key_count() const noexcept { return index_.size(); }

private:
    std::string file_;       // entire file contents in memory
    std::size_t data_size_ = 0;
    std::string_view bloom_;
    // Sorted (key, data-offset) pairs; views point into file_.
    std::vector<std::pair<std::string_view, std::uint64_t>> index_;
};

}  // namespace sklad

#endif  // SKLAD_SSTABLE_HPP
