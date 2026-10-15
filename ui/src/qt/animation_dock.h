#pragma once

// Animation player dock: sequence list with filters, playhead scrubbing,
// playback speed and loop, blending, overlay layers, pose parameters,
// bodygroups, skins and the display flags. The dock talks to the engine over
// EngineView; the viewport forwards the live playback tick to updatePlayhead().
//
// The panel greys out when no model is loaded and every engine call is
// best-effort: a rejected command reports through statusMessage().

#include <QDockWidget>

#include <map>
#include <vector>

#include "bridge/engine_view.h"
#include "ui/animation_model.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;
class QVBoxLayout;
class QWidget;

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

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // --- construction / scene wiring ----------------------------------------
    void buildUi();
    void applyScene();
    void setPanelEnabled(bool enabled);

    void rebuildSequenceCombo();
    void rebuildOverlayCombos();
    void rebuildPoseRows();
    void rebuildBodyRows();
    void rebuildSkinCombo();
    void syncTimelineUi();
    void updateStatusLabel();

    const SequenceRow* rowForEngineIndex(int index) const;
    void selectSequenceRow(int row);
    void setPlaying(bool playing);
    void applyPlayheadValue(int frame);

    // --- widget handlers -----------------------------------------------------
    void onSequenceComboChanged(int row);
    void onFilterToggled();
    void onFirstSequenceClicked();
    void onPreviousSequenceClicked();
    void onNextSequenceClicked();
    void onPlayPauseClicked();
    void onStopClicked();

    void onFrameSliderValueChanged(int value);
    void onFrameSpinValueChanged(int value);
    void onFrameSliderPressed();
    void onFrameSliderReleased();
    void queueFrame(int frame);
    void flushPendingFrame();
    void beginScrub();
    void endScrub();
    void stepFrame(int delta);

    void onPoseSliderChanged(int index, float min_value, float max_value, int step, QLabel* label);
    void flushPendingPose();

    void reportCall(const vmt::Result<void>& result, const QString& action);

    bridge::EngineView* m_engine = nullptr;
    bridge::ModelScene m_scene;
    std::vector<SequenceRow> m_rows;
    SequenceFilter m_filter;
    bridge::AnimState m_animState;

    bool m_updating = false;   // suppress widget handlers while repopulating
    bool m_playing = false;    // last play/pause state the dock commanded
    bool m_scrubbing = false;  // the user drags the timeline
    bool m_wasPlayingBeforeScrub = false;

    int m_currentSequence = 0;  // engine index the combo selects
    int m_pendingFrame = 0;     // throttled playhead value
    int m_lastSentFrame = -1;

    QWidget* m_content = nullptr;
    QComboBox* m_sequenceCombo = nullptr;
    QCheckBox* m_showHiddenCheck = nullptr;
    QCheckBox* m_byActivityCheck = nullptr;
    QCheckBox* m_alphabeticalCheck = nullptr;
    QPushButton* m_firstButton = nullptr;
    QPushButton* m_previousButton = nullptr;
    QPushButton* m_playPauseButton = nullptr;
    QPushButton* m_nextButton = nullptr;
    QPushButton* m_stopButton = nullptr;

    QSlider* m_frameSlider = nullptr;
    QSpinBox* m_frameSpin = nullptr;
    QLabel* m_frameStatus = nullptr;
    QTimer* m_frameThrottle = nullptr;
    QTimer* m_scrubIdle = nullptr;

    QSlider* m_speedSlider = nullptr;
    QLabel* m_speedLabel = nullptr;
    QCheckBox* m_loopCheck = nullptr;

    QDoubleSpinBox* m_blendSpin = nullptr;

    QComboBox* m_overlayCombos[4] = { nullptr, nullptr, nullptr, nullptr };

    QVBoxLayout* m_poseLayout = nullptr;
    QTimer* m_poseThrottle = nullptr;
    std::map<int, float> m_pendingPose;

    QVBoxLayout* m_bodyLayout = nullptr;
    QComboBox* m_skinCombo = nullptr;
};

} // namespace qt
} // namespace ui
