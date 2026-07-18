// sklad — an embedded LSM-tree key-value storage engine for C++17.
// SPDX-License-Identifier: MIT
//
// db.hpp — the public database handle. A sklad::db is an ordered, persistent
// key/value store: writes go to a write-ahead log and an in-memory MemTable,
// which is flushed to immutable SSTables and periodically compacted. All
// methods are safe to call from multiple threads.
#ifndef SKLAD_DB_HPP
#define SKLAD_DB_HPP

#include <memory>
#include <string>
#include <string_view>

#include "sklad/iterator.hpp"
#include "sklad/options.hpp"
#include "sklad/status.hpp"
#include "sklad/write_batch.hpp"

namespace sklad {

class db {
public:
    virtual ~db() = default;

    /// Open (or create) the database stored in directory `path`. On success
    /// `*out` owns the handle. Replays the write-ahead log to recover any
    /// writes not yet flushed at the last shutdown or crash.
    static status open(const options& opts, const std::string& path,
                       std::unique_ptr<db>* out);

    /// Insert or overwrite a key.
    virtual status put(std::string_view key, std::string_view value,
                       const write_options& = {}) = 0;

    /// Delete a key (no-op if absent).
    virtual status del(std::string_view key, const write_options& = {}) = 0;

    /// Look up a key. Returns not_found if the key is absent or deleted.
    virtual status get(std::string_view key, std::string* value) = 0;

    /// True if the key exists (and is not deleted).
    virtual bool contains(std::string_view key) = 0;

    /// Apply a batch atomically and durably.
    virtual status write(const write_batch& batch, const write_options& = {}) = 0;

    /// A snapshot iterator over the whole key space, in key order.
    virtual std::unique_ptr<iterator> new_iterator() = 0;

    /// Force the current MemTable out to an SSTable.
    virtual status flush() = 0;

    /// Merge all SSTables into one, dropping overwritten values and tombstones.
    virtual status compact() = 0;

    /// Approximate number of live SSTable files (useful for tests/metrics).
    virtual std::size_t sstable_count() = 0;
};

}  // namespace sklad

#endif  // SKLAD_DB_HPP
