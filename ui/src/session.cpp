#include "ui/session.h"

#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

namespace ui {
namespace {

vmt::Error io_error(std::string message, const std::filesystem::path& path) {
    return vmt::make_error(vmt::ErrorCode::IoError, std::move(message),
                           "Check that the file exists and is writable", path.string());
}

bool is_materials_folder_name(const std::filesystem::path& name) {
    const std::string s = name.string();
    if (s.size() != 9) {
        return false;
    }
    const char expected[] = "materials";
    for (std::size_t i = 0; i < 9; ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) != expected[i]) {
            return false;
        }
    }
    return true;
}

bool ends_with_ci(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(text[text.size() - suffix.size() + i]);
        const unsigned char b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string vtf_texture_path(const std::filesystem::path& path) {
    if (path.empty()) {
        return {};
    }
    std::string generic = path.generic_string();
    std::string lower = generic;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    const std::string needle = "/materials/";
    const std::size_t pos = lower.rfind(needle);
    std::string rel;
    if (pos != std::string::npos) {
        rel = generic.substr(pos + needle.size());
    } else if (lower.rfind("materials/", 0) == 0) {
        rel = generic.substr(10);
    } else {
        rel = path.stem().generic_string();
    }

    if (ends_with_ci(rel, ".vtf")) {
        rel.erase(rel.size() - 4);
    } else if (ends_with_ci(rel, ".vmt")) {
        rel.erase(rel.size() - 4);
    }

    while (!rel.empty() && (rel.back() == '/' || rel.back() == '\\')) {
        rel.pop_back();
    }
    while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\')) {
        rel.erase(rel.begin());
    }
    return rel;
}

std::filesystem::path detect_material_root(const std::filesystem::path& path) {
    if (path.empty()) {
        return {};
    }

    const std::filesystem::path normal = path.lexically_normal();
    const std::filesystem::path file_dir = normal.parent_path();
    if (file_dir.empty()) {
        return {};
    }

    // 1. Walk up the path hierarchy to see if any ancestor folder is named "materials".
    for (std::filesystem::path p = file_dir; !p.empty() && p != p.parent_path(); p = p.parent_path()) {
        if (is_materials_folder_name(p.filename())) {
            return p.parent_path();
        }
    }

    // 2. If no ancestor is named "materials", check if any ancestor directory on disk
    // contains a "materials" subfolder.
    for (std::filesystem::path p = file_dir; !p.empty() && p != p.parent_path(); p = p.parent_path()) {
        std::error_code ec;
        if (std::filesystem::is_directory(p / L"materials", ec)) {
            return p;
        }
    }

    // 3. Otherwise, return the file directory itself.
    return file_dir;
}

vmt::Result<void> EditorSession::open_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        vmt::Error error = io_error("cannot open the file: " + path.string(), path);
        error.code = vmt::ErrorCode::NotFound;
        error.remediation = "Check the file path and that the file is readable";
        last_error_ = error;
        return vmt::Err<void>(error);
    }

    std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (!stream.good() && !stream.eof()) {
        vmt::Error error = io_error("failed to read the file: " + path.string(), path);
        last_error_ = error;
        return vmt::Err<void>(error);
    }

    const bool has_vtf_sig = content.size() >= 4 &&
                             content[0] == 'V' && content[1] == 'T' &&
                             content[2] == 'F' && content[3] == '\0';
    const bool has_vtf_ext = ends_with_ci(path.string(), ".vtf");

    if (has_vtf_sig || has_vtf_ext) {
        vmt::Result<vtf::VtfInfo> info =
            vtf::open_vtf_info_from_memory(reinterpret_cast<const std::uint8_t*>(content.data()),
                                           content.size(), path);
        if (!info) {
            last_error_ = info.error();
            return vmt::Err<void>(info.error());
        }

        const std::string tex_path = vtf_texture_path(path);
        std::string vmt_text = "\"UnlitGeneric\"\n{\n\t\"$basetexture\" \"" + tex_path + "\"\n}\n";

        const std::string name = path.filename().string();
        vmt::Result<void> result = apply_parsed(std::move(vmt_text), name, /*keep_text_on_error=*/false);
        if (!result) {
            return result;
        }

        path_ = path;
        material_root_ = detect_material_root(path_);
        virtual_name_.clear();
        saved_text_ = text_;
        is_vtf_ = true;
        vtf_info_ = info.value();
        clear_history();
        return vmt::Ok();
    }

    const std::string name = path.filename().string();
    vmt::Result<void> result = apply_parsed(std::move(content), name, /*keep_text_on_error=*/false);
    if (!result) {
        return result;
    }
    path_ = path;
    material_root_ = detect_material_root(path_);
    virtual_name_.clear();
    saved_text_ = text_;
    is_vtf_ = false;
    vtf_info_ = vtf::VtfInfo{};
    clear_history();
    return vmt::Ok();
}

