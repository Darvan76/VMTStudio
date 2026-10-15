#include "vmt/document.h"

#include "internal.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vmt {

class LineTable {
public:
    explicit LineTable(std::string_view s) {
        line_starts_.reserve(s.size() / 32 + 4);
        line_starts_.push_back(0);
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\n') {
                line_starts_.push_back(static_cast<std::uint32_t>(i + 1));
            }
        }
    }

    SourceLocation loc_of(std::size_t off, std::size_t buffer_size) const noexcept {
        const std::uint32_t clamped = static_cast<std::uint32_t>(off < buffer_size ? off : buffer_size);
        auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), clamped);
        --it;
        const auto line = static_cast<std::uint32_t>(std::distance(line_starts_.begin(), it) + 1);
        const auto col = clamped - *it + 1;
        return { line, col };
    }

private:
    std::vector<std::uint32_t> line_starts_;
};

namespace {

constexpr auto make_bare_delimiter_table() {
    std::array<std::uint8_t, 256> table{};
    table[' '] = 1;
    table['\t'] = 1;
    table['\v'] = 1;
    table['\f'] = 1;
    table['\r'] = 1;
    table['\n'] = 1;
    table['"'] = 1;
    table['{'] = 1;
    table['}'] = 1;
    table['/'] = 2;
    return table;
}
alignas(64) constexpr auto kBareDelimiters = make_bare_delimiter_table();

} // namespace

struct Token {
    enum class Kind { LBrace, RBrace, String, Bare, Comment, Newline, End };
    Kind kind = Kind::End;
    TextSpan span{};
    bool string_terminated = true;
    bool block_comment_terminated = true;
};

class TokenStream {
public:
    TokenStream(const std::string& s, std::size_t start) : s_(s), i_(start) { advance(); }

    const Token& peek() const { return cur_; }
    Token take() { Token t = cur_; advance(); return t; }

private:
    void advance() { cur_ = scan(); }
    Token scan();

    const std::string& s_;
    std::size_t i_;
    Token cur_{};
};

Token TokenStream::scan() {
    Token t;
    const std::size_t n = s_.size();
    while (i_ < n) {
        const char c = s_[i_];
        if (c == '\r' || c == '\n') {
            const std::size_t start = i_;
            if (c == '\r' && i_ + 1 < n && s_[i_ + 1] == '\n') i_ += 2;
            else i_ += 1;
            t.kind = Token::Kind::Newline;
            t.span = { (std::uint32_t)start, (std::uint32_t)(i_ - start) };
            return t;
        }
        if (detail::is_hspace(c)) { ++i_; continue; }
        if (c == '{') { t.kind = Token::Kind::LBrace; t.span = { (std::uint32_t)i_, 1 }; ++i_; return t; }
        if (c == '}') { t.kind = Token::Kind::RBrace; t.span = { (std::uint32_t)i_, 1 }; ++i_; return t; }
        if (c == '"') {
            const std::size_t start = i_;
            ++i_;
            bool terminated = false;
            while (i_ < n) {
                const char d = s_[i_];
                if (d == '\\' && i_ + 1 < n) { i_ += 2; continue; }
                if (d == '"') { ++i_; terminated = true; break; }
                if (d == '\r' || d == '\n') break;
                ++i_;
            }
            t.kind = Token::Kind::String;
            t.span = { (std::uint32_t)start, (std::uint32_t)(i_ - start) };
            t.string_terminated = terminated;
            return t;
        }
        if (c == '/' && i_ + 1 < n && s_[i_ + 1] == '/') {
            const std::size_t start = i_;
            i_ += 2;
            while (i_ < n && s_[i_] != '\n' && s_[i_] != '\r') ++i_;
            t.kind = Token::Kind::Comment;
            t.span = { (std::uint32_t)start, (std::uint32_t)(i_ - start) };
            return t;
        }
        if (c == '/' && i_ + 1 < n && s_[i_ + 1] == '*') {
            const std::size_t start = i_;
            i_ += 2;
            bool terminated = false;
            while (i_ < n) {
                if (s_[i_] == '*' && i_ + 1 < n && s_[i_ + 1] == '/') { i_ += 2; terminated = true; break; }
                ++i_;
            }
            t.kind = Token::Kind::Comment;
            t.span = { (std::uint32_t)start, (std::uint32_t)(i_ - start) };
            t.block_comment_terminated = terminated;
            return t;
        }
        {
            const std::size_t start = i_;
            while (i_ < n) {
                const std::uint8_t type = kBareDelimiters[static_cast<unsigned char>(s_[i_])];
                if (type == 1) break;
                if (type == 2 && i_ + 1 < n) {
                    const char next = s_[i_ + 1];
                    if (next == '/' || next == '*') break;
                }
                ++i_;
            }
            if (i_ == start) { ++i_; }
            t.kind = Token::Kind::Bare;
            t.span = { (std::uint32_t)start, (std::uint32_t)(i_ - start) };
            return t;
        }
    }
    t.kind = Token::Kind::End;
    t.span = { (std::uint32_t)n, 0 };
    return t;
}

