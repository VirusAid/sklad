<h1 align="center">sklad 📦</h1>

<p align="center">
  <b>An embedded, persistent, ordered key-value store for C++17 — a log-structured merge-tree (LSM) engine with zero dependencies.</b>
</p>

<p align="center">
  <img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-blue.svg">
  <img alt="License: MIT" src="https://img.shields.io/badge/License-MIT-green.svg">
  <img alt="build: CMake" src="https://img.shields.io/badge/build-CMake-orange.svg">
  <img alt="deps: none" src="https://img.shields.io/badge/dependencies-none-brightgreen.svg">
  <img alt="tests" src="https://img.shields.io/badge/tests-46%20passing-brightgreen.svg">
</p>

---

## What is it?

`sklad` is a small embeddable database engine in the spirit of **LevelDB /
RocksDB**: you link it into your process and get a durable, crash-safe,
ordered `string → string` map. No server, no dependencies, one `#include`.

It is a real **log-structured merge-tree**, built from the ground up:

```
        put/del                          get
           │                              │
           ▼                              ▼
   ┌──────────────┐   append      ┌──────────────┐
   │ write-ahead  │◀──────────────│   MemTable    │  (in-memory, sorted)
   │  log (WAL)   │   durability  └──────┬───────┘
   └──────────────┘                      │ flush when full
                                         ▼
                         ┌─────────────────────────────┐
                         │  SSTables (immutable, sorted │  newest ─┐
                         │  files with a bloom filter    │          ├─ read path
                         │  + index)                     │  oldest ─┘  checks newest first
                         └──────────────┬───────────────┘
                                        │ compaction
                                        ▼   merge, drop overwritten
                                            values and tombstones
```

- **Durable.** Every write goes to a write-ahead log before it is acknowledged;
  a crash is recovered by replaying it. CRC-checked records detect a torn write.
- **Crash-safe.** The set of live files is tracked in an atomically-swapped
  MANIFEST; orphaned files from an interrupted flush/compaction are cleaned up
  on open.
- **Ordered.** Keys are stored sorted, so range scans and `seek` are first-class.
- **Fast reads.** Each SSTable carries a bloom filter, so a lookup skips files
  that certainly do not hold the key.
- **Self-maintaining.** MemTables flush automatically; SSTables compact
  automatically once too many accumulate.

## Features

| | |
|---|---|
| `put` / `get` / `del` / `contains` | Point operations, newest value wins |
| `write(batch)` | Atomic, durable multi-key writes |
| `new_iterator()` | Ordered range scan with `seek` over a consistent snapshot |
| `flush()` / `compact()` | Manual control when you want it |
| Tunable | MemTable size, compaction trigger, bloom bits/key, per-write `sync` |
| Thread-safe | All operations are safe to call concurrently |

## Quick start

```cpp
#include <sklad/sklad.hpp>
using namespace sklad;

int main() {
    std::unique_ptr<db> store;
    db::open({}, "mydb", &store);        // creates ./mydb if missing

    store->put("user:1", "Alice");
    store->del("user:1");

    std::string value;
    if (store->get("user:2", &value).is_ok())
        /* found */;

    // Atomic batch
    write_batch batch;
    batch.put("a", "1");
    batch.put("b", "2");
    store->write(batch);

    // Ordered scan
    auto it = store->new_iterator();
    for (it->seek_to_first(); it->valid(); it->next())
        use(it->key(), it->value());
}
```

## Build

Requires only a C++17 compiler and CMake ≥ 3.14. No third-party dependencies.

```bash
git clone https://github.com/VirusAid/sklad.git
cd sklad
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Use it in your project (CMake)

```cmake
add_subdirectory(sklad)
target_link_libraries(my_app PRIVATE sklad::sklad)
```

## Tuning

```cpp
options opts;
opts.memtable_bytes     = 8 * 1024 * 1024; // flush threshold (bigger = fewer files)
opts.compaction_trigger = 8;               // compact once this many SSTables exist
opts.bloom_bits_per_key = 10;              // ~1% false-positive rate; 0 disables
opts.sync_writes        = true;            // fsync every write (durable but slow)
db::open(opts, "mydb", &store);
```

## Design notes & limits

- Single-process, embedded: like LevelDB, one process opens a database at a time.
- Compaction is **full** (size-tiered into a single file), not leveled — simple
  and correct, ideal for read-heavy or moderate-write workloads.
- SSTable readers currently keep the file resident in memory (equivalent to an
  mmap), which keeps lookups seek-free; suited to datasets that fit comfortably
  alongside your working set. A block-cache reader is a natural next step.
- A single mutex serializes operations. Correct and simple; not tuned for
  many-core write contention.

It is a genuine, tested LSM engine — not a toy — but young. Battle-test it
before trusting production data, and see [SECURITY.md](SECURITY.md) to report
issues.

## License

MIT — see [LICENSE](LICENSE). Free for any use, including commercial.
