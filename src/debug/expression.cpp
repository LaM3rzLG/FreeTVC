// SPDX-License-Identifier: GPL-3.0-or-later
#include "debug/expression.hpp"

#include <array>
#include <cctype>
#include <utility>

namespace freetvc::debug {

namespace {

enum class Op : std::uint8_t {
    Number, Var, Byte, Word,
    Neg, Not, Compl,
    Mul, Div, Mod, Add, Sub, Shl, Shr,
    Lt, Le, Gt, Ge, Eq, Ne,
    And, Xor, Or, LAnd, LOr,
};

enum class Var : std::uint8_t {
    A, F, B, C, D, E, H, L, I, R, AF, BC, DE, HL, IX, IY, SP, PC, WZ, AF2, BC2, DE2, HL2, IXH, IXL, IYH, IYL,
    IM, IFF1, IFF2, SF, ZF, HF, PF, NF, CF, Ticks, Cycle, Frame, Line, Column, Value, Address,
};

struct VarName {
    const char* name;
    Var var;
};

// Registers by Zilog name (AF' etc. with the apostrophe), flags as xF, machine time and the checkpoint hit
constexpr std::array<VarName, 46> kVars{{
    {"A", Var::A}, {"F", Var::F}, {"B", Var::B}, {"C", Var::C}, {"D", Var::D}, {"E", Var::E}, {"H", Var::H},
    {"L", Var::L}, {"I", Var::I}, {"R", Var::R}, {"AF", Var::AF}, {"BC", Var::BC}, {"DE", Var::DE}, {"HL", Var::HL},
    {"IX", Var::IX}, {"IY", Var::IY}, {"SP", Var::SP}, {"PC", Var::PC}, {"WZ", Var::WZ}, {"MEMPTR", Var::WZ},
    {"AF'", Var::AF2}, {"BC'", Var::BC2}, {"DE'", Var::DE2}, {"HL'", Var::HL2}, {"IXH", Var::IXH}, {"IXL", Var::IXL},
    {"IYH", Var::IYH}, {"IYL", Var::IYL}, {"IM", Var::IM}, {"IFF1", Var::IFF1}, {"IFF2", Var::IFF2},
    {"SF", Var::SF}, {"ZF", Var::ZF}, {"HF", Var::HF}, {"PF", Var::PF}, {"VF", Var::PF}, {"NF", Var::NF},
    {"CF", Var::CF}, {"TICKS", Var::Ticks}, {"CYCLE", Var::Cycle}, {"FRAME", Var::Frame}, {"LINE", Var::Line},
    {"COLUMN", Var::Column}, {"VALUE", Var::Value}, {"ADDRESS", Var::Address}, {"PORT", Var::Address},
}};

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::optional<Var> find_var(std::string_view name) {
    for (const VarName& v : kVars) {
        if (iequal(name, v.name)) {
            return v.var;
        }
    }
    return std::nullopt;
}

bool name_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '@'; }

bool name_char(char c) { return name_start(c) || std::isdigit(static_cast<unsigned char>(c)) || c == '?' || c == '!'; }

int digit_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    const int u = std::toupper(static_cast<unsigned char>(c));
    return (u >= 'A' && u <= 'Z') ? u - 'A' + 10 : 99;
}

}  // namespace

struct Expression::Node {
    Op op = Op::Number;
    std::int64_t value = 0;
    Var var = Var::A;
    std::optional<Bank> bank;
    std::shared_ptr<const Node> a;
    std::shared_ptr<const Node> b;
    unsigned depth = 1;  // of the tree below: evaluating and freeing it recurse that deep
};

using NodePtr = std::shared_ptr<const Expression::Node>;

namespace {

class Parser {
public:
    Parser(std::string_view text, const ParseOptions& options) : text_(text), options_(options) {}

    NodePtr parse_all() {
        NodePtr n = expression();
        skip_space();
        if (ok() && pos_ < text_.size()) {
            fail("unexpected '" + std::string(1, text_[pos_]) + "'");
        }
        return ok() ? n : nullptr;
    }

