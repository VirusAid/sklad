// sklad — an embedded LSM-tree key-value storage engine for C++17.
// SPDX-License-Identifier: MIT
//
// write_batch.hpp — a group of writes applied atomically and durably. Either
// all of them survive a crash or none do. Also the efficient way to apply many
// writes at once (a single WAL append and one MemTable transaction).
#ifndef SKLAD_WRITE_BATCH_HPP
#define SKLAD_WRITE_BATCH_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace sklad {

class write_batch {
public:
    /// Queue a key/value insertion (overwrites an existing key).
    void put(std::string_view key, std::string_view value) {
        ops_.push_back({kind::put, std::string(key), std::string(value)});
    }

    /// Queue a key deletion (a no-op if the key is absent).
    void del(std::string_view key) {
        ops_.push_back({kind::del, std::string(key), std::string()});
    }

    void clear() noexcept { ops_.clear(); }
    std::size_t count() const noexcept { return ops_.size(); }
    bool empty() const noexcept { return ops_.empty(); }

    /// Replay the queued operations onto a handler exposing put(key, value) and
    /// del(key). Used by the engine to both serialize (to the WAL) and apply
    /// (to the MemTable) a batch through one code path.
    template <class Handler>
    void iterate(Handler&& h) const {
        for (const auto& op : ops_) {
            if (op.k == kind::put)
                h.put(op.key, op.value);
            else
                h.del(op.key);
        }
    }

private:
    enum class kind : unsigned char { put = 1, del = 2 };
    struct op {
        kind k;
        std::string key;
        std::string value;  // empty for del
    };
    std::vector<op> ops_;
};

}  // namespace sklad

#endif  // SKLAD_WRITE_BATCH_HPP
