// sklad — an embedded LSM-tree key-value storage engine for C++17.
// SPDX-License-Identifier: MIT
//
// status.hpp — the result type returned by every operation that can fail.
// Cheap to copy when ok (no allocation); carries a message only on error.
#ifndef SKLAD_STATUS_HPP
#define SKLAD_STATUS_HPP

#include <memory>
#include <string>
#include <utility>

namespace sklad {

class status {
public:
    enum class code {
        ok = 0,
        not_found,
        corruption,
        io_error,
        invalid_argument,
        not_supported,
    };

    status() noexcept = default;  // ok

    static status ok() { return status(); }
    static status not_found(std::string msg = {}) {
        return status(code::not_found, std::move(msg));
    }
    static status corruption(std::string msg = {}) {
        return status(code::corruption, std::move(msg));
    }
    static status io_error(std::string msg = {}) {
        return status(code::io_error, std::move(msg));
    }
    static status invalid_argument(std::string msg = {}) {
        return status(code::invalid_argument, std::move(msg));
    }
    static status not_supported(std::string msg = {}) {
        return status(code::not_supported, std::move(msg));
    }

    bool is_ok() const noexcept { return code_ == code::ok; }
    bool is_not_found() const noexcept { return code_ == code::not_found; }
    bool is_corruption() const noexcept { return code_ == code::corruption; }
    bool is_io_error() const noexcept { return code_ == code::io_error; }

    explicit operator bool() const noexcept { return is_ok(); }

    code error_code() const noexcept { return code_; }

    bool is_invalid_argument() const noexcept { return code_ == code::invalid_argument; }

    /// Human-readable one-line description.
    std::string to_string() const {
        switch (code_) {
            case code::ok: return "OK";
            case code::not_found: return with("NotFound");
            case code::corruption: return with("Corruption");
            case code::io_error: return with("IOError");
            case code::invalid_argument: return with("InvalidArgument");
            case code::not_supported: return with("NotSupported");
        }
        return "Unknown";
    }

private:
    status(code c, std::string msg)
        : code_(c), msg_(msg.empty() ? nullptr : std::make_shared<std::string>(std::move(msg))) {}

    std::string with(const char* name) const {
        return msg_ ? std::string(name) + ": " + *msg_ : std::string(name);
    }

    code code_ = code::ok;
    // Shared so status stays cheap to copy; only allocated on error with a msg.
    std::shared_ptr<std::string> msg_;
};

}  // namespace sklad

#endif  // SKLAD_STATUS_HPP