    // [BANK:] prefix; false if there is none
    std::optional<Bank> bank_prefix() {
        skip_space();
        const std::size_t start = pos_;
        if (pos_ < text_.size() && name_start(text_[pos_])) {
            const std::string_view name = read_name();
            skip_space();
            if (pos_ < text_.size() && text_[pos_] == ':') {
                if (const auto bank = parse_bank(name)) {
                    ++pos_;
                    return bank;
                }
            }
        }
        pos_ = start;
        return std::nullopt;
    }

    NodePtr expression() { return binary(0); }

    bool ok() const { return error_.message.empty(); }
    const Expression::Error& error() const { return error_; }

private:
    struct BinaryOp {
        const char* token;
        Op op;
        int level;
    };

    // Lowest level first; longer tokens before their prefixes
    static constexpr std::array<BinaryOp, 19> kBinary{{
        {"||", Op::LOr, 0}, {"&&", Op::LAnd, 1}, {"|", Op::Or, 2}, {"^", Op::Xor, 3}, {"&", Op::And, 4},
        {"==", Op::Eq, 5}, {"!=", Op::Ne, 5}, {"=", Op::Eq, 5}, {"<=", Op::Le, 6}, {">=", Op::Ge, 6},
        {"<<", Op::Shl, 7}, {">>", Op::Shr, 7}, {"<", Op::Lt, 6}, {">", Op::Gt, 6},
        {"+", Op::Add, 8}, {"-", Op::Sub, 8}, {"*", Op::Mul, 9}, {"/", Op::Div, 9}, {"%", Op::Mod, 9},
    }};
    static constexpr int kLevels = 10;
    // Hostile input must not exhaust the stack: brackets and prefix operators, and the finished tree, are bounded
    static constexpr unsigned kMaxNesting = 100;
    static constexpr unsigned kMaxDepth = 1000;

    // Counts one level of bracket or prefix-operator nesting while alive; false past the limit
    struct Nest {
        Parser& p;
        explicit Nest(Parser& parser) : p(parser) { ++p.nesting_; }
        ~Nest() { --p.nesting_; }
        bool ok() {
            if (p.nesting_ > kMaxNesting) {
                p.fail("expression nested too deeply (at most " + std::to_string(kMaxNesting) + " brackets or signs)");
                return false;
            }
            return true;
        }
    };

    void skip_space() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    void fail(std::string message) {
        if (ok()) {
            error_ = {pos_, std::move(message)};
        }
    }

    // The operator at pos_ of this level, without consuming it
    const BinaryOp* peek_binary(int level) {
        skip_space();
        const std::string_view rest = text_.substr(pos_);
        for (const BinaryOp& b : kBinary) {
            const std::string_view token = b.token;
            if (rest.substr(0, token.size()) != token) {
                continue;
            }
            // "|" must not match "||", "&" not "&&", "<" not "<<" / "<=" and so on: the longest token wins
            const BinaryOp* longest = &b;
            for (const BinaryOp& other : kBinary) {
                const std::string_view t = other.token;
                if (t.size() > std::string_view(longest->token).size() && rest.substr(0, t.size()) == t) {
                    longest = &other;
                }
            }
            return longest->level == level ? longest : nullptr;
        }
        return nullptr;
    }

    NodePtr binary(int level) {
        if (level >= kLevels) {
            return unary();
        }
        NodePtr left = binary(level + 1);
        while (ok()) {
            const BinaryOp* b = peek_binary(level);
            if (!b) {
                break;
            }
            pos_ += std::string_view(b->token).size();
            NodePtr right = binary(level + 1);
            left = make(b->op, std::move(left), std::move(right));
        }
        return left;
    }

    NodePtr unary() {
        skip_space();
        if (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == '-' || c == '!' || c == '~' || c == '+') {
                ++pos_;
                Nest nest(*this);
                if (!nest.ok()) {
                    return nullptr;
                }
                NodePtr operand = unary();
                if (c == '+') {
                    return operand;
                }
                return make(c == '-' ? Op::Neg : c == '!' ? Op::Not : Op::Compl, std::move(operand), nullptr);
            }
        }
        return primary();
    }

