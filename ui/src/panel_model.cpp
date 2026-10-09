#include "ui/panel_model.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ui {

const char* const kUnrecognizedGroupName = "Unrecognized";

namespace {

std::string lower_copy(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return result;
}

PanelRow make_schema_row(const vmt::SchemaParam& param, const vmt::Node* node) {
    PanelRow row;
    row.kind = PanelRow::Kind::Parameter;
    row.name = param.name;
    row.type = param.type;
    row.advanced = param.advanced;
    row.has_default = param.has_default;
    row.default_value = param.default_value;
    row.has_range = param.has_range;
    row.min_value = param.min_value;
    row.max_value = param.max_value;
    row.enum_values = param.values;
    row.description = param.description;
    row.present = node != nullptr;
    if (node != nullptr) {
        row.value = std::string(node->value());
    } else if (param.has_default) {
        row.value = param.default_value;
    }
    return row;
}

} // namespace

std::vector<PanelGroup> build_panel_model(const vmt::Document* document,
                                          const vmt::ShaderSchema& schema,
                                          bool show_advanced) {
    std::vector<PanelGroup> groups;

    // Document parameters matched by any schema parameter (advanced ones
    // included: they are hidden, not "unrecognized").
    std::unordered_set<std::string> matched;

    for (const vmt::SchemaGroup& schema_group : schema.groups()) {
        PanelGroup group;
        group.name = schema_group.name;
        for (const vmt::SchemaParam& param : schema_group.parameters) {
            matched.insert(lower_copy(param.name));
            if (param.advanced && !show_advanced) {
                continue;
            }
            const vmt::Node* node = document != nullptr ? document->find(param.name) : nullptr;
            group.rows.push_back(make_schema_row(param, node));
        }
        if (!group.rows.empty()) {
            groups.push_back(std::move(group));
        }
    }

    // Reasons for parameters awaiting verification, keyed by lowercased name.
    std::unordered_map<std::string, std::string> review_reasons;
    for (const vmt::NeedsReviewParam& entry : schema.needs_review()) {
        review_reasons.emplace(lower_copy(entry.name), entry.reason);
    }

    PanelGroup unrecognized;
    unrecognized.name = kUnrecognizedGroupName;
    if (document != nullptr) {
        for (const vmt::Node* node : document->parameters()) {
            const std::string lower = lower_copy(node->key());
            if (matched.count(lower) != 0) {
                continue;
            }
            PanelRow row;
            row.kind = PanelRow::Kind::Unrecognized;
            row.name = std::string(node->key());
            row.value = std::string(node->value());
            row.present = true;
            const auto review = review_reasons.find(lower);
            if (review != review_reasons.end()) {
                row.warning = "unverified parameter: " + review->second;
            } else {
                row.warning = "not described by the shader schema";
            }
            unrecognized.rows.push_back(std::move(row));
        }
    }
    if (!unrecognized.rows.empty()) {
        groups.push_back(std::move(unrecognized));
    }

    return groups;
}

} // namespace ui
