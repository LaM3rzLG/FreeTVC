// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal JSON reader for test data (SingleStepTests); throws std::runtime_error on malformed input.
#pragma once

#include <cstddef>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace freetvc::test {

struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    // Object members as parallel vectors: a vector of an incomplete type is valid, a pair holding one is not.
    std::vector<std::string> keys;
    std::vector<JsonValue> values;

    const JsonValue* find(std::string_view key) const;
    const JsonValue& at(std::string_view key) const;
    int as_int() const { return static_cast<int>(number); }
};

inline const JsonValue* JsonValue::find(std::string_view key) const {
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) {
            return &values[i];
        }
    }
    return nullptr;
}

inline const JsonValue& JsonValue::at(std::string_view key) const {
    const JsonValue* value = find(key);
    if (!value) {
        throw std::runtime_error("missing JSON key '" + std::string(key) + "'");
    }
    return *value;
}

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    JsonValue parse() {
        JsonValue value = parse_value();
        skip_space();
        if (pos_ != text_.size()) {
            fail("trailing data");
        }
        return value;
    }

private:
    [[noreturn]] void fail(const char* what) const {
        throw std::runtime_error(std::string("JSON: ") + what + " at offset " + std::to_string(pos_));
    }

    void skip_space() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' || text_[pos_] == '\t')) {
            ++pos_;
        }
    }

    bool consume(std::string_view word) {
        if (text_.substr(pos_, word.size()) == word) {
            pos_ += word.size();
            return true;
        }
        return false;
    }

    JsonValue parse_value() {
        skip_space();
        if (pos_ >= text_.size()) {
            fail("unexpected end");
        }
        JsonValue value;
        const char c = text_[pos_];
        if (c == '{') {
            value.kind = JsonValue::Kind::Object;
            ++pos_;
            skip_space();
            if (pos_ < text_.size() && text_[pos_] == '}') {
                ++pos_;
                return value;
            }
            for (;;) {
                skip_space();
                std::string key = parse_string();
                skip_space();
                if (!consume(":")) {
                    fail("expected ':'");
                }
                value.keys.push_back(std::move(key));
                value.values.push_back(parse_value());
                skip_space();
                if (consume("}")) {
                    return value;
                }
                if (!consume(",")) {
                    fail("expected ',' or '}'");
                }
            }
        }
        if (c == '[') {
            value.kind = JsonValue::Kind::Array;
            ++pos_;
            skip_space();
            if (pos_ < text_.size() && text_[pos_] == ']') {
                ++pos_;
                return value;
            }
            for (;;) {
                value.array.push_back(parse_value());
                skip_space();
                if (consume("]")) {
                    return value;
                }
                if (!consume(",")) {
                    fail("expected ',' or ']'");
                }
            }
        }
        if (c == '"') {
            value.kind = JsonValue::Kind::String;
            value.string = parse_string();
            return value;
        }
        if (consume("true")) {
            value.kind = JsonValue::Kind::Bool;
            value.boolean = true;
            return value;
        }
        if (consume("false")) {
            value.kind = JsonValue::Kind::Bool;
            return value;
        }
        if (consume("null")) {
            return value;
        }
        value.kind = JsonValue::Kind::Number;
        const std::size_t start = pos_;
        while (pos_ < text_.size() && (std::string_view("+-.eE0123456789").find(text_[pos_]) != std::string_view::npos)) {
            ++pos_;
        }
        if (start == pos_) {
            fail("unexpected character");
        }
        const std::string digits(text_.substr(start, pos_ - start));
        char* end = nullptr;
        value.number = std::strtod(digits.c_str(), &end);
        if (end != digits.c_str() + digits.size()) {
            fail("bad number");
        }
        return value;
    }

    // Escapes other than \" \\ \/ and the control letters are not needed by the test data.
    std::string parse_string() {
        if (!consume("\"")) {
            fail("expected string");
        }
        std::string out;
        while (pos_ < text_.size() && text_[pos_] != '"') {
            char c = text_[pos_++];
            if (c == '\\') {
                if (pos_ >= text_.size()) {
                    fail("bad escape");
                }
                const char e = text_[pos_++];
                switch (e) {
                case '"': case '\\': case '/': c = e; break;
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                default: fail("unsupported escape");
                }
            }
            out.push_back(c);
        }
        if (!consume("\"")) {
            fail("unterminated string");
        }
        return out;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

}  // namespace freetvc::test
