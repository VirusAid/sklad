// sklad — an embedded LSM-tree key-value storage engine for C++17.
// SPDX-License-Identifier: MIT
//
// options.hpp — knobs for opening and tuning a database.
#ifndef SKLAD_OPTIONS_HPP
#define SKLAD_OPTIONS_HPP

#include <cstddef>

namespace sklad {

struct options {
    /// Create the database directory if it does not exist.
    bool create_if_missing = true;

    /// Fail to open if the database directory already exists.
    bool error_if_exists = false;

    /// When the in-memory MemTable grows past this many bytes, it is flushed to
    /// a new immutable SSTable on disk. Larger = fewer, bigger files.
    std::size_t memtable_bytes = 4 * 1024 * 1024;  // 4 MiB

    /// Trigger automatic compaction once this many SSTables accumulate. Fewer
    /// files = faster reads, more compaction work.
    std::size_t compaction_trigger = 8;

    /// Bits per key in the SSTable bloom filter. ~10 bits ≈ 1% false-positive
    /// rate. 0 disables the bloom filter.
    int bloom_bits_per_key = 10;

    /// fsync the write-ahead log on every write. Safer across power loss, but
    /// much slower. When false, data is flushed to the OS but may be lost on a
    /// hard crash within the last few writes.
    bool sync_writes = false;
};

/// Per-write overrides.
struct write_options {
    bool sync = false;
};

}  // namespace sklad

#endif  // SKLAD_OPTIONS_HPP