namespace {

std::string_view token_raw(const std::string& s, const Token& t) {
    return std::string_view(s).substr(t.span.offset, t.span.length);
}

bool token_quoted(const std::string& s, const Token& t) {
    return t.kind == Token::Kind::String && t.span.length >= 1 && s[t.span.offset] == '"';
}

std::string decode_token(const std::string& s, const Token& t) {
    if (t.kind == Token::Kind::String) return detail::decode_quoted(token_raw(s, t));
    return std::string(token_raw(s, t));
}

bool next_significant_is_lbrace(const std::string& s, std::size_t from) {
    TokenStream probe(s, from);
    for (;;) {
        const Token& t = probe.peek();
        if (t.kind == Token::Kind::Newline || t.kind == Token::Kind::Comment) { probe.take(); continue; }
        return t.kind == Token::Kind::LBrace;
    }
}

std::size_t line_start_of(const std::string& s, std::size_t off) {
    if (off > s.size()) off = s.size();
    while (off > 0 && s[off - 1] != '\n') --off;
    return off;
}

std::string leading_indent(const std::string& s, std::size_t off) {
    const std::size_t ls = line_start_of(s, off);
    std::size_t j = ls;
    while (j < off && detail::is_hspace(s[j])) ++j;
    return s.substr(ls, j - ls);
}

bool uses_crlf(const std::string& s) { return s.find("\r\n") != std::string::npos; }
bool uses_tabs(const std::string& s) { return s.find('\t') != std::string::npos; }

void append_escaped(std::string& o, std::string_view v) {
    for (const char c : v) {
        if (c == '\\' || c == '"') o.push_back('\\');
        o.push_back(c);
    }
}

bool value_needs_quoting(std::string_view v) noexcept {
    if (v.empty()) return true;
    for (const char c : v) {
        if (kBareDelimiters[static_cast<unsigned char>(c)] != 0) {
            return true;
        }
    }
    return false;
}

std::string format_key(std::string_view name, bool quoted) {
    if (!quoted) return std::string(name);
    std::string o;
    o.reserve(name.size() + 4);
    o.push_back('"');
    append_escaped(o, name);
    o.push_back('"');
    return o;
}

std::string format_value_token(std::string_view value_text, bool quoted) {
    if (quoted || value_needs_quoting(value_text)) {
        std::string o;
        o.reserve(value_text.size() + 4);
        o.push_back('"');
        append_escaped(o, value_text);
        o.push_back('"');
        return o;
    }
    return std::string(value_text);
}

BlockKind classify_block(std::string_view key, bool parent_is_proxies) {
    if (!key.empty()) {
        const char c0 = key[0];
        if (c0 == '>' || c0 == '<' || c0 == '=' || c0 == '!') return BlockKind::Conditional;
        if (c0 == '$') {
            bool has_upper = false;
            for (std::size_t i = 1; i < key.size(); ++i) {
                const char c = key[i];
                if (c >= 'a' && c <= 'z') {
                    has_upper = false;
                    break;
                }
                if (c >= 'A' && c <= 'Z') has_upper = true;
            }
            if (has_upper) return BlockKind::Conditional;
        }
    }
    if (detail::iequals(key, "proxies")) return BlockKind::Proxies;
    if (parent_is_proxies) return BlockKind::Proxy;
    return BlockKind::Nested;
}

} // namespace

Result<Document> Document::parse(std::string source, std::string name) {
    Document doc;
    doc.buffer_ = std::move(source);
    doc.original_ = doc.buffer_;
    doc.name_ = std::move(name);
    doc.rebuild();
    return Ok(std::move(doc));
}

