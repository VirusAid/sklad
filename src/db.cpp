// sklad — the LSM database orchestration. SPDX-License-Identifier: MIT
//
// Ties together the MemTable, write-ahead log and SSTables, and manages the
// MANIFEST (the durable list of live SSTables), crash recovery, flushing and
// compaction. A single mutex serializes all operations for correctness.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "bloom.hpp"
#include "coding.hpp"
#include "memtable.hpp"
#include "sklad/db.hpp"
#include "sstable.hpp"
#include "wal.hpp"

namespace fs = std::filesystem;

namespace sklad {

namespace {

constexpr std::uint64_t kManifestMagic = 0x736B6C4D414E0001ULL;

std::string num_to_name(std::uint64_t n) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%06llu.sst", static_cast<unsigned long long>(n));
    return buf;
}

// ---- internal cursors for the k-way merge --------------------------------

struct cursor {
    virtual ~cursor() = default;
    virtual void seek_to_first() = 0;
    virtual void seek(std::string_view target) = 0;
    virtual bool valid() const = 0;
    virtual void next() = 0;
    virtual std::string_view key() const = 0;
    virtual std::string_view value() const = 0;
    virtual value_kind kind() const = 0;
    int recency = 0;  // higher = newer; wins on equal keys
};

// A cursor over a snapshot of the MemTable (copied entries, key order).
struct mem_cursor : cursor {
    struct item {
        std::string key, value;
        value_kind kind;
    };
    std::shared_ptr<std::vector<item>> items;
    std::size_t i = 0;

    void seek_to_first() override { i = 0; }
    void seek(std::string_view target) override {
        i = static_cast<std::size_t>(
            std::lower_bound(items->begin(), items->end(), target,
                             [](const item& a, std::string_view k) { return a.key < k; }) -
            items->begin());
    }
    bool valid() const override { return i < items->size(); }
    void next() override { ++i; }
    std::string_view key() const override { return (*items)[i].key; }
    std::string_view value() const override { return (*items)[i].value; }
    value_kind kind() const override { return (*items)[i].kind; }
};

// A cursor over an immutable SSTable, held alive via shared_ptr.
struct sst_cursor : cursor {
    std::shared_ptr<sstable_reader> reader;
    std::size_t off = 0;
    std::string_view k_, v_;
    value_kind kind_ = value_kind::value;
    bool valid_ = false;

    void load() {
        std::size_t next;
        valid_ = reader->parse_entry(off, &k_, &kind_, &v_, &next);
    }
    void seek_to_first() override {
        off = 0;
        load();
    }
    void seek(std::string_view target) override {
        off = reader->seek_offset(target);
        load();
    }
    bool valid() const override { return valid_; }
    void next() override {
        std::size_t next;
        if (reader->parse_entry(off, &k_, &kind_, &v_, &next)) {
            off = next;
            load();
        } else {
            valid_ = false;
        }
    }
    std::string_view key() const override { return k_; }
    std::string_view value() const override { return v_; }
    value_kind kind() const override { return kind_; }
};

// Merges several cursors into one deduplicated ascending stream. For equal
// keys the newest (highest recency) entry wins; older duplicates are dropped.
// Emits tombstones too; the caller decides whether to skip them.
class merger {
public:
    void add(std::unique_ptr<cursor> c) { sources_.push_back(std::move(c)); }

    void seek_to_first() {
        for (auto& s : sources_) s->seek_to_first();
        advance_to_next_key();
    }
    void seek(std::string_view target) {
        for (auto& s : sources_) s->seek(target);
        advance_to_next_key();
    }
    bool valid() const { return valid_; }
    std::string_view key() const { return key_; }
    std::string_view value() const { return value_; }
    value_kind kind() const { return kind_; }

    void next() { advance_to_next_key(); }

private:
    // Position at the next distinct key, choosing the newest source for it.
    void advance_to_next_key() {
        valid_ = false;
        // Find the smallest current key among valid sources.
        std::string_view best;
        bool found = false;
        for (auto& s : sources_) {
            if (!s->valid()) continue;
            if (!found || s->key() < best) {
                best = s->key();
                found = true;
            }
        }
        if (!found) return;

        // Among sources at `best`, pick the newest; advance all of them.
        int best_recency = -1;
        std::string_view chosen_val;
        value_kind chosen_kind = value_kind::tombstone;
        for (auto& s : sources_) {
            if (s->valid() && s->key() == best) {
                if (s->recency > best_recency) {
                    best_recency = s->recency;
                    chosen_val = s->value();
                    chosen_kind = s->kind();
                }
            }
        }
        // Materialize the winner before advancing sources (views may dangle).
        key_owned_.assign(best.data(), best.size());
        val_owned_.assign(chosen_val.data(), chosen_val.size());
        for (auto& s : sources_) {
            while (s->valid() && s->key() == best) s->next();
        }
        key_ = key_owned_;
        value_ = val_owned_;
        kind_ = chosen_kind;
        valid_ = true;
    }

