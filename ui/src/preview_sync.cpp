#include "ui/preview_sync.h"

#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace ui {
namespace {

vmt::Error preview_error(vmt::ErrorCode code, std::string message) {
    return vmt::make_error(std::move(code), std::move(message),
                           "The preview keeps showing the last good material");
}

vmt::Error parse_error_fallback() {
    return vmt::make_error(vmt::ErrorCode::ParseError, "the VMT text does not parse",
                           "Fix the syntax error shown in the status bar");
}

} // namespace

namespace {

// "brick/brickwall001a" -> a safe relative path below materials/. Empty result
// (with a message) for empty names, absolute segments, ".." or characters that
// cannot appear in a Windows file name.
bool safe_material_relative_path(const std::string& material_name, std::string& out,
                                 std::string& error) {
    out.clear();
    if (material_name.empty()) {
        error = "empty material name";
        return false;
    }
    if (material_name.find(':') != std::string::npos ||
        material_name.find('*') != std::string::npos ||
        material_name.find('?') != std::string::npos ||
        material_name.find('"') != std::string::npos ||
        material_name.find('<') != std::string::npos ||
        material_name.find('>') != std::string::npos ||
        material_name.find('|') != std::string::npos) {
        error = "material name contains characters a file name cannot hold";
        return false;
    }
    std::string current;
    for (std::size_t i = 0; i <= material_name.size(); ++i) {
        const char c = i < material_name.size() ? material_name[i] : '/';
        if (c == '/' || c == '\\') {
            if (current.empty() || current == ".") {
                // skip empty / current-directory components
            } else if (current == "..") {
                error = "material name must not contain '..'";
                return false;
            } else {
                if (!out.empty()) {
                    out += '/';
                }
                out += current;
            }
            current.clear();
        } else {
            current += c;
        }
    }
    if (out.empty()) {
        error = "empty material name";
        return false;
    }
    return true;
}

} // namespace

vmt::Result<void> write_preview_material(const std::filesystem::path& root,
                                         const std::string& text) {
    return write_preview_material_at(root, kPreviewMaterialName, text);
}

vmt::Result<void> write_preview_material_at(const std::filesystem::path& root,
                                            const std::string& material_name,
                                            const std::string& text) {
    if (root.empty()) {
        return vmt::Err<void>(
            preview_error(vmt::ErrorCode::IoError, "the preview folder is not set"));
    }

    std::string relative;
    std::string name_error;
    if (!safe_material_relative_path(material_name, relative, name_error)) {
        return vmt::Err<void>(preview_error(vmt::ErrorCode::InvalidArgument,
                                            "invalid preview material name: " + name_error));
    }

    std::filesystem::path relative_path;
    {
        std::string component;
        for (std::size_t i = 0; i <= relative.size(); ++i) {
            const char c = i < relative.size() ? relative[i] : '/';
            if (c == '/') {
                relative_path /= std::filesystem::path(component);
                component.clear();
            } else {
                component += c;
            }
        }
    }

    const std::filesystem::path materials = root / L"materials";
    std::filesystem::path target = materials / relative_path;
    target += L".vmt";
    std::filesystem::path temp = target;
    temp += L".tmp";

    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        return vmt::Err<void>(preview_error(
            vmt::ErrorCode::IoError,
            "the preview folder could not be created: " + target.parent_path().string()));
    }

    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return vmt::Err<void>(preview_error(
                vmt::ErrorCode::IoError,
                "the preview material could not be written: " + temp.string()));
        }
        if (!text.empty()) {
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        }
        stream.close();
        if (!stream) {
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            return vmt::Err<void>(preview_error(
                vmt::ErrorCode::IoError,
                "the preview material could not be written: " + temp.string()));
        }
    }

    std::filesystem::remove(target, ec);
    ec.clear();
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return vmt::Err<void>(preview_error(
            vmt::ErrorCode::IoError,
            "the preview material could not be replaced: " + target.string()));
    }
    return vmt::Ok();
}

void PreviewSync::configure(WriteFn write, ReloadFn reload) {
    write_ = std::move(write);
    reload_ = std::move(reload);
}

void PreviewSync::set_active(bool active) {
    if (active_ == active) {
        return;
    }
    active_ = active;
    if (!active) {
        cancel();
    }
}

void PreviewSync::note_previewed(const std::string& text) {
    last_good_ = text;
    pending_ = false;
    stale_ = false;
    last_error_ = vmt::Error{};
}

bool PreviewSync::document_changed(bool valid, vmt::Error error) {
    if (!active_) {
        return false;
    }
    if (!valid) {
        pending_ = false;
        if (error.code == vmt::ErrorCode::Ok) {
            error = parse_error_fallback();
        }
        mark_stale(std::move(error));
        return false;
    }
    pending_ = true;
    stale_ = false;
    last_error_ = vmt::Error{};
    return true;
}

vmt::Result<void> PreviewSync::flush(const std::string& text) {
    if (!active_ || !pending_) {
        return vmt::Ok();
    }
    pending_ = false;
    return apply_and_reload(text);
}

vmt::Result<void> PreviewSync::flush_now(const std::string& text, bool valid, vmt::Error error) {
    if (!active_) {
        return vmt::Ok();
    }
    pending_ = false;
    if (!valid) {
        if (error.code == vmt::ErrorCode::Ok) {
            error = parse_error_fallback();
        }
        mark_stale(std::move(error));
        return vmt::Err<void>(last_error_);
    }
    return apply_and_reload(text);
}

void PreviewSync::cancel() {
    pending_ = false;
    stale_ = false;
    last_error_ = vmt::Error{};
}

vmt::Result<void> PreviewSync::apply_and_reload(const std::string& text) {
    if (!write_ || !write_(text)) {
        mark_stale(preview_error(vmt::ErrorCode::IoError,
                                 "the preview material could not be written"));
        return vmt::Err<void>(last_error_);
    }
    if (!reload_ || !reload_()) {
        // The engine rejected the edit. Put the last good revision back so the
        // viewport keeps its good render, then report the failure.
        if (!last_good_.empty() && write_ && write_(last_good_) && reload_) {
            (void)reload_();
        }
        mark_stale(preview_error(vmt::ErrorCode::RenderFailed,
                                 "the engine rejected the edited material"));
        return vmt::Err<void>(last_error_);
    }
    last_good_ = text;
    stale_ = false;
    last_error_ = vmt::Error{};
    return vmt::Ok();
}

void PreviewSync::mark_stale(vmt::Error error) {
    stale_ = true;
    last_error_ = std::move(error);
}

} // namespace ui
