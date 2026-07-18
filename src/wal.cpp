// sklad — internal: write-ahead log implementation. SPDX-License-Identifier: MIT
#include "wal.hpp"

#include <cstdint>

#include "coding.hpp"

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace sklad {

status fsync_file(std::FILE* f) {
    if (!f) return status::io_error("fsync: null file");
    if (std::fflush(f) != 0) return status::io_error("fflush failed");
#if defined(_WIN32)
    int fd = _fileno(f);
    if (fd >= 0) {
        HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        if (h != INVALID_HANDLE_VALUE && !::FlushFileBuffers(h))
            return status::io_error("FlushFileBuffers failed");
    }
#else
    int fd = ::fileno(f);
    if (fd >= 0 && ::fsync(fd) != 0) return status::io_error("fsync failed");
#endif
    return status::ok();
}

// --- writer ---------------------------------------------------------------

status wal_writer::open(const std::string& path) {
    close();
    f_ = std::fopen(path.c_str(), "ab");
    if (!f_) return status::io_error("cannot open WAL for append: " + path);
    return status::ok();
}

status wal_writer::open_truncate(const std::string& path) {
    close();
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return status::io_error("cannot truncate WAL: " + path);
    return status::ok();
}

status wal_writer::add_record(std::string_view payload, bool sync) {
    if (!f_) return status::io_error("WAL not open");
    std::string header;
    put_fixed32(&header, static_cast<std::uint32_t>(payload.size()));
    put_fixed32(&header, crc32(payload));
    if (std::fwrite(header.data(), 1, header.size(), f_) != header.size())
        return status::io_error("WAL header write failed");
    if (!payload.empty() &&
        std::fwrite(payload.data(), 1, payload.size(), f_) != payload.size())
        return status::io_error("WAL payload write failed");
    if (sync) return fsync_file(f_);
    return status::ok();
}

status wal_writer::sync() { return f_ ? fsync_file(f_) : status::ok(); }

void wal_writer::close() {
    if (f_) {
        std::fclose(f_);
        f_ = nullptr;
    }
}

// --- reader ---------------------------------------------------------------

status wal_reader::open(const std::string& path) {
    close();
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_) return status::io_error("cannot open WAL for read: " + path);
    return status::ok();
}

status wal_reader::read_record(std::string* out, bool* eof) {
    *eof = false;
    char hdr[8];
    std::size_t got = std::fread(hdr, 1, 8, f_);
    if (got == 0) {  // clean end of file
        *eof = true;
        return status::ok();
    }
    if (got < 8) {  // torn header from a crash: stop cleanly
        *eof = true;
        return status::ok();
    }
    const std::uint32_t len = decode_fixed32(hdr);
    const std::uint32_t want_crc = decode_fixed32(hdr + 4);

    out->resize(len);
    if (len > 0 && std::fread(out->data(), 1, len, f_) != len) {
        *eof = true;  // truncated payload from a crash
        return status::ok();
    }
    if (crc32(std::string_view(out->data(), len)) != want_crc) {
        *eof = true;  // corrupt trailing record from a crash
        return status::ok();
    }
    return status::ok();
}

void wal_reader::close() {
    if (f_) {
        std::fclose(f_);
        f_ = nullptr;
    }
}

}  // namespace sklad