    std::vector<std::unique_ptr<cursor>> sources_;
    std::string key_owned_, val_owned_;
    std::string_view key_, value_;
    value_kind kind_ = value_kind::value;
    bool valid_ = false;
};

// The public iterator: a merger that skips tombstones and exposes only values.
class db_iterator : public iterator {
public:
    explicit db_iterator(std::unique_ptr<merger> m) : m_(std::move(m)) {}

    void seek_to_first() override {
        m_->seek_to_first();
        skip_tombstones();
    }
    void seek(std::string_view target) override {
        m_->seek(target);
        skip_tombstones();
    }
    bool valid() const override { return m_->valid(); }
    void next() override {
        m_->next();
        skip_tombstones();
    }
    std::string_view key() const override { return m_->key(); }
    std::string_view value() const override { return m_->value(); }

private:
    void skip_tombstones() {
        while (m_->valid() && m_->kind() == value_kind::tombstone) m_->next();
    }
    std::unique_ptr<merger> m_;
};

// ---- the database implementation -----------------------------------------

class db_impl : public db {
public:
    db_impl(const options& opts, std::string dir) : opts_(opts), dir_(std::move(dir)) {}

    status open_impl();

    status put(std::string_view key, std::string_view value,
               const write_options& wo) override {
        write_batch b;
        b.put(key, value);
        return write(b, wo);
    }
    status del(std::string_view key, const write_options& wo) override {
        write_batch b;
        b.del(key);
        return write(b, wo);
    }

    status write(const write_batch& batch, const write_options& wo) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (batch.empty()) return status::ok();

        // Serialize the batch and append it to the WAL first (durability).
        // Record format: [count varint][ (op 1B)(klen,key)[ (vlen,value) ] ... ]
        // op byte: 1 = put, 2 = del.
        std::string payload;
        put_varint32(&payload, static_cast<std::uint32_t>(batch.count()));
        struct wal_encoder {
            std::string* p;
            void put(std::string_view k, std::string_view v) {
                p->push_back(1);
                put_length_prefixed(p, k);
                put_length_prefixed(p, v);
            }
            void del(std::string_view k) {
                p->push_back(2);
                put_length_prefixed(p, k);
            }
        } enc{&payload};
        batch.iterate(enc);

        status s = wal_.add_record(payload, wo.sync || opts_.sync_writes);
        if (!s) return s;

        // Then apply to the MemTable.
        struct mem_applier {
            mem_table* m;
            void put(std::string_view k, std::string_view v) { m->put(k, v); }
            void del(std::string_view k) { m->del(k); }
        } app{&mem_};
        batch.iterate(app);

        return maybe_flush_locked();
    }

    status get(std::string_view key, std::string* value) override {
        std::lock_guard<std::mutex> lk(mu_);
        std::string tmp;
        auto r = mem_.get(key, value ? value : &tmp);
        if (r == mem_table::result::found_value) return status::ok();
        if (r == mem_table::result::found_tombstone) return status::not_found();

        // Newest SSTable first.
        for (auto it = tables_.rbegin(); it != tables_.rend(); ++it) {
            auto res = it->reader->get(key, value);
            if (res == sstable_reader::lookup::value) return status::ok();
            if (res == sstable_reader::lookup::tombstone) return status::not_found();
        }
        return status::not_found();
    }

    bool contains(std::string_view key) override {
        std::string ignore;
        return get(key, &ignore).is_ok();
    }

    std::unique_ptr<iterator> new_iterator() override {
        std::lock_guard<std::mutex> lk(mu_);
        auto m = std::make_unique<merger>();

        // Snapshot the MemTable (highest recency).
        auto items = std::make_shared<std::vector<mem_cursor::item>>();
        items->reserve(mem_.size());
        for (const auto& kv : mem_.data())
            items->push_back({kv.first, kv.second.value, kv.second.kind});
        int recency = static_cast<int>(tables_.size()) + 1;
        {
            auto mc = std::make_unique<mem_cursor>();
            mc->items = items;
            mc->recency = recency;
            m->add(std::move(mc));
        }
        // SSTables: older number => lower recency.
        int r = static_cast<int>(tables_.size());
        for (auto it = tables_.rbegin(); it != tables_.rend(); ++it, --r) {
            auto sc = std::make_unique<sst_cursor>();
            sc->reader = it->reader;
            sc->recency = r;
            m->add(std::move(sc));
        }
        return std::make_unique<db_iterator>(std::move(m));
    }

