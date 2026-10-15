#include "ui/export_plan.h"

// Scaffold: the export planner lands with the export dialog work.

namespace ui {

vmt::Result<ExportPlan> plan_material_export(const std::vector<std::string>&,
                                             const ExportOptions& options) {
    if (options.destination.empty()) {
        return vmt::Err<ExportPlan>(
            vmt::make_error(vmt::ErrorCode::InvalidArgument, "no destination folder"));
    }
    return vmt::Ok(ExportPlan{});
}

std::string flat_material_file_name(const std::string&) {
    return {};
}

} // namespace ui