    NodePtr primary() {
        skip_space();
        if (pos_ >= text_.size()) {
            fail("expression expected");
            return nullptr;
        }
        const char c = text_[pos_];
        Nest nest(*this);
        if ((c == '(' || c == '[' || c == '{') && !nest.ok()) {
            return nullptr;
        }
        if (c == '(') {
            ++pos_;
            NodePtr inner = expression();
            expect(')');
            return inner;
        }
        if (c == '[' || c == '{') {
            ++pos_;
            const std::optional<Bank> bank = bank_prefix();
            NodePtr address = expression();
            expect(c == '[' ? ']' : '}');
            std::shared_ptr<Expression::Node> node = make(c == '[' ? Op::Byte : Op::Word, std::move(address), nullptr);
            if (node) {
                node->bank = bank;
            }
            return node;
        }
        if (c == '#' || c == '$' || c == '%' || std::isdigit(static_cast<unsigned char>(c))) {
            return number();
        }
        if (name_start(c)) {
            return name();
        }
        fail("unexpected '" + std::string(1, c) + "'");
        return nullptr;
    }

    void expect(char c) {
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
        } else {
            fail(std::string("'") + c + "' expected");
        }
    }

    std::string_view read_name() {
        const std::size_t start = pos_;
        while (pos_ < text_.size() && name_char(text_[pos_])) {
            ++pos_;
        }
        std::string_view n = text_.substr(start, pos_ - start);
        if (pos_ < text_.size() && text_[pos_] == '\'' &&
            (iequal(n, "AF") || iequal(n, "BC") || iequal(n, "DE") || iequal(n, "HL"))) {
            ++pos_;
            n = text_.substr(start, pos_ - start);
        }
        return n;
    }

    NodePtr name() {
        const std::size_t start = pos_;
        const std::string_view n = read_name();
        if (const auto var = find_var(n)) {
            auto node = std::make_shared<Expression::Node>();
            node->op = Op::Var;
            node->var = *var;
            return node;
        }
        if (options_.symbols) {
            if (const Symbol* s = options_.symbols->find(n)) {
                return constant(s->address);
            }
        }
        pos_ = start;
        const bool hexlike = n.find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos;
        fail("unknown name '" + std::string(n) + "'" + (hexlike ? " (write hex numbers that start with a letter as $" +
                                                                    std::string(n) + " or 0" + std::string(n) + ")"
                                                              : ""));
        return nullptr;
    }

    NodePtr number() {
        const std::size_t start = pos_;
        unsigned radix = options_.radix;
        const char c = text_[pos_];
        if (c == '#' || c == '$' || c == '%') {
            radix = c == '#' ? 10 : c == '$' ? 16 : 2;
            ++pos_;
        } else if (c == '0' && pos_ + 1 < text_.size() && (text_[pos_ + 1] == 'x' || text_[pos_ + 1] == 'X')) {
            radix = 16;
            pos_ += 2;
        }
        const std::size_t digits_start = pos_;
        while (pos_ < text_.size() && std::isalnum(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
        std::string_view digits = text_.substr(digits_start, pos_ - digits_start);
        if (radix != 2 && digits.size() > 1 && (digits.back() == 'h' || digits.back() == 'H') && c != '#' && c != '$') {
            digits.remove_suffix(1);
            radix = 16;
        }
        if (digits.empty()) {
            pos_ = start;
            fail("number expected");
            return nullptr;
        }
        std::uint64_t value = 0;
        for (const char d : digits) {
            const int v = digit_value(d);
            if (v >= static_cast<int>(radix)) {
                pos_ = start;
                fail("bad digit '" + std::string(1, d) + "' in a base-" + std::to_string(radix) + " number");
                return nullptr;
            }
            value = value * radix + static_cast<unsigned>(v);
            if (value > 0xFFFFFFFFull) {
                pos_ = start;
                fail("number too large");
                return nullptr;
            }
        }
        return constant(static_cast<std::int64_t>(value));
    }

    static NodePtr constant(std::int64_t value) {
        auto node = std::make_shared<Expression::Node>();
        node->op = Op::Number;
        node->value = value;
        return node;
    }

    std::shared_ptr<Expression::Node> make(Op op, NodePtr a, NodePtr b) {
        auto node = std::make_shared<Expression::Node>();
        node->op = op;
        node->depth = 1 + std::max(a ? a->depth : 0u, b ? b->depth : 0u);
        node->a = std::move(a);
        node->b = std::move(b);
        if (node->depth > kMaxDepth) {
            fail("expression too long (at most " + std::to_string(kMaxDepth) + " operators deep)");
            return nullptr;
        }
        return node;
    }

    std::string_view text_;
    const ParseOptions& options_;
    std::size_t pos_ = 0;
    Expression::Error error_;
    unsigned nesting_ = 0;
};

std::int64_t var_value(Var var, const EvalContext& ctx) {
    const Machine& m = ctx.machine;
    const Z80Registers r = m.cpu().registers();
    const auto flag = [&](unsigned bit) { return static_cast<std::int64_t>((r.af >> bit) & 1); };
    switch (var) {
    case Var::A: return r.af >> 8;
    case Var::F: return r.af & 0xFF;
    case Var::B: return r.bc >> 8;
    case Var::C: return r.bc & 0xFF;
    case Var::D: return r.de >> 8;
    case Var::E: return r.de & 0xFF;
    case Var::H: return r.hl >> 8;
    case Var::L: return r.hl & 0xFF;
    case Var::I: return r.i;
    case Var::R: return r.r;
    case Var::AF: return r.af;
    case Var::BC: return r.bc;
    case Var::DE: return r.de;
    case Var::HL: return r.hl;
    case Var::IX: return r.ix;
    case Var::IY: return r.iy;
    case Var::SP: return r.sp;
    case Var::PC: return m.pc();
    case Var::WZ: return r.wz;
    case Var::AF2: return r.af2;
    case Var::BC2: return r.bc2;
    case Var::DE2: return r.de2;
    case Var::HL2: return r.hl2;
    case Var::IXH: return r.ix >> 8;
    case Var::IXL: return r.ix & 0xFF;
    case Var::IYH: return r.iy >> 8;
    case Var::IYL: return r.iy & 0xFF;
    case Var::IM: return r.im;
    case Var::IFF1: return r.iff1 ? 1 : 0;
    case Var::IFF2: return r.iff2 ? 1 : 0;
    case Var::SF: return flag(7);
    case Var::ZF: return flag(6);
    case Var::HF: return flag(4);
    case Var::PF: return flag(2);
    case Var::NF: return flag(1);
    case Var::CF: return flag(0);
    case Var::Ticks: return static_cast<std::int64_t>(m.ticks());
    case Var::Cycle: return static_cast<std::int64_t>(m.ticks() / kTicksPerTState);
    case Var::Frame: return static_cast<std::int64_t>(m.frame_count());
    case Var::Line: return m.video().beam_line();
    case Var::Column: return m.video().beam_column();
    case Var::Value: return ctx.hit ? ctx.hit->value : 0;
    case Var::Address: return ctx.hit ? ctx.hit->address : 0;
    }
    return 0;
}

std::uint8_t read_byte(const EvalContext& ctx, std::optional<Bank> bank, std::int64_t address) {
    const auto a = static_cast<std::uint16_t>(address);
    if (!bank) {
        return ctx.machine.peek(a);
    }
    const std::span<const std::uint8_t> bytes = ctx.machine.memory().bank_bytes(*bank);
    return a < bytes.size() ? bytes[a] : MemoryMap::kOpenBus;
}

std::int64_t eval(const Expression::Node* n, const EvalContext& ctx) {
    if (!n) {
        return 0;
    }
    switch (n->op) {
    case Op::Number:
        return n->value;
    case Op::Var:
        return var_value(n->var, ctx);
    case Op::Byte:
        return read_byte(ctx, n->bank, eval(n->a.get(), ctx));
    case Op::Word: {
        const std::int64_t a = eval(n->a.get(), ctx);
        return read_byte(ctx, n->bank, a) | read_byte(ctx, n->bank, static_cast<std::uint16_t>(a + 1)) << 8;
    }
    case Op::Neg:
        return -eval(n->a.get(), ctx);
    case Op::Not:
        return eval(n->a.get(), ctx) == 0 ? 1 : 0;
    case Op::Compl:
        return ~eval(n->a.get(), ctx);
    case Op::LAnd:
        return (eval(n->a.get(), ctx) != 0 && eval(n->b.get(), ctx) != 0) ? 1 : 0;
    case Op::LOr:
        return (eval(n->a.get(), ctx) != 0 || eval(n->b.get(), ctx) != 0) ? 1 : 0;
    default:
        break;
    }
    const std::int64_t a = eval(n->a.get(), ctx);
    const std::int64_t b = eval(n->b.get(), ctx);
    switch (n->op) {
    case Op::Mul: return a * b;
    case Op::Div: return b == 0 ? 0 : a / b;
    case Op::Mod: return b == 0 ? 0 : a % b;
    case Op::Add: return a + b;
    case Op::Sub: return a - b;
    case Op::Shl: return (b < 0 || b > 62) ? 0 : a << b;
    case Op::Shr: return (b < 0 || b > 62) ? 0 : a >> b;
    case Op::Lt: return a < b ? 1 : 0;
    case Op::Le: return a <= b ? 1 : 0;
    case Op::Gt: return a > b ? 1 : 0;
    case Op::Ge: return a >= b ? 1 : 0;
    case Op::Eq: return a == b ? 1 : 0;
    case Op::Ne: return a != b ? 1 : 0;
    case Op::And: return a & b;
    case Op::Xor: return a ^ b;
    case Op::Or: return a | b;
    default: return 0;
    }
}

bool is_constant(const Expression::Node* n) {
    if (!n) {
        return true;
    }
    if (n->op == Op::Var || n->op == Op::Byte || n->op == Op::Word) {
        return false;
    }
    return is_constant(n->a.get()) && is_constant(n->b.get());
}

}  // namespace

