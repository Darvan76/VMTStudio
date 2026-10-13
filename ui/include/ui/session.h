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
#include "vtf/vtf_reader.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ui {

// Returns the game-relative texture path (no "materials/" prefix, no ".vtf" extension)
// for a VTF file path. If the path contains "materials/", the relative path within
// materials/ is returned; otherwise, the file stem is returned.
std::string vtf_texture_path(const std::filesystem::path& path);

// Detects the material root directory for a given VMT file path:
// - If any ancestor folder in the path is named "materials" (case-insensitive),
//   returns the directory containing that "materials" folder (the ancestor's parent).
// - If no ancestor is named "materials", but the file directory (or an ancestor)
//   contains a "materials" subdirectory on disk, returns that directory.
// - Otherwise, returns the directory containing the file itself (path.parent_path()).
// Returns an empty path if path is empty.
std::filesystem::path detect_material_root(const std::filesystem::path& path);

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

    // Replaces the shader name of the current document, leaving every
    // parameter in place: the parameter panel is rebuilt from the new shader's
    // schema, and parameters the new shader does not describe simply stop
    // matching (they become "Unrecognized" rows, never deletions). One undo
    // step, exactly like a parameter edit.
    vmt::Result<void> set_shader(std::string_view name);

    // Writes the current text atomically (temp file + rename). Fails with
    // ParseError while the text is invalid; fails with IoError when the file
    // cannot be written.
    vmt::Result<void> save();
    vmt::Result<void> save_as(const std::filesystem::path& path);

    // --- Edit history (Phase 10) ---------------------------------------------
    // Every mutation (set_text / set_parameter) records an undo step.
    // Consecutive set_text calls (typing in the raw editor) coalesce into one
    // step; panel edits are always separate. History is cleared when a new
    // file/material is opened.

    bool can_undo() const noexcept { return !undo_stack_.empty(); }
    bool can_redo() const noexcept { return !redo_stack_.empty(); }
    vmt::Result<void> undo();
    vmt::Result<void> redo();

    // Restores the most recently saved (or, before the first save, opened)
    // revision. The restore itself is undoable.
    bool can_revert() const noexcept { return text_ != saved_text_; }
    vmt::Result<void> revert();

    bool has_document() const noexcept { return document_ != nullptr; }
    bool text_is_valid() const noexcept { return valid_; }

    // Null until the first successful parse.
    const vmt::Document* document() const noexcept { return document_.get(); }
    const std::string& text() const noexcept { return text_; }
    const std::filesystem::path& path() const noexcept { return path_; }
    bool has_path() const noexcept { return !path_.empty(); }
    const std::filesystem::path& material_root() const noexcept { return material_root_; }
    void set_material_root(std::filesystem::path root) { material_root_ = std::move(root); }
    bool is_vtf() const noexcept { return is_vtf_; }
    const vtf::VtfInfo* vtf_info() const noexcept { return is_vtf_ ? &vtf_info_ : nullptr; }
    std::string display_name() const;
    bool dirty() const noexcept;

    // Error from the most recent failed operation; code == Ok when none.
    const vmt::Error& last_error() const noexcept { return last_error_; }

private:
    static constexpr std::size_t kMaxHistory = 128;
    static constexpr std::chrono::milliseconds kTypingCoalesce{ 800 };

    vmt::Result<void> apply_parsed(std::string text, const std::string& name, bool keep_text_on_error);
    void push_undo(std::string text);
    vmt::Result<void> restore(std::string text);
    void clear_history();

    std::filesystem::path path_;
    std::filesystem::path material_root_;
    std::string virtual_name_; // display label when path_ is empty
    std::string text_;
    std::string saved_text_;
    std::unique_ptr<vmt::Document> document_;
    bool valid_ = false;
    bool is_vtf_ = false;
    vtf::VtfInfo vtf_info_{};
    vmt::Error last_error_;

    std::vector<std::string> undo_stack_;
    std::vector<std::string> redo_stack_;
    std::chrono::steady_clock::time_point last_text_edit_time_{};
    bool coalesce_text_edit_ = false;
};

} // namespace ui
