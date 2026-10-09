#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace vmt {

enum class ErrorCode : std::uint16_t {
    Ok = 0,
    // generic lifecycle
    IoError,
    NotFound,
    PermissionDenied,
    InvalidArgument,
    OutOfMemory,
    Internal,
    // VMT parse / serialize
    ParseError,
    UnterminatedString,
    UnterminatedBlock,
    UnexpectedToken,
    InvalidEncoding,
    // schema
    SchemaLoadError,
    SchemaNotFound,
    SchemaValidationError,
    // VTF
    VtfInvalidHeader,
    VtfUnsupportedVersion,
    VtfUnsupportedFormat,
    VtfTruncated,
    // engine bridge
    ArchitectureMismatch,
    DllLoadFailed,
    ProcNotFound,
    InterfaceNotFound,
    EngineInitFailed,
    SteamRequired,
    GameInfoNotFound,
    MountFailed,
    DeviceUnavailable,
    RenderFailed,
    // environment
    UnsupportedGame,
};

// The single error value carried by every Result<T> in the program.
struct Error {
    ErrorCode   code = ErrorCode::Ok;
    std::string message;      // human-readable summary
    std::string remediation;  // one actionable instruction for the user
    std::string detail;       // developer detail; not shown by default
    std::string file;         // source path, empty if n/a
    std::uint32_t line = 0;   // 1-based; 0 = unknown
    std::uint32_t column = 0; // 1-based; 0 = unknown
};

inline Error make_error(ErrorCode code, std::string message,
                        std::string remediation = {}, std::string file = {},
                        std::uint32_t line = 0, std::uint32_t column = 0) {
    Error e;
    e.code = code;
    e.message = std::move(message);
    e.remediation = std::move(remediation);
    e.file = std::move(file);
    e.line = line;
    e.column = column;
    return e;
}

// True for errors that must tell the user what to do about their install.
inline bool is_install_error(ErrorCode code) {
    switch (code) {
        case ErrorCode::ArchitectureMismatch:
        case ErrorCode::DllLoadFailed:
        case ErrorCode::ProcNotFound:
        case ErrorCode::InterfaceNotFound:
        case ErrorCode::EngineInitFailed:
        case ErrorCode::SteamRequired:
        case ErrorCode::GameInfoNotFound:
        case ErrorCode::MountFailed:
        case ErrorCode::DeviceUnavailable:
            return true;
        default:
            return false;
    }
}

} // namespace vmt
