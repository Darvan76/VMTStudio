#pragma once

#include "vmt/result.h"
#include "vmt/span.h"
#include "vmt/value.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vmt {

enum class NodeKind : std::uint8_t {
    Shader,
    Parameter,
    Block,
    Comment,
    BlankLine,
    Unknown,
};

enum class BlockKind : std::uint8_t {
    None,
    Nested,      // "$key { ... }"
    Proxies,     // "Proxies" { ... }
    Proxy,       // "ProxyName" { ... } inside a Proxies block
    Conditional, // >=DX90 { ... }, <DX90 { ... }, $WIN32 { ... }
};

struct Diagnostic {
    ErrorCode      code = ErrorCode::Ok;
    std::string    message;
    SourceLocation location;
    TextSpan       span;
};

// Internal token types (defined in document.cpp). Forward-declared only so the
// private parse helpers can be declared here.
struct Token;
class TokenStream;
class LineTable;

class Node {
public:
    Node() = default;
    Node(const Node&) = default;
    Node(Node&&) noexcept = default;
    Node& operator=(const Node&) = default;
    Node& operator=(Node&&) noexcept = default;

    NodeKind  kind() const noexcept { return kind_; }
    BlockKind block_kind() const noexcept { return block_kind_; }

    TextSpan span() const noexcept { return span_; }
    TextSpan key_span() const noexcept { return key_span_; }
    TextSpan value_span() const noexcept { return value_span_; }
    TextSpan close_brace_span() const noexcept { return close_brace_span_; }

    const std::vector<Node>& children() const noexcept { return children_; }

    // Decoded key/value (quotes removed, escapes resolved); original case kept.
    std::string_view key() const noexcept { return key_; }
    std::string_view value() const noexcept { return value_; }

    SourceLocation location() const noexcept { return SourceLocation{ line_, column_ }; }

    bool is_parameter() const noexcept { return kind_ == NodeKind::Parameter; }
    bool is_block() const noexcept { return kind_ == NodeKind::Block; }

private:
    friend class Document;

    NodeKind  kind_ = NodeKind::Unknown;
    BlockKind block_kind_ = BlockKind::None;
    TextSpan  span_{};
    TextSpan  key_span_{};
    TextSpan  value_span_{};
    TextSpan  close_brace_span_{};
    std::string key_;
    std::string value_;
    std::vector<Node> children_;
    std::uint32_t line_ = 0;
    std::uint32_t column_ = 0;
};

// A lossless VMT document. Holds the pristine source and a parse tree whose
// nodes reference byte spans. Serialization returns the current buffer; an
// unedited document serializes byte-identically to its source.
class Document {
public:
    static Result<Document> parse(std::string source, std::string name = {});

    const std::string& source() const noexcept { return buffer_; }
    const std::string& original_source() const noexcept { return original_; }
    const std::string& name() const noexcept { return name_; }
    std::string_view   shader() const noexcept { return shader_; }

    // Location of the shader name token ({0,0} when it is missing).
    SourceLocation shader_location() const noexcept { return shader_location_; }

    const std::vector<Node>& nodes() const noexcept { return nodes_; }
    const std::vector<Diagnostic>& diagnostics() const noexcept { return diagnostics_; }

    bool edited() const noexcept { return edited_; }

    std::string  serialize() const { return buffer_; }
    Result<void> revert_edits();

    // Parameter lookup at shader-body level. Case-insensitive, original case
    // preserved in Node::key().
    const Node* find(std::string_view name) const noexcept;
    bool        has(std::string_view name) const noexcept;
    const Node* find_block(std::string_view name) const noexcept;
    std::vector<const Node*> parameters() const; // body-level, in file order

    Result<void> set_parameter(std::string_view name, std::string_view value_text);
    Result<void> remove_parameter(std::string_view name);

    // Replaces the shader name that precedes the opening brace. Only the name
    // token is spliced, so the body (and its parameters) is left untouched and
    // the document stays lossless. The original quoting style is preserved.
    Result<void> set_shader(std::string_view name);

    // Typed helpers (body-level parameters).
    Result<int>    get_int(std::string_view name) const;
    Result<float>  get_float(std::string_view name) const;
    Result<bool>   get_bool(std::string_view name) const;
    Result<Vec3>   get_vec3(std::string_view name) const;
    Result<Color>  get_color(std::string_view name) const;
    Result<Matrix> get_matrix(std::string_view name) const;

    Result<void> set_int(std::string_view name, int value);
    Result<void> set_float(std::string_view name, float value);
    Result<void> set_bool(std::string_view name, bool value);
    Result<void> set_vec3(std::string_view name, const Vec3& value);
    Result<void> set_color(std::string_view name, const Color& value);
    Result<void> set_matrix(std::string_view name, const Matrix& value);

private:
    void rebuild();
    void reparse();
    void add_diag(ErrorCode code, std::string message, TextSpan span);

    std::vector<Node> parse_body(TokenStream& ts, bool parent_is_proxies, Token& out_close, int depth);

    Result<void> replace_value(const Node& node, std::string_view value_text);
    Result<void> insert_parameter(std::string_view name, std::string_view value_text);

    std::string buffer_;
    std::string original_;
    std::string name_;
    std::string shader_;
    bool        shader_quoted_ = false;
    TextSpan    shader_span_{};
    SourceLocation shader_location_{};
    TextSpan    body_close_brace_{};
    std::vector<Node> nodes_;
    std::vector<Diagnostic> diagnostics_;
    bool edited_ = false;
    const LineTable* line_table_ = nullptr;
};

} // namespace vmt