void Document::rebuild() {
    nodes_.clear();
    diagnostics_.clear();
    shader_.clear();
    shader_quoted_ = false;
    shader_span_ = TextSpan{};
    shader_location_ = SourceLocation{};
    body_close_brace_ = TextSpan{};

    LineTable line_table(buffer_);
    line_table_ = &line_table;

    struct ScopedReset {
        const LineTable*& ptr;
        ~ScopedReset() { ptr = nullptr; }
    } reset_lt{ line_table_ };

    std::size_t start = 0;
    if (buffer_.size() >= 3 &&
        static_cast<unsigned char>(buffer_[0]) == 0xEF &&
        static_cast<unsigned char>(buffer_[1]) == 0xBB &&
        static_cast<unsigned char>(buffer_[2]) == 0xBF) {
        start = 3;
    }

    TokenStream ts(buffer_, start);

    for (;;) {
        const Token& t = ts.peek();
        if (t.kind == Token::Kind::Newline || t.kind == Token::Kind::Comment) { ts.take(); continue; }
        break;
    }

    const Token& first = ts.peek();
    if (first.kind == Token::Kind::End) {
        add_diag(ErrorCode::ParseError, "empty document", first.span);
        return;
    }
    if (first.kind == Token::Kind::LBrace) {
        add_diag(ErrorCode::ParseError, "missing shader name before '{'", first.span);
    } else {
        Token name_tok = ts.take();
        shader_span_ = name_tok.span;
        shader_ = decode_token(buffer_, name_tok);
        shader_quoted_ = token_quoted(buffer_, name_tok);
        shader_location_ = line_table_->loc_of(name_tok.span.offset, buffer_.size());
        if (name_tok.kind == Token::Kind::String && !name_tok.string_terminated) {
            add_diag(ErrorCode::UnterminatedString, "unterminated shader name string", name_tok.span);
        }
    }

    for (;;) {
        const Token& t = ts.peek();
        if (t.kind == Token::Kind::Newline || t.kind == Token::Kind::Comment) { ts.take(); continue; }
        break;
    }

    if (ts.peek().kind == Token::Kind::LBrace) {
        ts.take();
        Token close;
        nodes_ = parse_body(ts, false, close, 0);
        body_close_brace_ = close.span;
    } else {
        add_diag(ErrorCode::ParseError, "missing '{' after shader name", ts.peek().span);
    }
}

void Document::reparse() {
    rebuild();
    edited_ = (buffer_ != original_);
}

void Document::add_diag(ErrorCode code, std::string message, TextSpan span) {
    Diagnostic d;
    d.code = code;
    d.message = std::move(message);
    d.span = span;
    d.location = line_table_ != nullptr
        ? line_table_->loc_of(span.offset, buffer_.size())
        : detail::loc_of(buffer_, span.offset);
    diagnostics_.push_back(std::move(d));
}

