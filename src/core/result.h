// core/result.h — the minimal Result<T> used across the library (B1).
//
// One error language (spec2 principle 4): Ok carries the value; Error
// carries a Failure (the expected-outcome record); exceptional situations
// are NOT carried here — they throw FatalError.
//
// Hand-rolled on purpose: std::expected would pull C++23. The storage is
// std::optional<T> (engages only on Ok) — move-only types like FileHandle
// work naturally, no default construction required. Invariants:
//   * is_ok()  <=> value_ engaged;
//   * accessing the wrong side is a programming error (assert in debug);
//   * Result<void> carries no value, only the Failure on Error.

#pragma once

#include <cassert>
#include <optional>
#include <utility>

#include "common.h"

namespace vmt {

template <typename T>
class [[nodiscard]] Result {
public:
    static Result Ok(T value) { return Result(std::move(value)); }
    static Result Error(Failure failure) { return Result(std::move(failure)); }

    bool is_ok() const { return value_.has_value(); }
    bool is_error() const { return !value_.has_value(); }

    // Unchecked on the wrong side (documented contract; debug asserts).
    T& value() {
        assert(value_.has_value());
        return *value_;
    }
    const T& value() const {
        assert(value_.has_value());
        return *value_;
    }

    Failure& error() {
        assert(!value_.has_value());
        return failure_;
    }
    const Failure& error() const {
        assert(!value_.has_value());
        return failure_;
    }

private:
    explicit Result(T value) : value_(std::move(value)) {}
    explicit Result(Failure failure) : failure_(std::move(failure)) {}

    std::optional<T> value_;   // engaged <=> Ok
    Failure failure_;          // meaningful only on Error
};

// Result<void>: Ok carries nothing; Error carries the Failure.
template <>
class [[nodiscard]] Result<void> {
public:
    static Result Ok() { return Result(true); }
    static Result Error(Failure failure) { return Result(std::move(failure)); }

    bool is_ok() const { return ok_; }
    bool is_error() const { return !ok_; }

    Failure& error() {
        assert(!ok_);
        return failure_;
    }
    const Failure& error() const {
        assert(!ok_);
        return failure_;
    }

private:
    explicit Result(bool /*ok tag*/) : ok_(true) {}
    explicit Result(Failure failure) : ok_(false), failure_(std::move(failure)) {}

    bool ok_;
    Failure failure_;   // meaningful only on Error
};

} // namespace vmt
