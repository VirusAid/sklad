// sklad — internal: the write-ahead log. SPDX-License-Identifier: MIT
//
// Every write is appended here before it touches the MemTable, so a crash can
// be recovered by replaying the log. Each record is framed as
//   [payload_len : fixed32][crc32(payload) : fixed32][payload bytes]
// and the CRC lets recovery detect a torn final write from a crash and stop
// cleanly at the last intact record.
#ifndef SKLAD_WAL_HPP
#define SKLAD_WAL_HPP

#include <cstdio>
#include <string>
#include <string_view>

#include "sklad/status.hpp"

namespace sklad {

class wal_writer {
public:
    ~wal_writer() { close(); }

    /// Open `path` for appending (creating it if needed).
    status open(const std::string& path);

    /// Open `path` truncated to empty (used to rotate the log after a flush).
    status open_truncate(const std::string& path);

    /// Append one framed record. When `sync` is true, force it to stable
    /// storage before returning.
    status add_record(std::string_view payload, bool sync);

    /// Flush buffered data to the OS and, on request, to the physical device.
    status sync();

    void close();

private:
    std::FILE* f_ = nullptr;
};

class wal_reader {
public:
    ~wal_reader() { close(); }

    status open(const std::string& path);

    /// Read the next record into `*out`. Sets `*eof` at a clean end of file. A
    /// truncated or CRC-mismatching trailing record is reported as a clean EOF
    /// (a crash during the last write), not a hard error.
    status read_record(std::string* out, bool* eof);

    void close();

private:
    std::FILE* f_ = nullptr;
};

/// fsync-equivalent for a C FILE*: flushes userspace buffers and asks the OS to
/// commit them to the device.
status fsync_file(std::FILE* f);

}  // namespace sklad

#endif  // SKLAD_WAL_HPP