std::vector<Node> Document::parse_body(TokenStream& ts, bool parent_is_proxies, Token& out_close, int depth) {
    std::vector<Node> out;
    out.reserve(depth == 0 ? std::min<std::size_t>(64, std::max<std::size_t>(16, buffer_.size() / 40)) : 8);

    if (depth > 1024) {
        add_diag(ErrorCode::ParseError, "block nesting too deep", ts.peek().span);
        out_close = ts.peek();
        while (ts.peek().kind != Token::Kind::End) ts.take();
        return out;
    }

    for (;;) {
        const Token& t = ts.peek();

        if (t.kind == Token::Kind::End) {
            add_diag(ErrorCode::UnterminatedBlock, "unterminated block: missing '}'", t.span);
            out_close = t;
            return out;
        }
        if (t.kind == Token::Kind::RBrace) {
            out_close = ts.take();
            return out;
        }
        if (t.kind == Token::Kind::Newline) {
            const std::uint32_t start_off = t.span.offset;
            std::uint32_t end_off = t.span.end();
            std::size_t extra = 0;
            while (ts.peek().kind == Token::Kind::Newline) {
                end_off = ts.take().span.end();
                ++extra;
            }
            if (extra >= 1) { // two or more consecutive newlines => at least one blank line
                Node n;
                n.kind_ = NodeKind::BlankLine;
                n.span_ = { start_off, end_off - start_off };
                const auto loc = line_table_ != nullptr
                    ? line_table_->loc_of(start_off, buffer_.size())
                    : detail::loc_of(buffer_, start_off);
                n.line_ = loc.line;
                n.column_ = loc.column;
                out.push_back(std::move(n));
            }
            continue;
        }
        if (t.kind == Token::Kind::Comment) {
            Token c = ts.take();
            Node n;
            n.kind_ = NodeKind::Comment;
            n.span_ = c.span;
            n.value_ = std::string(token_raw(buffer_, c));
            const auto loc = line_table_ != nullptr
                ? line_table_->loc_of(c.span.offset, buffer_.size())
                : detail::loc_of(buffer_, c.span.offset);
            n.line_ = loc.line;
            n.column_ = loc.column;
            if (!c.block_comment_terminated) {
                add_diag(ErrorCode::ParseError, "unterminated block comment", c.span);
            }
            out.push_back(std::move(n));
            continue;
        }
        if (t.kind == Token::Kind::LBrace) {
            Token brace = ts.take();
            add_diag(ErrorCode::UnexpectedToken, "unexpected '{' without a key", brace.span);
            Token close;
            auto children = parse_body(ts, false, close, depth + 1);
            Node n;
            n.kind_ = NodeKind::Block;
            n.block_kind_ = BlockKind::Nested;
            n.close_brace_span_ = close.span;
            n.children_ = std::move(children);
            n.span_ = { brace.span.offset, close.span.end() - brace.span.offset };
            const auto loc = line_table_ != nullptr
                ? line_table_->loc_of(brace.span.offset, buffer_.size())
                : detail::loc_of(buffer_, brace.span.offset);
            n.line_ = loc.line;
            n.column_ = loc.column;
            out.push_back(std::move(n));
            continue;
        }

        Token key = ts.take();
        Node n;
        n.key_span_ = key.span;
        n.key_ = decode_token(buffer_, key);
        const auto kloc = line_table_ != nullptr
            ? line_table_->loc_of(key.span.offset, buffer_.size())
            : detail::loc_of(buffer_, key.span.offset);
        n.line_ = kloc.line;
        n.column_ = kloc.column;
        if (key.kind == Token::Kind::String && !key.string_terminated) {
            add_diag(ErrorCode::UnterminatedString, "unterminated key string", key.span);
        }

        const Token& v = ts.peek();

        if (v.kind == Token::Kind::LBrace) {
            ts.take();
            Token close;
            const bool child_proxies = detail::iequals(n.key_, "proxies");
            n.kind_ = NodeKind::Block;
            n.block_kind_ = classify_block(n.key_, parent_is_proxies);
            n.children_ = parse_body(ts, child_proxies, close, depth + 1);
            n.close_brace_span_ = close.span;
            n.span_ = { key.span.offset, close.span.end() - key.span.offset };
            out.push_back(std::move(n));
            continue;
        }
        if (v.kind == Token::Kind::String || v.kind == Token::Kind::Bare) {
            Token val = ts.take();
            n.kind_ = NodeKind::Parameter;
            n.value_span_ = val.span;
            n.value_ = decode_token(buffer_, val);
            n.span_ = { key.span.offset, val.span.end() - key.span.offset };
            if (val.kind == Token::Kind::String && !val.string_terminated) {
                add_diag(ErrorCode::UnterminatedString, "unterminated value string", val.span);
            }
            out.push_back(std::move(n));
            continue;
        }

        if (next_significant_is_lbrace(buffer_, v.span.offset)) {
            while (ts.peek().kind == Token::Kind::Newline || ts.peek().kind == Token::Kind::Comment) ts.take();
            ts.take(); // '{'
            Token close;
            const bool child_proxies = detail::iequals(n.key_, "proxies");
            n.kind_ = NodeKind::Block;
            n.block_kind_ = classify_block(n.key_, parent_is_proxies);
            n.children_ = parse_body(ts, child_proxies, close, depth + 1);
            n.close_brace_span_ = close.span;
            n.span_ = { key.span.offset, close.span.end() - key.span.offset };
            out.push_back(std::move(n));
            continue;
        }

        n.kind_ = NodeKind::Parameter;
        n.span_ = key.span;
        add_diag(ErrorCode::UnexpectedToken, "parameter has no value", key.span);
        out.push_back(std::move(n));
    }
}

const Node* Document::find(std::string_view name) const noexcept {
    for (const auto& n : nodes_) {
        if (n.kind_ == NodeKind::Parameter && detail::iequals(n.key_, name)) return &n;
    }
    return nullptr;
}

bool Document::has(std::string_view name) const noexcept {
    return find(name) != nullptr;
}

const Node* Document::find_block(std::string_view name) const noexcept {
    for (const auto& n : nodes_) {
        if (n.kind_ == NodeKind::Block && detail::iequals(n.key_, name)) return &n;
    }
    return nullptr;
}

