// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace defragger {

class Json {
public:
    struct Number {
        std::string text;
    };
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;

    Json() noexcept;
    Json(std::nullptr_t) noexcept;
    Json(bool value) noexcept;
    Json(const char* value);
    Json(std::string value);
    Json(Array value);
    Json(Object value);

    static Json number(std::string text);
    static Json integer(std::int64_t value);
    static Json unsigned_integer(std::uint64_t value);
    static Json real(double value);
    static Json parse(std::string_view text);

    bool is_null() const noexcept;
    bool is_bool() const noexcept;
    bool is_number() const noexcept;
    bool is_string() const noexcept;
    bool is_array() const noexcept;
    bool is_object() const noexcept;

    bool boolean() const;
    std::string_view string() const;
    std::string_view number_text() const;
    std::int64_t integer_value() const;
    std::uint64_t unsigned_value() const;
    double real_value() const;

    const Array& array() const;
    Array& array();
    const Object& object() const;
    Object& object();

    const Json* find(std::string_view key) const noexcept;
    Json* find(std::string_view key) noexcept;
    const Json& at(std::string_view key) const;
    Json& at(std::string_view key);

    std::string string_or(std::string_view fallback = {}) const;
    std::uint64_t unsigned_or(std::uint64_t fallback = 0U) const;
    std::int64_t integer_or(std::int64_t fallback = 0) const;
    double real_or(double fallback = 0.0) const;
    bool bool_or(bool fallback = false) const;

    std::string dump() const;

private:
    using Storage =
        std::variant<std::nullptr_t, bool, Number, std::string, Array, Object>;
    Storage value_;
    explicit Json(Number value);
};

} // namespace defragger
