#pragma once

// ui preview_sync - debounced editor-session -> engine-preview bridge.
//
// The Win32 shell owns the debounce timer and the engine; this class owns the
// decision state so it can be unit tested without Windows or a game install.
//
// Failure policy (Phase 7):
//  - Text that does not parse is never written: the engine keeps rendering the
//    last good material and last_error() explains why the preview is stale.
//  - If the engine rejects a written VMT, the last successfully previewed text
//    is written back (best effort) before reporting the error, so the viewport
//    keeps its last good render.
//  - The state is fixed-size: reload cycles replace the same file through the
//    same hooks and retain only the last good text.

#include "vmt/result.h"

#include <filesystem>
#include <functional>
#include <string>

namespace ui {

// The preview material name the engine resolves; its file lives at
// <preview root>/materials/<name>.vmt. Declared here (not in the Win32 shell)
// so the on-disk contract is covered by the headless tests.
inline constexpr const char* kPreviewMaterialName = "vmtviewer_preview";

// Writes `text` to <root>/materials/vmtviewer_preview.vmt through a sibling
// ".tmp" file that then replaces the target, so a crash mid-write can never
// leave a truncated VMT behind. Returns IoError with a user-facing message on
// failure. Creates the directory chain when missing.
vmt::Result<void> write_preview_material(const std::filesystem::path& root,
                                         const std::string& text);

// Same, but at the material's own game-relative path: `material_name` is
// "brick/brickwall001a" and the file lands at
// <root>/materials/brick/brickwall001a.vmt. With the preview root mounted at
// the highest priority this copy overrides the game's original material, so
// arbitrary game materials can be edited live. Names with absolute roots or
// ".." components are rejected with InvalidArgument.
vmt::Result<void> write_preview_material_at(const std::filesystem::path& root,
                                            const std::string& material_name,
                                            const std::string& text);

class PreviewSync {
public:
    // Writes the text to the mounted preview search path.
    using WriteFn = std::function<bool(const std::string& text)>;
    // Asks the engine to reload the preview material from disk.
    using ReloadFn = std::function<bool()>;

    void configure(WriteFn write, ReloadFn reload);

    // Preview started/stopped. Stopping cancels pending work and clears the
    // staleness state (the last good text is kept).
    void set_active(bool active);
    bool active() const noexcept { return active_; }

    // Records the text that is currently on screen (initial preview), so a
    // later rejected reload can be rolled back to it.
    void note_previewed(const std::string& text);

    // A document/text edit. Invalid text cancels any pending update and marks
    // the preview stale; valid text schedules a debounced update.
    // Returns true when an update is now pending.
    bool document_changed(bool valid, vmt::Error error = {});

    bool update_pending() const noexcept { return pending_; }

    // True while the newest revision is not on screen (parse error, or a
    // write/reload that failed and was rolled back).
    bool stale() const noexcept { return stale_; }
    const vmt::Error& last_error() const noexcept { return last_error_; }

    // The debounce timer elapsed: performs the pending write + reload. A no-op
    // while inactive or with nothing pending.
    vmt::Result<void> flush(const std::string& text);

    // Save / manual refresh: mirrors the current text to the engine right away,
    // ignoring the debounce. Invalid text is reported without touching the
    // engine or the last good file.
    vmt::Result<void> flush_now(const std::string& text, bool valid, vmt::Error error = {});

    // Cancels pending work and clears staleness (preview stopped, or the host
    // seeded a fresh material through the start path).
    void cancel();

private:
    // Writes `text` and asks the engine to reload; on rejection restores the
    // last good text when one is known.
    vmt::Result<void> apply_and_reload(const std::string& text);
    void mark_stale(vmt::Error error);

    WriteFn write_;
    ReloadFn reload_;
    std::string last_good_;
    vmt::Error last_error_;
    bool active_ = false;
    bool pending_ = false;
    bool stale_ = false;
};

} // namespace ui
