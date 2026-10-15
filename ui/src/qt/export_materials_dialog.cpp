#include "export_materials_dialog.h"

namespace ui {
namespace qt {

ExportMaterialsDialog::ExportMaterialsDialog(bridge::EngineView* engine,
                                             QStringList materials,
                                             QString modelName,
                                             QWidget* parent)
    : QDialog(parent),
      m_engine(engine),
      m_materials(std::move(materials)),
      m_modelName(std::move(modelName)) {
    setWindowTitle(tr("Export Model Materials"));
}

} // namespace qt
} // namespace ui
