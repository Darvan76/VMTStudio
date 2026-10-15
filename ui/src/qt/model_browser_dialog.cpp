#include "model_browser_dialog.h"

namespace ui {
namespace qt {

ModelBrowserDialog::ModelBrowserDialog(bridge::EngineView* engine, QWidget* parent)
    : QDialog(parent), m_engine(engine) {
    setWindowTitle(tr("Model Browser"));
}

} // namespace qt
} // namespace ui