    status flush() override {
        std::lock_guard<std::mutex> lk(mu_);
        return flush_locked();
    }

    status compact() override {
        std::lock_guard<std::mutex> lk(mu_);
        status s = flush_locked();  // fold the MemTable in first
        if (!s) return s;
        return compact_locked();
    }

    std::size_t sstable_count() override {
        std::lock_guard<std::mutex> lk(mu_);
        return tables_.size();
    }

private:
    struct live_table {
        std::uint64_t number;
        std::shared_ptr<sstable_reader> reader;
    };

    std::string sst_path(std::uint64_t n) const {
        return (fs::path(dir_) / num_to_name(n)).string();
    }
    std::string wal_path() const { return (fs::path(dir_) / "wal.log").string(); }
    std::string manifest_path() const { return (fs::path(dir_) / "MANIFEST").string(); }

    status maybe_flush_locked() {
        if (mem_.approx_bytes() >= opts_.memtable_bytes) {
            status s = flush_locked();
            if (!s) return s;
            if (tables_.size() >= opts_.compaction_trigger) return compact_locked();
        }
        return status::ok();
    }

    status flush_locked();
    status compact_locked();
    status write_manifest(std::uint64_t next_num,
                          const std::vector<std::uint64_t>& numbers);
    status load_manifest(bool* had_manifest);
    status recover_wal();

    options opts_;
    std::string dir_;
    std::mutex mu_;
    mem_table mem_;
    wal_writer wal_;
    std::vector<live_table> tables_;  // ascending by number (oldest..newest)
    std::uint64_t next_file_number_ = 1;
};

status db_impl::write_manifest(std::uint64_t next_num,
                               const std::vector<std::uint64_t>& numbers) {
    std::string payload;
    put_fixed64(&payload, kManifestMagic);
    put_fixed64(&payload, next_num);
    put_fixed32(&payload, static_cast<std::uint32_t>(numbers.size()));
    for (std::uint64_t n : numbers) put_fixed64(&payload, n);

    std::string file;
    put_fixed32(&file, crc32(payload));
    file += payload;

    const std::string tmp = manifest_path() + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return status::io_error("cannot write MANIFEST");
    bool ok = std::fwrite(file.data(), 1, file.size(), f) == file.size();
    status s = ok ? fsync_file(f) : status::io_error("MANIFEST write failed");
    std::fclose(f);
    if (!s) return s;
    std::remove(manifest_path().c_str());
    if (std::rename(tmp.c_str(), manifest_path().c_str()) != 0)
        return status::io_error("MANIFEST rename failed");
    return status::ok();
}

status db_impl::load_manifest(bool* had_manifest) {
    *had_manifest = false;
    std::FILE* f = std::fopen(manifest_path().c_str(), "rb");
    if (!f) return status::ok();  // fresh database
    *had_manifest = true;
    std::string buf;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 4) {
        std::fclose(f);
        return status::corruption("MANIFEST too small");
    }
    buf.resize(static_cast<std::size_t>(size));
    std::size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) return status::io_error("MANIFEST short read");

    const std::uint32_t want_crc = decode_fixed32(buf.data());
    std::string_view payload(buf.data() + 4, buf.size() - 4);
    if (crc32(payload) != want_crc) return status::corruption("MANIFEST CRC mismatch");

    const char* p = payload.data();
    const char* limit = p + payload.size();
    if (limit - p < 20) return status::corruption("MANIFEST truncated");
    if (decode_fixed64(p) != kManifestMagic) return status::corruption("MANIFEST magic");
    p += 8;
    next_file_number_ = decode_fixed64(p);
    p += 8;
    std::uint32_t count = decode_fixed32(p);
    p += 4;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (limit - p < 8) return status::corruption("MANIFEST list truncated");
        std::uint64_t n = decode_fixed64(p);
        p += 8;
        auto reader = std::make_shared<sstable_reader>();
        status s = reader->open(sst_path(n));
        if (!s) return s;
        tables_.push_back({n, std::move(reader)});
    }
    std::sort(tables_.begin(), tables_.end(),
              [](const live_table& a, const live_table& b) { return a.number < b.number; });
    return status::ok();
}

status db_impl::recover_wal() {
    wal_reader reader;
    if (!fs::exists(wal_path())) return status::ok();
    status s = reader.open(wal_path());
    if (!s) return s;
    std::string rec;
    bool eof = false;
    while (true) {
        s = reader.read_record(&rec, &eof);
        if (!s) return s;
        if (eof) break;
        const char* p = rec.data();
        const char* limit = p + rec.size();
        std::uint32_t count = 0;
        if (!get_varint32(&p, limit, &count)) return status::corruption("WAL record");
        for (std::uint32_t i = 0; i < count; ++i) {
            if (p >= limit) return status::corruption("WAL op truncated");
            const unsigned char op = static_cast<unsigned char>(*p++);  // 1=put 2=del
            std::string_view key;
            if (!get_length_prefixed(&p, limit, &key))
                return status::corruption("WAL key");
            if (op == 1) {
                std::string_view val;
                if (!get_length_prefixed(&p, limit, &val))
                    return status::corruption("WAL value");
                mem_.put(key, val);
            } else {
                mem_.del(key);
            }
        }
    }
    return status::ok();
}