std::vector<const Node*> Document::parameters() const {
    std::vector<const Node*> out;
    out.reserve(nodes_.size());
    for (const auto& n : nodes_) {
        if (n.kind_ == NodeKind::Parameter) out.push_back(&n);
    }
    return out;
}

Result<void> Document::replace_value(const Node& node, std::string_view value_text) {
    const bool was_quoted = (node.value_span_.length >= 1) && buffer_[node.value_span_.offset] == '"';
    const std::string token = format_value_token(value_text, was_quoted);
    if (node.value_span_.length > 0) {
        buffer_.replace(node.value_span_.offset, node.value_span_.length, token);
    } else {
        std::string ins;
        ins.reserve(1 + token.size());
        ins.push_back(' ');
        ins.append(token);
        buffer_.insert(node.key_span_.end(), ins);
    }
    reparse();
    return Ok();
}

Result<void> Document::insert_parameter(std::string_view name, std::string_view value_text) {
    if (body_close_brace_.length == 0) {
        return Err(make_error(ErrorCode::ParseError,
            "cannot add a parameter: the shader body has no closing brace",
            "Fix the malformed material or add the parameter manually", name_, 0, 0));
    }
    const std::size_t close = body_close_brace_.offset;

    const Node* last_param = nullptr;
    for (const auto& n : nodes_) {
        if (n.kind_ == NodeKind::Parameter) last_param = &n;
    }

    std::string indent;
    std::string sep = " ";
    bool quoted = shader_quoted_;

    if (last_param != nullptr) {
        indent = leading_indent(buffer_, last_param->key_span_.offset);
        if (last_param->value_span_.length > 0) {
            sep = buffer_.substr(last_param->key_span_.end(),
                                 last_param->value_span_.offset - last_param->key_span_.end());
        }
        quoted = (last_param->key_span_.length >= 1) && buffer_[last_param->key_span_.offset] == '"';
    } else {
        indent = uses_tabs(buffer_) ? "\t" : "    ";
    }

    const std::string eol = uses_crlf(buffer_) ? "\r\n" : "\n";
    const std::size_t at = line_start_of(buffer_, close);
    const std::string key_tok = format_key(name, quoted);
    const std::string val_tok = format_value_token(value_text, quoted);
    std::string line;
    line.reserve(indent.size() + key_tok.size() + sep.size() + val_tok.size() + eol.size());
    line.append(indent);
    line.append(key_tok);
    line.append(sep);
    line.append(val_tok);
    line.append(eol);
    buffer_.insert(at, line);
    reparse();
    return Ok();
}

Result<void> Document::set_parameter(std::string_view name, std::string_view value_text) {
    const Node* existing = find(name);
    if (existing != nullptr) {
        if (existing->kind_ != NodeKind::Parameter) {
            return Err(make_error(ErrorCode::InvalidArgument,
                "parameter '" + std::string(name) + "' is a block, not a value"));
        }
        return replace_value(*existing, value_text);
    }
    return insert_parameter(name, value_text);
}

Result<void> Document::remove_parameter(std::string_view name) {
    const Node* n = find(name);
    if (n == nullptr) {
        return Err(make_error(ErrorCode::NotFound, "parameter '" + std::string(name) + "' not found"));
    }

    const std::size_t ls = line_start_of(buffer_, n->span_.offset);
    std::size_t erase_start = ls;
    for (std::size_t i = ls; i < n->span_.offset; ++i) {
        if (!detail::is_hspace(buffer_[i])) { erase_start = n->span_.offset; break; }
    }

    std::size_t erase_end = n->span_.end();
    while (erase_end < buffer_.size() && buffer_[erase_end] != '\n' && buffer_[erase_end] != '\r') ++erase_end;
    if (erase_end < buffer_.size()) {
        if (buffer_[erase_end] == '\r' && erase_end + 1 < buffer_.size() && buffer_[erase_end + 1] == '\n') erase_end += 2;
        else erase_end += 1;
    }

    buffer_.erase(erase_start, erase_end - erase_start);
    reparse();
    return Ok();
}