std::pair<std::optional<Expression>, Expression::Error> Expression::parse(std::string_view text, const ParseOptions& options) {
    Parser parser(text, options);
    NodePtr root = parser.parse_all();
    if (!parser.ok()) {
        return {std::nullopt, parser.error()};
    }
    Expression e;
    e.text_ = std::string(text);
    e.root_ = std::move(root);
    return {std::move(e), {}};
}

std::int64_t Expression::evaluate(const EvalContext& context) const { return eval(root_.get(), context); }

bool Expression::constant() const { return is_constant(root_.get()); }

std::optional<Bank> parse_bank(std::string_view name) {
    for (std::size_t i = 0; i < kBankCount; ++i) {
        const auto bank = static_cast<Bank>(i);
        if (iequal(name, bank_name(bank))) {
            return bank;
        }
    }
    return std::nullopt;
}

std::pair<std::optional<AddressSpec>, Expression::Error> parse_address(std::string_view text, const ParseOptions& options) {
    Parser prefix(text, options);
    const std::optional<Bank> bank = prefix.bank_prefix();
    const std::size_t colon = bank ? text.find(':') + 1 : 0;
    auto [expression, error] = Expression::parse(text.substr(colon), options);
    if (!expression) {
        error.position += colon;
        return {std::nullopt, error};
    }
    return {AddressSpec{bank, std::move(*expression)}, {}};
}

std::function<bool(const Machine&, const Hit&)> make_condition(const Expression& expression) {
    return [expression](const Machine& machine, const Hit& hit) { return expression.evaluate({machine, &hit}) != 0; };
}

}  // namespace freetvc::debug