status db_impl::open_impl() {
    const bool exists = fs::exists(dir_);
    if (exists && opts_.error_if_exists)
        return status::invalid_argument("database already exists: " + dir_);
    if (!exists) {
        if (!opts_.create_if_missing)
            return status::invalid_argument("database does not exist: " + dir_);
        std::error_code ec;
        fs::create_directories(dir_, ec);
        if (ec) return status::io_error("cannot create directory: " + dir_);
    }

    bool had_manifest = false;
    status s = load_manifest(&had_manifest);
    if (!s) return s;

    // Remove orphan .sst files not referenced by the MANIFEST (leftovers from a
    // crash mid-flush/compaction).
    {
        std::vector<std::uint64_t> live;
        for (const auto& t : tables_) live.push_back(t.number);
        std::error_code ec;
        for (const auto& de : fs::directory_iterator(dir_, ec)) {
            if (ec) break;
            const std::string name = de.path().filename().string();
            if (name.size() >= 4 && name.substr(name.size() - 4) == ".sst") {
                std::uint64_t n = std::strtoull(name.c_str(), nullptr, 10);
                if (std::find(live.begin(), live.end(), n) == live.end())
                    fs::remove(de.path(), ec);
            }
        }
    }

    s = recover_wal();
    if (!s) return s;

    // Open the WAL for appending subsequent writes.
    return wal_.open(wal_path());
}

status db_impl::flush_locked() {
    if (mem_.empty()) return status::ok();

    const std::uint64_t n = next_file_number_;
    sstable_builder builder;
    for (const auto& kv : mem_.data())
        builder.add(kv.first, kv.second.kind, kv.second.value);
    status s = builder.finish(sst_path(n), opts_.bloom_bits_per_key);
    if (!s) return s;

    auto reader = std::make_shared<sstable_reader>();
    s = reader->open(sst_path(n));
    if (!s) return s;

    std::vector<std::uint64_t> numbers;
    for (const auto& t : tables_) numbers.push_back(t.number);
    numbers.push_back(n);
    s = write_manifest(next_file_number_ + 1, numbers);
    if (!s) return s;

    // Commit: adopt the new table, bump the file counter, reset the WAL and
    // MemTable. The WAL is now redundant because its data lives in the SSTable.
    tables_.push_back({n, std::move(reader)});
    ++next_file_number_;
    wal_.close();
    s = wal_.open_truncate(wal_path());
    if (!s) return s;
    mem_.clear();
    return status::ok();
}

status db_impl::compact_locked() {
    if (tables_.size() <= 1) return status::ok();

    // Merge every SSTable, newest wins, dropping tombstones (nothing older can
    // resurrect a key once all tables are merged).
    merger m;
    int r = static_cast<int>(tables_.size());
    for (auto it = tables_.rbegin(); it != tables_.rend(); ++it, --r) {
        auto sc = std::make_unique<sst_cursor>();
        sc->reader = it->reader;
        sc->recency = r;
        m.add(std::move(sc));
    }

    const std::uint64_t n = next_file_number_;
    sstable_builder builder;
    for (m.seek_to_first(); m.valid(); m.next()) {
        if (m.kind() == value_kind::value) builder.add(m.key(), value_kind::value, m.value());
    }
    status s = builder.finish(sst_path(n), opts_.bloom_bits_per_key);
    if (!s) return s;

    auto reader = std::make_shared<sstable_reader>();
    s = reader->open(sst_path(n));
    if (!s) return s;

    s = write_manifest(next_file_number_ + 1, {n});
    if (!s) return s;

    // Commit: replace all tables with the merged one, then delete old files.
    std::vector<std::uint64_t> old;
    for (const auto& t : tables_) old.push_back(t.number);
    tables_.clear();
    tables_.push_back({n, std::move(reader)});
    ++next_file_number_;
    for (std::uint64_t o : old) std::remove(sst_path(o).c_str());
    return status::ok();
}

}  // namespace

// ---- public entry point ---------------------------------------------------

status db::open(const options& opts, const std::string& path,
                std::unique_ptr<db>* out) {
    auto impl = std::make_unique<db_impl>(opts, path);
    status s = impl->open_impl();
    if (!s) return s;
    *out = std::move(impl);
    return status::ok();
}

}  // namespace sklad
