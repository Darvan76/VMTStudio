#pragma once

// Animation player dock: sequence list, playhead, speed, loop, overlays, pose
// parameters, bodygroups and skins. Talks to the engine through EngineView;
// playback state arrives from the viewport's live tick.
//
// Scaffold: the dock lands with the animation player work.

#include <QDockWidget>

#include "bridge/engine_view.h"

namespace ui {
namespace qt {

class AnimationDock : public QDockWidget {
    Q_OBJECT

public:
    explicit AnimationDock(QWidget* parent = nullptr);
    ~AnimationDock() override = default;

    void setEngine(bridge::EngineView* engine);
    // Repopulates the sequence/bodygroup/skin lists; empty model_name clears.
    void setScene(const bridge::ModelScene& scene);
    // Live tick from the viewport: frame and cycle * 10000.
    void updatePlayhead(int frame, int cycle_x10000);
    void clear();

signals:
    void sequenceChanged(int index);
    void statusMessage(const QString& text);

private:
    bridge::EngineView* m_engine = nullptr;
    bridge::ModelScene m_scene;
};

} // namespace qt
} // namespace ui