Result<void> Document::set_shader(std::string_view name) {
    if (shader_span_.length == 0) {
        return Err(make_error(ErrorCode::ParseError,
            "cannot change the shader: the document has no shader name",
            "Fix the syntax error first, then pick the shader again", name_));
    }
    if (name.empty()) {
        return Err(make_error(ErrorCode::InvalidArgument, "the shader name cannot be empty",
            "Choose a shader such as VertexLitGeneric"));
    }
    for (const char c : name) {
        if (detail::is_hspace(c) || c == '\r' || c == '\n' || c == '{' || c == '}' ||
            c == '"' || c == '/' || c == '\\') {
            return Err(make_error(ErrorCode::InvalidArgument,
                "'" + std::string(name) + "' is not a valid shader name",
                "A shader name is a single word, e.g. VertexLitGeneric"));
        }
    }

    // Keep the document's own spelling: a quoted name stays quoted, a bare one
    // stays bare. format_value_token only quotes bare text that needs it, which
    // a shader name validated above never does.
    const bool quoted = shader_quoted_ ||
                        (shader_span_.length >= 1 && buffer_[shader_span_.offset] == '"');
    buffer_.replace(shader_span_.offset, shader_span_.length,
                    format_value_token(name, quoted));
    reparse();
    return Ok();
}

Result<void> Document::revert_edits() {
    buffer_ = original_;
    reparse();
    return Ok();
}

Result<int> Document::get_int(std::string_view name) const {
    const Node* n = find(name);
    if (n == nullptr) return Result<int>(make_error(ErrorCode::NotFound, "parameter '" + std::string(name) + "' not found"));
    return parse_int(n->value());
}

Result<float> Document::get_float(std::string_view name) const {
    const Node* n = find(name);
    if (n == nullptr) return Result<float>(make_error(ErrorCode::NotFound, "parameter '" + std::string(name) + "' not found"));
    return parse_float(n->value());
}

Result<bool> Document::get_bool(std::string_view name) const {
    const Node* n = find(name);
    if (n == nullptr) return Result<bool>(make_error(ErrorCode::NotFound, "parameter '" + std::string(name) + "' not found"));
    return parse_bool(n->value());
}

Result<Vec3> Document::get_vec3(std::string_view name) const {
    const Node* n = find(name);
    if (n == nullptr) return Result<Vec3>(make_error(ErrorCode::NotFound, "parameter '" + std::string(name) + "' not found"));
    return parse_vec3(n->value());
}

Result<Color> Document::get_color(std::string_view name) const {
    const Node* n = find(name);
    if (n == nullptr) return Result<Color>(make_error(ErrorCode::NotFound, "parameter '" + std::string(name) + "' not found"));
    return parse_color(n->value());
}

Result<Matrix> Document::get_matrix(std::string_view name) const {
    const Node* n = find(name);
    if (n == nullptr) return Result<Matrix>(make_error(ErrorCode::NotFound, "parameter '" + std::string(name) + "' not found"));
    return parse_matrix(n->value());
}

Result<void> Document::set_int(std::string_view name, int value) {
    const Node* n = find(name);
    if (n != nullptr && n->kind_ == NodeKind::Parameter) {
        auto cur = parse_int(n->value());
        if (cur.ok() && cur.value() == value) return Ok();
    }
    return set_parameter(name, format_int(value));
}

Result<void> Document::set_float(std::string_view name, float value) {
    const Node* n = find(name);
    if (n != nullptr && n->kind_ == NodeKind::Parameter) {
        auto cur = parse_float(n->value());
        if (cur.ok() && cur.value() == value) return Ok();
    }
    return set_parameter(name, format_float(value));
}

Result<void> Document::set_bool(std::string_view name, bool value) {
    const Node* n = find(name);
    if (n != nullptr && n->kind_ == NodeKind::Parameter) {
        auto cur = parse_bool(n->value());
        if (cur.ok() && cur.value() == value) return Ok();
    }
    return set_parameter(name, format_bool(value));
}

Result<void> Document::set_vec3(std::string_view name, const Vec3& value) {
    const Node* n = find(name);
    if (n != nullptr && n->kind_ == NodeKind::Parameter) {
        auto cur = parse_vec3(n->value());
        if (cur.ok() && cur.value() == value) return Ok();
    }
    return set_parameter(name, format_vec3(value));
}

Result<void> Document::set_color(std::string_view name, const Color& value) {
    const Node* n = find(name);
    if (n != nullptr && n->kind_ == NodeKind::Parameter) {
        auto cur = parse_color(n->value());
        if (cur.ok() && cur.value() == value) return Ok();
    }
    return set_parameter(name, format_color(value));
}

Result<void> Document::set_matrix(std::string_view name, const Matrix& value) {
    const Node* n = find(name);
    if (n != nullptr && n->kind_ == NodeKind::Parameter) {
        auto cur = parse_matrix(n->value());
        if (cur.ok() && cur.value() == value) return Ok();
    }
    return set_parameter(name, format_matrix(value));
}

} // namespace vmt