vmt::Result<void> EditorSession::open_text(std::string text, std::string display_name) {
    vmt::Result<void> result =
        apply_parsed(std::move(text), display_name, /*keep_text_on_error=*/false);
    if (!result) {
        return result;
    }
    path_.clear();
    material_root_.clear();
    virtual_name_ = std::move(display_name);
    saved_text_ = text_;
    is_vtf_ = false;
    vtf_info_ = vtf::VtfInfo{};
    clear_history();
    return vmt::Ok();
}

vmt::Result<void> EditorSession::set_text(std::string text, std::string name) {
    // Typing streams one set_text per keystroke; merge those into a single
    // undo step so Ctrl+Z removes the whole burst instead of one character.
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const bool merge =
        coalesce_text_edit_ && (now - last_text_edit_time_) < kTypingCoalesce;
    if (!merge) {
        push_undo(text_);
    }
    redo_stack_.clear();
    coalesce_text_edit_ = true;
    last_text_edit_time_ = now;
    return apply_parsed(std::move(text), name, /*keep_text_on_error=*/true);
}

vmt::Result<void> EditorSession::apply_parsed(std::string text, const std::string& name,
                                              bool keep_text_on_error) {
    const std::string source = name.empty() && document_ != nullptr ? document_->name() : name;

    vmt::Result<vmt::Document> parsed = vmt::Document::parse(text, source);
    if (!parsed) {
        if (keep_text_on_error) {
            // Keep the last good document and the user's text; only report.
            text_ = std::move(text);
            valid_ = false;
        }
        last_error_ = parsed.error();
        return vmt::Err<void>(parsed.error());
    }

    document_ = std::make_unique<vmt::Document>(std::move(parsed.value()));
    text_ = document_->source();
    valid_ = true;
    last_error_ = vmt::Error{};
    return vmt::Ok();
}

vmt::Result<void> EditorSession::set_parameter(std::string_view name, std::string_view value_text) {
    if (!valid_ || document_ == nullptr) {
        vmt::Error error = vmt::make_error(
            vmt::ErrorCode::ParseError,
            "cannot edit parameters while the VMT text has syntax errors",
            "Fix the highlighted syntax error first");
        last_error_ = error;
        return vmt::Err<void>(error);
    }

    // Panel edits always end a typing burst and get their own undo step; the
    // pre-edit text is pushed only after the edit succeeded.
    const std::string previous = text_;
    vmt::Result<void> result = document_->set_parameter(name, value_text);
    if (!result) {
        last_error_ = result.error();
        return result;
    }

    push_undo(std::move(previous));
    redo_stack_.clear();
    coalesce_text_edit_ = false;
    text_ = document_->source();
    last_error_ = vmt::Error{};
    return vmt::Ok();
}

vmt::Result<void> EditorSession::set_shader(std::string_view name) {
    if (!valid_ || document_ == nullptr) {
        vmt::Error error = vmt::make_error(
            vmt::ErrorCode::ParseError,
            "cannot change the shader while the VMT text has syntax errors",
            "Fix the highlighted syntax error first");
        last_error_ = error;
        return vmt::Err<void>(error);
    }

    const std::string previous = text_;
    vmt::Result<void> result = document_->set_shader(name);
    if (!result) {
        last_error_ = result.error();
        return result;
    }

    push_undo(std::move(previous));
    redo_stack_.clear();
    coalesce_text_edit_ = false;
    text_ = document_->source();
    last_error_ = vmt::Error{};
    return vmt::Ok();
}

vmt::Result<void> EditorSession::undo() {
    if (undo_stack_.empty()) {
        vmt::Error error = vmt::make_error(vmt::ErrorCode::InvalidArgument, "there is nothing to undo",
                                           "Edit the material first");
        last_error_ = error;
        return vmt::Err<void>(error);
    }
    std::string previous = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    redo_stack_.push_back(text_);
    if (redo_stack_.size() > kMaxHistory) {
        redo_stack_.erase(redo_stack_.begin());
    }
    return restore(std::move(previous));
}

