#include "ui/session.h"

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

} // namespace

vmt::Result<void> EditorSession::open_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        vmt::Error error = io_error("cannot open the VMT file: " + path.string(), path);
        error.code = vmt::ErrorCode::NotFound;
        error.remediation = "Check the file path and that the file is readable";
        last_error_ = error;
        return vmt::Err<void>(error);
    }

    std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (!stream.good() && !stream.eof()) {
        vmt::Error error = io_error("failed to read the VMT file: " + path.string(), path);
        last_error_ = error;
        return vmt::Err<void>(error);
    }

    const std::string name = path.filename().string();
    vmt::Result<void> result = apply_parsed(std::move(content), name, /*keep_text_on_error=*/false);
    if (!result) {
        return result;
    }
    path_ = path;
    virtual_name_.clear();
    saved_text_ = text_;
    return vmt::Ok();
}

vmt::Result<void> EditorSession::open_text(std::string text, std::string display_name) {
    vmt::Result<void> result =
        apply_parsed(std::move(text), display_name, /*keep_text_on_error=*/false);
    if (!result) {
        return result;
    }
    path_.clear();
    virtual_name_ = std::move(display_name);
    saved_text_ = text_;
    return vmt::Ok();
}

vmt::Result<void> EditorSession::set_text(std::string text, std::string name) {
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

    vmt::Result<void> result = document_->set_parameter(name, value_text);
    if (!result) {
        last_error_ = result.error();
        return result;
    }

    text_ = document_->source();
    last_error_ = vmt::Error{};
    return vmt::Ok();
}

vmt::Result<void> EditorSession::save() {
    if (path_.empty()) {
        vmt::Error error = vmt::make_error(vmt::ErrorCode::InvalidArgument,
                                           "no file path to save to",
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

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
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
        last_error_ = error;
        return vmt::Err<void>(error);
    }
    stream.close();
    if (!stream) {
        vmt::Error error = io_error("failed to close the VMT file: " + path.string(), path);
        last_error_ = error;
        return vmt::Err<void>(error);
    }

    path_ = path;
    virtual_name_.clear();
    saved_text_ = text_;
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
