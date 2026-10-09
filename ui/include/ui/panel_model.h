#pragma once

// ui panel model - presentation rows for the parameter panel, derived from one
// vmt::Document plus the matching vmt::ShaderSchema. Windows-free and unit
// testable.
//
// Rows are grouped in schema order. Parameters that exist in the document but
// are not described by the schema land in the synthetic "Unrecognized" group;
// parameters known only from the schema's needs_review list carry their
// verification reason as a warning. Advanced parameters are hidden unless
// show_advanced is true.

#include "vmt/document.h"
#include "vmt/schema.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ui {

struct PanelRow {
    enum class Kind : std::uint8_t {
        Parameter,    // described by the schema
        Unrecognized, // present in the document but not in the schema
    };

    Kind        kind = Kind::Parameter;
    std::string name;          // as written, e.g. "$basetexture"
    vmt::ParamType type = vmt::ParamType::String;
    std::string value;         // current raw value text; when absent, the default
    bool        present = false; // the document contains this parameter
    bool        advanced = false;
    bool        has_default = false;
    std::string default_value;
    bool        has_range = false;
    double      min_value = 0.0;
    double      max_value = 0.0;
    std::vector<std::string> enum_values;
    std::string description;
    std::string warning;       // non-empty for unrecognized rows
};

struct PanelGroup {
    std::string name;
    std::vector<PanelRow> rows;
};

// Synthetic group name used for Kind::Unrecognized rows.
extern const char* const kUnrecognizedGroupName;

// Builds the panel model. `document` may be null (no file loaded yet): the
// result then contains the schema's parameters as absent rows.
std::vector<PanelGroup> build_panel_model(const vmt::Document* document,
                                          const vmt::ShaderSchema& schema,
                                          bool show_advanced);

} // namespace ui
