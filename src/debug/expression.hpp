// SPDX-License-Identifier: GPL-3.0-or-later
// Expressions shared by every front end: C precedence, registers, [byte] / {word} memory, BANK:addr, symbols
// (design/debug-and-remote.md "Expressions").
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "core/machine/machine.hpp"
#include "debug/debugger.hpp"
#include "debug/symbols.hpp"

namespace freetvc::debug {

struct ParseOptions {
    unsigned radix = 16;                    // for numbers without $ / 0x / h / # / %; hex in the monitor
    const SymbolTable* symbols = nullptr;  // names are resolved when parsing
};

struct EvalContext {
    const Machine& machine;
    const Hit* hit = nullptr;  // VALUE and ADDRESS; 0 without a hit
};

class Expression {
public:
    Expression() = default;  // evaluates to 0

    struct Error {
        std::size_t position = 0;  // offset in the text
        std::string message;
    };

    // Either an expression or an error.
    static std::pair<std::optional<Expression>, Error> parse(std::string_view text, const ParseOptions& options = {});

    std::int64_t evaluate(const EvalContext& context) const;
    const std::string& text() const { return text_; }
    // True if it is a plain number or symbol, so it needs no machine.
    bool constant() const;

    struct Node;

private:
    std::string text_;
    std::shared_ptr<const Node> root_;
};

// Parses `[BANK:]expr`: the bank qualifier of a checkpoint or memory address (e.g. "VID1:0100", "U2:start").
struct AddressSpec {
    std::optional<Bank> bank;
    Expression value;
};
std::pair<std::optional<AddressSpec>, Expression::Error> parse_address(std::string_view text, const ParseOptions& options = {});

std::optional<Bank> parse_bank(std::string_view name);

// A checkpoint condition that is true where the expression is non-zero.
std::function<bool(const Machine&, const Hit&)> make_condition(const Expression& expression);

}  // namespace freetvc::debug
