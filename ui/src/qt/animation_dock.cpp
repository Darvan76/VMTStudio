#include "animation_dock.h"

namespace ui {
namespace qt {

AnimationDock::AnimationDock(QWidget* parent) : QDockWidget(tr("Animation"), parent) {
    setObjectName("AnimationDock");
}

void AnimationDock::setEngine(bridge::EngineView* engine) {
    m_engine = engine;
}

void AnimationDock::setScene(const bridge::ModelScene& scene) {
    m_scene = scene;
}

void AnimationDock::updatePlayhead(int, int) {}

void AnimationDock::clear() {
    m_scene = bridge::ModelScene{};
}

} // namespace qt
} // namespace ui
