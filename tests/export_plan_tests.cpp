#include "ut.h"

#include "ui/export_plan.h"

// Scaffold: real coverage lands with the export dialog work.

UT_TEST_CASE("export_plan: scaffold placeholder") {
    ui::ExportOptions options;
    const auto result = ui::plan_material_export({}, options);
    UT_CHECK(!result.ok());
}
