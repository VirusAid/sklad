// sklad — internal: SSTable builder and reader. SPDX-License-Identifier: MIT
#include "sstable.hpp"

#include <algorithm>
#include <cstdio>

#include "bloom.hpp"
#include "coding.hpp"
#include "wal.hpp"  // fsync_file

namespace sklad {

namespace {
constexpr std::uint64_t kMagic = 0x736B6C6164000001ULL;  // "sklad" + version
constexpr std::size_t kFooterSize = 5 * 8;               // 5 fixed64 fields
}  // namespace

// --- builder --------------------------------------------------------------

void sstable_builder::add(std::string_view key, value_kind kind,
                          std::string_view value) {
    offsets_.push_back(data_.size());
    keys_.emplace_back(key);
    data_.push_back(static_cast<char>(kind == value_kind::value ? 1 : 0));
    put_varint32(&data_, static_cast<std::uint32_t>(key.size()));
    data_.append(key.data(), key.size());
    if (kind == value_kind::value) {
        put_varint32(&data_, static_cast<std::uint32_t>(value.size()));
        data_.append(value.data(), value.size());
    } else {
        put_varint32(&data_, 0);
    }
}

status sstable_builder::finish(const std::string& path, int bloom_bits_per_key) {
    std::string file = data_;

    // Bloom block over all keys.
    const std::uint64_t bloom_off = file.size();
    std::vector<std::string_view> key_views;
    key_views.reserve(keys_.size());
    for (const auto& k : keys_) key_views.emplace_back(k);
    std::string bf = bloom::build(key_views, bloom_bits_per_key);
    file += bf;
    const std::uint64_t bloom_size = bf.size();

    // Index block: count + (key, offset) pairs.
    const std::uint64_t index_off = file.size();
    std::string idx;
    put_fixed32(&idx, static_cast<std::uint32_t>(keys_.size()));
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        put_varint32(&idx, static_cast<std::uint32_t>(keys_[i].size()));
        idx.append(keys_[i]);
        put_fixed64(&idx, offsets_[i]);
    }
    file += idx;
    const std::uint64_t index_size = idx.size();

    // Footer.
    put_fixed64(&file, bloom_off);
    put_fixed64(&file, bloom_size);
    put_fixed64(&file, index_off);
    put_fixed64(&file, index_size);
    put_fixed64(&file, kMagic);

    // Write to a temp file then atomically rename into place.
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return status::io_error("cannot create SSTable: " + tmp);
    if (std::fwrite(file.data(), 1, file.size(), f) != file.size()) {
        std::fclose(f);
        return status::io_error("SSTable write failed: " + tmp);
    }
    status s = fsync_file(f);
    std::fclose(f);
    if (!s) return s;

    std::remove(path.c_str());  // in case a stale target exists
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
        return status::io_error("SSTable rename failed: " + path);
    return status::ok();
}

// --- reader ---------------------------------------------------------------

status sstable_reader::open(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return status::io_error("cannot open SSTable: " + path);
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < static_cast<long>(kFooterSize)) {
        std::fclose(f);
        return status::corruption("SSTable too small: " + path);
    }
    file_.resize(static_cast<std::size_t>(size));
    std::size_t got = std::fread(file_.data(), 1, file_.size(), f);
    std::fclose(f);
    if (got != file_.size()) return status::io_error("SSTable short read: " + path);

    // Parse footer.
    const char* footer = file_.data() + file_.size() - kFooterSize;
    const std::uint64_t bloom_off = decode_fixed64(footer);
    const std::uint64_t bloom_size = decode_fixed64(footer + 8);
    const std::uint64_t index_off = decode_fixed64(footer + 16);
    const std::uint64_t index_size = decode_fixed64(footer + 24);
    const std::uint64_t magic = decode_fixed64(footer + 32);
    if (magic != kMagic) return status::corruption("bad SSTable magic: " + path);
    if (bloom_off > file_.size() || index_off > file_.size() ||
        bloom_off + bloom_size > file_.size() ||
        index_off + index_size > file_.size() || bloom_off > index_off)
        return status::corruption("bad SSTable footer: " + path);

    data_size_ = bloom_off;
    bloom_ = std::string_view(file_.data() + bloom_off, bloom_size);

    // Parse the index into memory.
    const char* p = file_.data() + index_off;
    const char* limit = p + index_size;
    if (limit - p < 4) return status::corruption("bad SSTable index: " + path);
    std::uint32_t count = decode_fixed32(p);
    p += 4;
    index_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::string_view key;
        if (!get_length_prefixed(&p, limit, &key) || limit - p < 8)
            return status::corruption("truncated SSTable index: " + path);
        std::uint64_t off = decode_fixed64(p);
        p += 8;
        index_.emplace_back(key, off);
    }
    return status::ok();
}

sstable_reader::lookup sstable_reader::get(std::string_view key,
                                           std::string* value) const {
    if (!bloom::may_contain(bloom_, key)) return lookup::absent;

    auto it = std::lower_bound(
        index_.begin(), index_.end(), key,
        [](const std::pair<std::string_view, std::uint64_t>& e, std::string_view k) {
            return e.first < k;
        });
    if (it == index_.end() || it->first != key) return lookup::absent;

    std::string_view k, v;
    value_kind kind;
    std::size_t next;
    if (!parse_entry(it->second, &k, &kind, &v, &next)) return lookup::absent;
    if (kind == value_kind::tombstone) return lookup::tombstone;
    if (value) value->assign(v.data(), v.size());
    return lookup::value;
}

std::size_t sstable_reader::seek_offset(std::string_view target) const {
    auto it = std::lower_bound(
        index_.begin(), index_.end(), target,
        [](const std::pair<std::string_view, std::uint64_t>& e, std::string_view k) {
            return e.first < k;
        });
    return it == index_.end() ? data_size_ : it->second;
}

bool sstable_reader::parse_entry(std::size_t off, std::string_view* key,
                                 value_kind* kind, std::string_view* value,
                                 std::size_t* next) const {
    if (off >= data_size_) return false;
    const char* p = file_.data() + off;
    const char* limit = file_.data() + data_size_;
    if (p >= limit) return false;
    *kind = (static_cast<unsigned char>(*p) == 1) ? value_kind::value
                                                  : value_kind::tombstone;
    ++p;
    if (!get_length_prefixed(&p, limit, key)) return false;
    if (!get_length_prefixed(&p, limit, value)) return false;
    *next = static_cast<std::size_t>(p - file_.data());
    return true;
}

}  // namespace sklad
