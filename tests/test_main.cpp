// sklad — functional test suite: CRUD, persistence/recovery, flush, compaction,
// tombstones, ordered iteration and atomic batches. Uses a fresh temp directory
// and returns a non-zero code on any failure. SPDX-License-Identifier: MIT
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "sklad/sklad.hpp"

namespace fs = std::filesystem;
using namespace sklad;

static int g_failed = 0, g_total = 0;
static void check(bool cond, const char* name) {
    ++g_total;
    std::printf(cond ? "  [ok]   %s\n" : "  [FAIL] %s\n", name);
    if (!cond) ++g_failed;
}

// A temp directory removed on destruction.
struct temp_dir {
    fs::path path;
    explicit temp_dir(const std::string& tag) {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("sklad_test_" + tag + "_" + std::to_string(++counter));
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ~temp_dir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
};

static std::string get_or(db& d, const std::string& k, const char* dflt) {
    std::string v;
    return d.get(k, &v).is_ok() ? v : std::string(dflt);
}

static void test_basic() {
    std::printf("basic CRUD:\n");
    temp_dir dir("basic");
    std::unique_ptr<db> d;
    check(db::open({}, dir.str(), &d).is_ok(), "open");

    check(d->put("apple", "red").is_ok(), "put");
    check(get_or(*d, "apple", "?") == "red", "get");
    check(d->contains("apple"), "contains true");
    check(!d->contains("missing"), "contains false");
    check(d->get("missing", nullptr).is_not_found(), "get missing => not_found");

    check(d->put("apple", "green").is_ok(), "overwrite");
    check(get_or(*d, "apple", "?") == "green", "get overwritten");

    check(d->del("apple").is_ok(), "delete");
    check(!d->contains("apple"), "deleted gone");
    check(d->del("never-existed").is_ok(), "delete absent is ok");
}

static void test_persistence() {
    std::printf("persistence (WAL recovery):\n");
    temp_dir dir("persist");
    {
        std::unique_ptr<db> d;
        db::open({}, dir.str(), &d);
        d->put("k1", "v1");
        d->put("k2", "v2");
        d->del("k2");
        d->put("k3", "v3");
        // Destroyed here WITHOUT an explicit flush: data lives only in the WAL.
    }
    {
        std::unique_ptr<db> d;
        check(db::open({}, dir.str(), &d).is_ok(), "reopen");
        check(get_or(*d, "k1", "?") == "v1", "k1 recovered from WAL");
        check(!d->contains("k2"), "k2 deletion recovered");
        check(get_or(*d, "k3", "?") == "v3", "k3 recovered");
    }
}

static void test_flush_and_sstables() {
    std::printf("flush -> SSTable:\n");
    temp_dir dir("flush");
    std::unique_ptr<db> d;
    db::open({}, dir.str(), &d);
    d->put("a", "1");
    d->put("b", "2");
    check(d->sstable_count() == 0, "no SSTables before flush");
    check(d->flush().is_ok(), "flush");
    check(d->sstable_count() == 1, "one SSTable after flush");
    check(get_or(*d, "a", "?") == "1", "read a from SSTable");
    check(get_or(*d, "b", "?") == "2", "read b from SSTable");

    // A deletion after flush must shadow the on-disk value.
    d->del("a");
    check(!d->contains("a"), "delete shadows SSTable value");
    d->flush();
    check(!d->contains("a"), "tombstone persists across flush");
    check(d->sstable_count() == 2, "second SSTable");
}

static void test_persist_after_flush() {
    std::printf("persistence (MANIFEST recovery):\n");
    temp_dir dir("manifest");
    {
        std::unique_ptr<db> d;
        db::open({}, dir.str(), &d);
        d->put("x", "100");
        d->put("y", "200");
        d->flush();
        d->put("z", "300");  // stays in the WAL only
    }
    {
        std::unique_ptr<db> d;
        check(db::open({}, dir.str(), &d).is_ok(), "reopen after flush");
        check(d->sstable_count() == 1, "SSTable restored from MANIFEST");
        check(get_or(*d, "x", "?") == "100", "x from SSTable");
        check(get_or(*d, "z", "?") == "300", "z from WAL");
    }
}

static void test_compaction() {
    std::printf("compaction:\n");
    temp_dir dir("compact");
    options opts;
    opts.bloom_bits_per_key = 10;
    std::unique_ptr<db> d;
    db::open(opts, dir.str(), &d);

    // Create several SSTables with overwrites and a deletion.
    for (int round = 0; round < 4; ++round) {
        for (int i = 0; i < 50; ++i)
            d->put("key" + std::to_string(i), "round" + std::to_string(round));
        d->flush();
    }
    d->del("key7");
    d->flush();
    check(d->sstable_count() == 5, "five SSTables before compaction");

    check(d->compact().is_ok(), "compact");
    check(d->sstable_count() == 1, "one SSTable after compaction");
    // Newest value wins; deleted key stays gone.
    check(get_or(*d, "key0", "?") == "round3", "newest value survives");
    check(get_or(*d, "key49", "?") == "round3", "newest value survives (2)");
    check(!d->contains("key7"), "deleted key dropped by compaction");
}

static void test_iteration() {
    std::printf("ordered iteration:\n");
    temp_dir dir("iter");
    options opts;
    opts.memtable_bytes = 256;  // force several flushes
    std::unique_ptr<db> d;
    db::open(opts, dir.str(), &d);

    d->put("banana", "2");
    d->put("apple", "1");
    d->put("cherry", "3");
    d->flush();
    d->put("date", "4");
    d->put("apple", "1-new");  // overwrite spanning memtable + SSTable
    d->del("cherry");

    std::vector<std::pair<std::string, std::string>> got;
    auto it = d->new_iterator();
    for (it->seek_to_first(); it->valid(); it->next())
        got.emplace_back(std::string(it->key()), std::string(it->value()));

    check(got.size() == 3, "three live keys (cherry deleted)");
    check(got.size() == 3 && got[0].first == "apple" && got[0].second == "1-new",
          "sorted, newest value for apple");
    check(got.size() == 3 && got[1].first == "banana", "banana second");
    check(got.size() == 3 && got[2].first == "date", "date last");

    // seek to a midpoint.
    auto it2 = d->new_iterator();
    it2->seek("b");
    check(it2->valid() && std::string(it2->key()) == "banana", "seek('b') => banana");
}

static void test_batch() {
    std::printf("atomic batch:\n");
    temp_dir dir("batch");
    std::unique_ptr<db> d;
    db::open({}, dir.str(), &d);
    d->put("keep", "yes");

    write_batch b;
    b.put("one", "1");
    b.put("two", "2");
    b.del("keep");
    check(b.count() == 3, "batch has 3 ops");
    check(d->write(b).is_ok(), "write batch");
    check(get_or(*d, "one", "?") == "1", "batch put 1");
    check(get_or(*d, "two", "?") == "2", "batch put 2");
    check(!d->contains("keep"), "batch delete applied");
}

static void test_scale() {
    std::printf("scale (auto-flush + compaction under load):\n");
    temp_dir dir("scale");
    options opts;
    opts.memtable_bytes = 4096;    // small: forces many flushes
    opts.compaction_trigger = 4;   // and periodic compaction
    std::unique_ptr<db> d;
    db::open(opts, dir.str(), &d);

    const int N = 5000;
    for (int i = 0; i < N; ++i)
        d->put("k" + std::to_string(i), "v" + std::to_string(i));
    // Overwrite the first half.
    for (int i = 0; i < N / 2; ++i)
        d->put("k" + std::to_string(i), "updated");

    bool all_ok = true;
    for (int i = 0; i < N; ++i) {
        std::string expect = (i < N / 2) ? "updated" : ("v" + std::to_string(i));
        if (get_or(*d, "k" + std::to_string(i), "?") != expect) { all_ok = false; break; }
    }
    check(all_ok, "all 5000 keys correct after flushes/compactions");

    // Survives a reopen.
    d.reset();
    db::open(opts, dir.str(), &d);
    check(get_or(*d, "k0", "?") == "updated", "reopen: k0 updated");
    check(get_or(*d, "k4999", "?") == "v4999", "reopen: k4999 original");
}

int main() {
    std::printf("== sklad test suite ==\n\n");
    test_basic();
    test_persistence();
    test_flush_and_sstables();
    test_persist_after_flush();
    test_compaction();
    test_iteration();
    test_batch();
    test_scale();

    std::printf("\n%d/%d checks passed.\n", g_total - g_failed, g_total);
    if (g_failed) {
        std::printf("FAILED: %d\n", g_failed);
        return 1;
    }
    std::printf("All tests passed.\n");
    return 0;
}
