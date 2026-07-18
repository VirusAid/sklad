// sklad — example: basic usage of the key-value store.
// Usage: demo_kv [dir]   (defaults to ./sklad-demo)
// SPDX-License-Identifier: MIT
#include <cstdio>
#include <memory>
#include <string>

#include "sklad/sklad.hpp"

using namespace sklad;

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "sklad-demo";

    options opts;
    opts.create_if_missing = true;
    std::unique_ptr<db> store;
    status s = db::open(opts, dir, &store);
    if (!s) {
        std::printf("open failed: %s\n", s.to_string().c_str());
        return 1;
    }

    // Single writes.
    store->put("user:1", "Alice");
    store->put("user:2", "Bob");
    store->put("user:3", "Carol");

    // An atomic batch: either all of these land or none do.
    write_batch batch;
    batch.put("user:4", "Dave");
    batch.del("user:2");            // Bob leaves
    batch.put("user:1", "Alice A.");  // rename
    store->write(batch);

    // Point lookup.
    std::string name;
    if (store->get("user:1", &name).is_ok())
        std::printf("user:1 = %s\n", name.c_str());
    std::printf("user:2 present: %s\n", store->contains("user:2") ? "yes" : "no");

    // Force an on-disk SSTable, then range-scan everything in key order.
    store->flush();
    std::printf("SSTable files: %zu\n", store->sstable_count());
    std::printf("--- all users in order ---\n");
    auto it = store->new_iterator();
    for (it->seek_to_first(); it->valid(); it->next())
        std::printf("  %.*s = %.*s\n", (int)it->key().size(), it->key().data(),
                    (int)it->value().size(), it->value().data());

    std::printf("Data persisted in '%s'. Re-run to see it recovered.\n", dir.c_str());
    return 0;
}
