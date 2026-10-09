#pragma once

// ui session - headless editing state shared by the parameter panel and the
// raw text editor. This header is Windows-free and can be unit tested.
//
// Invariant: `text()` is always the exact buffer the user sees. When it parses,
// `document()` is valid and `text() == document()->source()`; when it does not,
// the last good document is kept (for the panel) and `text_is_valid()` is
// false so saving is refused until the syntax is fixed.

#include "vmt/document.h"
#include "vmt/result.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace ui {

class EditorSession {
public:
    EditorSession() = default;

    // Loads a VMT file from disk. The session is left untouched on failure.
    vmt::Result<void> open_file(const std::filesystem::path& path);

    // Opens a material that lives in the game file system (a VPK entry, for
    // example): `text` is the VMT source and `display_name` the label shown in
    // the title bar ("brick/brickwall001a.vmt"). The session has no save
    // target, so Save falls back to Save As; the engine preview still writes
    // the edited copy to the preview override path. On parse failure the
    // previous session is kept.
    vmt::Result<void> open_text(std::string text, std::string display_name);

    // Replaces the session with a document parsed from text. On parse failure
    // nothing changes except last_error(): the previous document is kept and
    // the text is remembered so the editor never loses the user's input.
    vmt::Result<void> set_text(std::string text, std::string name = {});

    // Single-parameter edit coming from the parameter panel. The document
    // replaces only the value bytes (existing parameter) or inserts one line
    // (new parameter), then text() follows the document.
    vmt::Result<void> set_parameter(std::string_view name, std::string_view value_text);

    // Writes the current text. Fails with ParseError while the text is
    // invalid; fails with IoError when the file cannot be written.
    vmt::Result<void> save();
    vmt::Result<void> save_as(const std::filesystem::path& path);

    bool has_document() const noexcept { return document_ != nullptr; }
    bool text_is_valid() const noexcept { return valid_; }

    // Null until the first successful parse.
    const vmt::Document* document() const noexcept { return document_.get(); }
    const std::string& text() const noexcept { return text_; }
    const std::filesystem::path& path() const noexcept { return path_; }
    std::string display_name() const;
    bool dirty() const noexcept;

    // Error from the most recent failed operation; code == Ok when none.
    const vmt::Error& last_error() const noexcept { return last_error_; }

private:
    vmt::Result<void> apply_parsed(std::string text, const std::string& name, bool keep_text_on_error);

    std::filesystem::path path_;
    std::string virtual_name_; // display label when path_ is empty
    std::string text_;
    std::string saved_text_;
    std::unique_ptr<vmt::Document> document_;
    bool valid_ = false;
    vmt::Error last_error_;
};

} // namespace ui
