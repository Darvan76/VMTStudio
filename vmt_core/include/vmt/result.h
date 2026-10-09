#pragma once

#include "vmt/error.h"

#include <type_traits>
#include <utility>
#include <variant>

namespace vmt {

// Result<T> carries either a value or an Error. No exceptions are used for
// control flow; every public entry point returns a Result and never throws
// across a module boundary.
template <class T>
class Result {
public:
    static_assert(!std::is_reference_v<T>, "Result<T&> is not supported; use Result<T*>");

    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    bool ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    T&       value() &       { return std::get<0>(storage_); }
    const T& value() const&  { return std::get<0>(storage_); }
    T&&      value() &&      { return std::get<0>(std::move(storage_)); }

    T value_or(T fallback) const& {
        return ok() ? std::get<0>(storage_) : std::move(fallback);
    }

    const Error& error() const& { return std::get<1>(storage_); }

    template <class F>
    auto map(F&& f) const& {
        using U = std::decay_t<decltype(f(std::declval<const T&>()))>;
        if (ok()) return Result<U>(f(std::get<0>(storage_)));
        return Result<U>(std::get<1>(storage_));
    }

    template <class F>
    auto and_then(F&& f) const& {
        using U = std::decay_t<decltype(f(std::declval<const T&>()))>;
        if (ok()) return f(std::get<0>(storage_));
        return Result<U>(std::get<1>(storage_));
    }

private:
    std::variant<T, Error> storage_;
};

template <>
class Result<void> {
public:
    Result() noexcept : storage_(std::in_place_index<0>) {}
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    bool ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    const Error& error() const& { return std::get<1>(storage_); }

private:
    std::variant<std::monostate, Error> storage_;
};

template <class T>
Result<std::decay_t<T>> Ok(T&& value) {
    return Result<std::decay_t<T>>(std::forward<T>(value));
}

inline Result<void> Ok() {
    return Result<void>();
}

template <class T = void>
Result<T> Err(Error error) {
    return Result<T>(std::move(error));
}

} // namespace vmt
