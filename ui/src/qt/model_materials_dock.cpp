#include "model_materials_dock.h"

namespace ui {
namespace qt {

ModelMaterialsDock::ModelMaterialsDock(QWidget* parent) : QDockWidget(tr("Model Materials"), parent) {
    setObjectName("ModelMaterialsDock");
}

void ModelMaterialsDock::setScene(const bridge::ModelScene& scene) {
    m_scene = scene;
}

void ModelMaterialsDock::clear() {
    m_scene = bridge::ModelScene{};
}

QString ModelMaterialsDock::currentMaterial() const {
    return {};
}

void ModelMaterialsDock::selectNextMaterial() {}

void ModelMaterialsDock::selectPreviousMaterial() {}

} // namespace qt
} // namespace ui