vmt::Result<void> EditorSession::redo() {
    if (redo_stack_.empty()) {
        vmt::Error error = vmt::make_error(vmt::ErrorCode::InvalidArgument, "there is nothing to redo",
                                           "Undo an edit first");
        last_error_ = error;
        return vmt::Err<void>(error);
    }
    std::string next = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    push_undo(text_);
    return restore(std::move(next));
}

vmt::Result<void> EditorSession::revert() {
    if (!can_revert()) {
        vmt::Error error = vmt::make_error(vmt::ErrorCode::InvalidArgument,
                                           "the material has no changes to revert",
                                           "Edit the material first");
        last_error_ = error;
        return vmt::Err<void>(error);
    }
    push_undo(text_);
    redo_stack_.clear();
    return restore(saved_text_);
}

vmt::Result<void> EditorSession::restore(std::string text) {
    // Restoring never fails: intermediate typing states can be unparseable and
    // must still come back exactly as the user saw them (valid_ then reflects
    // the parse outcome, like while typing).
    (void)apply_parsed(std::move(text), std::string(), /*keep_text_on_error=*/true);
    coalesce_text_edit_ = false;
    return vmt::Ok();
}

void EditorSession::push_undo(std::string text) {
    undo_stack_.push_back(std::move(text));
    if (undo_stack_.size() > kMaxHistory) {
        undo_stack_.erase(undo_stack_.begin());
    }
}

void EditorSession::clear_history() {
    undo_stack_.clear();
    redo_stack_.clear();
    coalesce_text_edit_ = false;
}

vmt::Result<void> EditorSession::save() {
    if (path_.empty() || is_vtf_) {
        vmt::Error error = vmt::make_error(vmt::ErrorCode::InvalidArgument,
                                           is_vtf_ ? "cannot overwrite a binary VTF texture file with VMT text"
                                                   : "no file path to save to",
                                           "Use Save As to choose a destination");
        last_error_ = error;
        return vmt::Err<void>(error);
    }
    return save_as(path_);
}

vmt::Result<void> EditorSession::save_as(const std::filesystem::path& path) {
    if (!valid_) {
        vmt::Error error = vmt::make_error(
            vmt::ErrorCode::ParseError,
            "cannot save while the VMT text has syntax errors",
            "Fix the syntax error shown in the status bar first");
        last_error_ = error;
        return vmt::Err<void>(error);
    }

    if (ends_with_ci(path.string(), ".vtf")) {
        vmt::Error error = vmt::make_error(
            vmt::ErrorCode::InvalidArgument,
            "cannot save VMT text with a .vtf extension",
            "Choose a .vmt file extension for the material");
        last_error_ = error;
        return vmt::Err<void>(error);
    }

    // Write to a sibling temp file, then rename over the target: a crash or a
    // full disk cannot leave a truncated VMT behind. Windows rename does not
    // replace an existing file, hence the remove + retry.
    std::filesystem::path temp = path;
    temp += L".tmp";
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream) {
            vmt::Error error = io_error("cannot write the VMT file: " + path.string(), path);
            last_error_ = error;
            return vmt::Err<void>(error);
        }
        if (!text_.empty()) {
            stream.write(text_.data(), static_cast<std::streamsize>(text_.size()));
        }
        if (!stream) {
            vmt::Error error = io_error("failed while writing the VMT file: " + path.string(), path);
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            last_error_ = error;
            return vmt::Err<void>(error);
        }
        stream.close();
        if (!stream) {
            vmt::Error error = io_error("failed to close the VMT file: " + path.string(), path);
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            last_error_ = error;
            return vmt::Err<void>(error);
        }
    }

    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        ec.clear();
        std::filesystem::rename(temp, path, ec);
    }
    if (ec) {
        vmt::Error error = io_error("cannot replace the VMT file: " + path.string(), path);
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        last_error_ = error;
        return vmt::Err<void>(error);
    }

    path_ = path;
    material_root_ = detect_material_root(path_);
    virtual_name_.clear();
    saved_text_ = text_;
    is_vtf_ = false;
    vtf_info_ = vtf::VtfInfo{};
    last_error_ = vmt::Error{};
    return vmt::Ok();
}

std::string EditorSession::display_name() const {
    if (!path_.empty()) {
        return path_.filename().string();
    }
    if (!virtual_name_.empty()) {
        return virtual_name_;
    }
    return std::string("untitled.vmt");
}

bool EditorSession::dirty() const noexcept {
    if (!valid_ && !text_.empty()) {
        return true; // unparsed text is unsaved work
    }
    return text_ != saved_text_;
}

} // namespace ui
