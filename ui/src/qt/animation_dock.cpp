#include "animation_dock.h"

// Full animation player surface, laid out like the HLMV sequence tab: sequence
// choices with filters, playhead scrubbing, speed scale, loop, blend time,
// four overlay layers, pose parameters, bodygroups, skins and the display
// flags. Everything lives in a scroll area because the dock is narrow.

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLayoutItem>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ui {
namespace qt {

namespace {

// Throttled controls (playhead scrubbing, pose parameters) reach the engine at
// most this often while the user keeps moving a slider: ~30 Hz.
constexpr int kThrottleIntervalMs = 33;

// A playhead scrub ends this long after the last change when the slider is no
// longer held down (spin box typing, page keys).
constexpr int kScrubIdleMs = 250;

// Slider position (1/100 steps) of a speed scale.
int speed_to_slider(float speed) {
    return static_cast<int>(std::lround(static_cast<double>(speed) * 100.0));
}

struct ViewFlag {
    const char* label;
    const char* flag;
};

// The display flags of the HLMV render tab with their engine spellings.
const ViewFlag kViewFlags[] = {
    { "Bones", "bones" },
    { "Hitboxes", "hitboxes" },
    { "Attachments", "attachments" },
    { "Ground", "ground" },
    { "Movement", "movement" },
    { "Wireframe overlay", "overlay_wireframe" },
    { "Normals", "normals" },
    { "Tangents", "tangents" },
    { "Shadow", "shadow" },
    { "Sequence boxes", "sequence_boxes" },
};

constexpr int kViewFlagCount = static_cast<int>(sizeof(kViewFlags) / sizeof(kViewFlags[0]));

} // namespace

AnimationDock::AnimationDock(QWidget* parent) : QDockWidget(tr("Animation"), parent) {
    setObjectName("AnimationDock");
    buildUi();
    setPanelEnabled(false);
}

// --- public surface ----------------------------------------------------------

void AnimationDock::setEngine(bridge::EngineView* engine) {
    m_engine = engine;
    setPanelEnabled(m_engine != nullptr && !m_scene.model_name.empty());
}

void AnimationDock::setScene(const bridge::ModelScene& scene) {
    m_scene = scene;
    if (m_scene.model_name.empty()) {
        clear();
        return;
    }
    applyScene();
}

void AnimationDock::clear() {
    m_scene = bridge::ModelScene{};
    applyScene();
}

void AnimationDock::updatePlayhead(int frame, int cycle_x10000) {
    if (m_frameSlider == nullptr || m_frameSpin == nullptr) {
        return;
    }
    m_animState.cycle = static_cast<float>(cycle_x10000) / 10000.0f;
    if (m_scrubbing) {
        // Never fight the user's drag: the widgets keep the dragged value.
        m_animState.frame = static_cast<float>(frame);
    } else {
        applyPlayheadValue(frame);
    }
    updateStatusLabel();
}

// --- construction ------------------------------------------------------------

void AnimationDock::buildUi() {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    m_content = new QWidget(scroll);
    scroll->setWidget(m_content);
    setWidget(scroll);

    auto* root = new QVBoxLayout(m_content);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(8);

    // --- Sequence ------------------------------------------------------------
    auto* sequenceGroup = new QGroupBox(tr("Sequence"), m_content);
    auto* sequenceLayout = new QVBoxLayout(sequenceGroup);
    sequenceLayout->setSpacing(4);

    m_sequenceCombo = new QComboBox(sequenceGroup);
    m_sequenceCombo->setToolTip(tr("Select the active sequence"));
    sequenceLayout->addWidget(m_sequenceCombo);

    auto* transport = new QHBoxLayout();
    transport->setSpacing(4);
    m_firstButton = new QPushButton(tr("|<"), sequenceGroup);
    m_firstButton->setToolTip(tr("First sequence"));
    m_previousButton = new QPushButton(tr("<"), sequenceGroup);
    m_previousButton->setToolTip(tr("Previous sequence"));
    m_playPauseButton = new QPushButton(tr("Play/Pause"), sequenceGroup);
    m_playPauseButton->setToolTip(tr("Play or pause the current sequence"));
    m_nextButton = new QPushButton(tr(">"), sequenceGroup);
    m_nextButton->setToolTip(tr("Next sequence"));
    m_stopButton = new QPushButton(tr("Stop"), sequenceGroup);
    m_stopButton->setToolTip(tr("Stop and rewind to the first frame"));
    transport->addWidget(m_firstButton);
    transport->addWidget(m_previousButton);
    transport->addWidget(m_playPauseButton, 2);
    transport->addWidget(m_nextButton);
    transport->addWidget(m_stopButton);
    sequenceLayout->addLayout(transport);

    auto* filterLayout = new QGridLayout();
    filterLayout->setSpacing(4);
    m_showHiddenCheck = new QCheckBox(tr("Show hidden"), sequenceGroup);
    m_byActivityCheck = new QCheckBox(tr("By activity"), sequenceGroup);
    m_alphabeticalCheck = new QCheckBox(tr("Alphabetical"), sequenceGroup);
    filterLayout->addWidget(m_showHiddenCheck, 0, 0);
    filterLayout->addWidget(m_byActivityCheck, 0, 1);
    filterLayout->addWidget(m_alphabeticalCheck, 1, 0);
    sequenceLayout->addLayout(filterLayout);
    root->addWidget(sequenceGroup);

    connect(m_sequenceCombo, &QComboBox::currentIndexChanged, this,
            &AnimationDock::onSequenceComboChanged);
    connect(m_firstButton, &QPushButton::clicked, this, &AnimationDock::onFirstSequenceClicked);
    connect(m_previousButton, &QPushButton::clicked, this,
            &AnimationDock::onPreviousSequenceClicked);
    connect(m_nextButton, &QPushButton::clicked, this, &AnimationDock::onNextSequenceClicked);
    connect(m_playPauseButton, &QPushButton::clicked, this, &AnimationDock::onPlayPauseClicked);
    connect(m_stopButton, &QPushButton::clicked, this, &AnimationDock::onStopClicked);
    connect(m_showHiddenCheck, &QCheckBox::toggled, this, &AnimationDock::onFilterToggled);
    connect(m_byActivityCheck, &QCheckBox::toggled, this, &AnimationDock::onFilterToggled);
    connect(m_alphabeticalCheck, &QCheckBox::toggled, this, &AnimationDock::onFilterToggled);

    // --- Timeline ------------------------------------------------------------
    auto* timelineGroup = new QGroupBox(tr("Timeline"), m_content);
    auto* timelineLayout = new QVBoxLayout(timelineGroup);
    timelineLayout->setSpacing(4);

    m_frameSlider = new QSlider(Qt::Horizontal, timelineGroup);
    m_frameSlider->setRange(0, 0);
    m_frameSlider->setToolTip(tr("Scrub the current sequence"));
    timelineLayout->addWidget(m_frameSlider);

    auto* frameRow = new QHBoxLayout();
    frameRow->setSpacing(4);
    frameRow->addWidget(new QLabel(tr("Frame:"), timelineGroup));
    m_frameSpin = new QSpinBox(timelineGroup);
    m_frameSpin->setRange(0, 0);
    m_frameSpin->setKeyboardTracking(false);
    m_frameSpin->setMaximumWidth(80);
    frameRow->addWidget(m_frameSpin);
    m_frameStatus = new QLabel(timelineGroup);
    m_frameStatus->setStyleSheet("color: #9a9a9e;");
    frameRow->addWidget(m_frameStatus, 1);
    timelineLayout->addLayout(frameRow);
    root->addWidget(timelineGroup);

    m_frameThrottle = new QTimer(this);
    m_frameThrottle->setSingleShot(true);
    m_frameThrottle->setInterval(kThrottleIntervalMs);
    connect(m_frameThrottle, &QTimer::timeout, this, &AnimationDock::flushPendingFrame);

    m_scrubIdle = new QTimer(this);
    m_scrubIdle->setSingleShot(true);
    m_scrubIdle->setInterval(kScrubIdleMs);
    connect(m_scrubIdle, &QTimer::timeout, this, [this]() {
        if (m_frameSlider != nullptr && !m_frameSlider->isSliderDown()) {
            endScrub();
        }
    });

    connect(m_frameSlider, &QSlider::valueChanged, this,
            &AnimationDock::onFrameSliderValueChanged);
    connect(m_frameSlider, &QSlider::sliderPressed, this, &AnimationDock::onFrameSliderPressed);
    connect(m_frameSlider, &QSlider::sliderReleased, this, &AnimationDock::onFrameSliderReleased);
    connect(m_frameSpin, &QSpinBox::valueChanged, this, &AnimationDock::onFrameSpinValueChanged);
    m_frameSlider->installEventFilter(this);
    m_frameSpin->installEventFilter(this);

    // --- Speed and loop --------------------------------------------------------
    auto* speedGroup = new QGroupBox(tr("Speed"), m_content);
    auto* speedLayout = new QHBoxLayout(speedGroup);
    speedLayout->setSpacing(6);
    m_speedSlider = new QSlider(Qt::Horizontal, speedGroup);
    m_speedSlider->setRange(speed_to_slider(kAnimationSpeedMin),
                            speed_to_slider(kAnimationSpeedMax));
    m_speedSlider->setSingleStep(1);
    m_speedSlider->setPageStep(25);
    m_speedSlider->setValue(speed_to_slider(1.0f));
    m_speedSlider->setToolTip(tr("Animation speed scale"));
    m_speedLabel = new QLabel(tr("1.00x"), speedGroup);
    m_speedLabel->setMinimumWidth(44);
    m_speedLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_loopCheck = new QCheckBox(tr("Loop"), speedGroup);
    m_loopCheck->setToolTip(tr("Wrap around at the end of the sequence"));
    speedLayout->addWidget(m_speedSlider, 1);
    speedLayout->addWidget(m_speedLabel);
    speedLayout->addWidget(m_loopCheck);
    root->addWidget(speedGroup);

    connect(m_speedSlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_updating) {
            return;
        }
        const float speed = clamp_animation_speed(static_cast<float>(value) / 100.0f);
        m_speedLabel->setText(QString::number(static_cast<double>(speed), 'f', 2) +
                              QStringLiteral("x"));
        if (m_engine != nullptr) {
            reportCall(m_engine->set_speed(speed), tr("Set speed"));
        }
    });
    connect(m_loopCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_updating) {
            return;
        }
        if (m_engine != nullptr) {
            reportCall(m_engine->set_loop(checked), tr("Set loop"));
        }
    });

    // --- Blend ---------------------------------------------------------------
    auto* blendGroup = new QGroupBox(tr("Blend"), m_content);
    auto* blendLayout = new QHBoxLayout(blendGroup);
    blendLayout->setSpacing(6);
    blendLayout->addWidget(new QLabel(tr("Blend time:"), blendGroup));
    m_blendSpin = new QDoubleSpinBox(blendGroup);
    m_blendSpin->setRange(0.0, 2.0);
    m_blendSpin->setSingleStep(0.05);
    m_blendSpin->setDecimals(2);
    m_blendSpin->setValue(0.20);
    m_blendSpin->setSuffix(tr(" s"));
    m_blendSpin->setToolTip(tr("Time used when blending between sequences"));
    blendLayout->addWidget(m_blendSpin, 1);
    auto* blendNowButton = new QPushButton(tr("Blend Now"), blendGroup);
    blendNowButton->setToolTip(tr("Start the blend towards the target sequence now"));
    blendLayout->addWidget(blendNowButton);
    root->addWidget(blendGroup);

    connect(m_blendSpin, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (m_updating) {
            return;
        }
        if (m_engine != nullptr) {
            reportCall(m_engine->set_blend_time(static_cast<float>(value)), tr("Set blend time"));
        }
    });
    connect(blendNowButton, &QPushButton::clicked, this, [this]() {
        if (m_engine != nullptr) {
            reportCall(m_engine->blend_now(), tr("Blend now"));
        }
    });

    // --- Overlays ------------------------------------------------------------
    auto* overlayGroup = new QGroupBox(tr("Overlays"), m_content);
    auto* overlayLayout = new QGridLayout(overlayGroup);
    overlayLayout->setSpacing(4);
    for (int layer = 1; layer <= 4; ++layer) {
        const int row = layer - 1;
        auto* combo = new QComboBox(overlayGroup);
        combo->addItem(tr("(none)"), -1);
        auto* weight = new QSlider(Qt::Horizontal, overlayGroup);
        weight->setRange(0, 100);
        weight->setValue(0);
        weight->setToolTip(tr("Overlay weight (%)"));
        overlayLayout->addWidget(new QLabel(tr("Layer %1").arg(layer), overlayGroup), row, 0);
        overlayLayout->addWidget(combo, row, 1);
        overlayLayout->addWidget(weight, row, 2);
        overlayLayout->setColumnStretch(1, 1);
        m_overlayCombos[row] = combo;

        connect(combo, &QComboBox::currentIndexChanged, this,
                [this, layer, combo, weight](int index) {
                    if (m_updating || index < 0) {
                        return;
                    }
                    const int sequence = combo->itemData(index).toInt();
                    if (sequence < 0) {
                        {
                            const QSignalBlocker blocker(weight);
                            weight->setValue(0);
                        }
                        if (m_engine != nullptr) {
                            reportCall(m_engine->set_overlay(layer, -1, 0.0f),
                                       tr("Set overlay"));
                        }
                    } else if (m_engine != nullptr) {
                        reportCall(m_engine->set_overlay(
                                       layer, sequence,
                                       static_cast<float>(weight->value()) / 100.0f),
                                   tr("Set overlay"));
                    }
                });
        connect(weight, &QSlider::valueChanged, this, [this, layer, combo](int value) {
            if (m_updating) {
                return;
            }
            const int comboIndex = combo->currentIndex();
            const int sequence = comboIndex > 0 ? combo->itemData(comboIndex).toInt() : -1;
            if (m_engine == nullptr) {
                return;
            }
            if (sequence < 0) {
                // The weight of a cleared layer carries no meaning.
                reportCall(m_engine->set_overlay(layer, -1, 0.0f), tr("Set overlay"));
            } else {
                reportCall(m_engine->set_overlay(layer, sequence,
                                                 static_cast<float>(value) / 100.0f),
                           tr("Set overlay"));
            }
        });
    }
    root->addWidget(overlayGroup);

    // --- Pose parameters -------------------------------------------------------
    auto* poseGroup = new QGroupBox(tr("Pose parameters"), m_content);
    auto* poseOuter = new QVBoxLayout(poseGroup);
    poseOuter->setSpacing(4);
    m_poseLayout = new QVBoxLayout();
    m_poseLayout->setSpacing(2);
    poseOuter->addLayout(m_poseLayout);
    root->addWidget(poseGroup);

    m_poseThrottle = new QTimer(this);
    m_poseThrottle->setSingleShot(true);
    m_poseThrottle->setInterval(kThrottleIntervalMs);
    connect(m_poseThrottle, &QTimer::timeout, this, &AnimationDock::flushPendingPose);

    // --- Bodygroups and skins --------------------------------------------------
    auto* bodyGroup = new QGroupBox(tr("Bodygroups and skins"), m_content);
    auto* bodyOuter = new QVBoxLayout(bodyGroup);
    bodyOuter->setSpacing(4);
    m_bodyLayout = new QVBoxLayout();
    m_bodyLayout->setSpacing(2);
    bodyOuter->addLayout(m_bodyLayout);

    auto* skinRow = new QHBoxLayout();
    skinRow->setSpacing(4);
    skinRow->addWidget(new QLabel(tr("Skin:"), bodyGroup));
    m_skinCombo = new QComboBox(bodyGroup);
    skinRow->addWidget(m_skinCombo, 1);
    bodyOuter->addLayout(skinRow);
    root->addWidget(bodyGroup);

    connect(m_skinCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_updating || index < 0) {
            return;
        }
        if (m_engine != nullptr) {
            reportCall(m_engine->set_skin(m_skinCombo->itemData(index).toInt()), tr("Set skin"));
        }
    });

    // --- Display flags ---------------------------------------------------------
    auto* flagsGroup = new QGroupBox(tr("Display flags"), m_content);
    auto* flagsLayout = new QGridLayout(flagsGroup);
    flagsLayout->setSpacing(4);
    for (int i = 0; i < kViewFlagCount; ++i) {
        auto* check = new QCheckBox(tr(kViewFlags[i].label), flagsGroup);
        check->setChecked(false);
        flagsLayout->addWidget(check, i / 2, i % 2);
        connect(check, &QCheckBox::toggled, this, [this, flag = kViewFlags[i].flag](bool checked) {
            if (m_updating) {
                return;
            }
            if (m_engine != nullptr) {
                reportCall(m_engine->set_view_flag(flag, checked), tr("Set view flag"));
            }
        });
    }
    root->addWidget(flagsGroup);

    root->addStretch(1);
}

// --- scene wiring --------------------------------------------------------------

void AnimationDock::applyScene() {
    if (m_frameThrottle != nullptr) {
        m_frameThrottle->stop();
    }
    if (m_poseThrottle != nullptr) {
        m_poseThrottle->stop();
    }
    if (m_scrubIdle != nullptr) {
        m_scrubIdle->stop();
    }
    m_pendingPose.clear();
    m_rows.clear();
    m_animState = bridge::AnimState{};
    m_currentSequence = 0;
    m_playing = false;
    m_scrubbing = false;
    m_wasPlayingBeforeScrub = false;
    m_pendingFrame = 0;
    m_lastSentFrame = -1;

    m_updating = true;
    rebuildSequenceCombo();
    rebuildOverlayCombos();
    rebuildPoseRows();
    rebuildBodyRows();
    rebuildSkinCombo();
    syncTimelineUi();
    m_updating = false;

    setPanelEnabled(!m_scene.model_name.empty() && m_engine != nullptr);
    updateStatusLabel();
}

void AnimationDock::setPanelEnabled(bool enabled) {
    if (m_content != nullptr) {
        m_content->setEnabled(enabled);
    }
}

void AnimationDock::rebuildSequenceCombo() {
    const int previous = m_currentSequence;
    m_rows = build_sequence_rows(m_scene, m_filter);

    const QSignalBlocker blocker(m_sequenceCombo);
    m_sequenceCombo->clear();
    for (const SequenceRow& row : m_rows) {
        m_sequenceCombo->addItem(QString::fromStdString(row.display), row.index);
    }

    int target = -1;
    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
        if (m_rows[static_cast<std::size_t>(i)].index == previous) {
            target = i;
            break;
        }
    }
    if (target < 0 && !m_rows.empty()) {
        // The filter hid the previous selection: fall back to the first row.
        target = 0;
    }
    if (target >= 0) {
        m_sequenceCombo->setCurrentIndex(target);
        m_currentSequence = m_rows[static_cast<std::size_t>(target)].index;
    } else {
        m_sequenceCombo->setCurrentIndex(-1);
    }
}

void AnimationDock::rebuildOverlayCombos() {
    for (int layer = 1; layer <= 4; ++layer) {
        QComboBox* combo = m_overlayCombos[layer - 1];
        if (combo == nullptr) {
            continue;
        }
        const int comboIndex = combo->currentIndex();
        const int previous = comboIndex > 0 ? combo->itemData(comboIndex).toInt() : -1;

        const QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItem(tr("(none)"), -1);
        for (const SequenceRow& row : m_rows) {
            combo->addItem(QString::fromStdString(row.display), row.index);
        }

        int target = 0;
        for (int i = 1; i < combo->count(); ++i) {
            if (combo->itemData(i).toInt() == previous) {
                target = i;
                break;
            }
        }
        combo->setCurrentIndex(target);
    }
}

void AnimationDock::rebuildPoseRows() {
    if (m_poseLayout == nullptr) {
        return;
    }
    while (QLayoutItem* item = m_poseLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    for (const bridge::ModelPoseParameter& parameter : m_scene.pose_parameters) {
        const float low = std::min(parameter.min_value, parameter.max_value);
        const float high = std::max(parameter.min_value, parameter.max_value);
        const float span = high - low;

        auto* container = new QWidget(m_content);
        auto* layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        const QString name = QString::fromStdString(parameter.name);
        auto* nameLabel = new QLabel(name, container);
        nameLabel->setToolTip(name);
        nameLabel->setMinimumWidth(72);

        auto* slider = new QSlider(Qt::Horizontal, container);
        slider->setRange(0, 100);
        slider->setSingleStep(1);
        slider->setPageStep(10);

        auto* valueLabel = new QLabel(container);
        valueLabel->setMinimumWidth(48);
        valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        layout->addWidget(nameLabel);
        layout->addWidget(slider, 1);
        layout->addWidget(valueLabel);

        // The scene dump does not carry the current flex value; start at 0
        // clamped into the parameter range, like HLMV does on model load.
        const float initial = std::clamp(0.0f, low, high);
        const int initialStep = span > 0.0f
            ? static_cast<int>(std::lround(
                  static_cast<double>((initial - low) / span) * 100.0))
            : 0;
        slider->setValue(initialStep);
        slider->setEnabled(span > 0.0f);
        valueLabel->setText(QString::number(static_cast<double>(initial), 'f', 2));

        connect(slider, &QSlider::valueChanged, this,
                [this, index = parameter.index, low_min = low, high_max = high, valueLabel](
                    int step) {
                    onPoseSliderChanged(index, low_min, high_max, step, valueLabel);
                });

        m_poseLayout->addWidget(container);
    }
}

void AnimationDock::rebuildBodyRows() {
    if (m_bodyLayout == nullptr) {
        return;
    }
    while (QLayoutItem* item = m_bodyLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    for (const bridge::ModelBodypart& bodypart : m_scene.bodyparts) {
        auto* container = new QWidget(m_content);
        auto* layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        QString name = QString::fromStdString(bodypart.name);
        if (name.isEmpty()) {
            name = tr("Bodypart %1").arg(bodypart.index);
        }
        auto* nameLabel = new QLabel(name, container);
        nameLabel->setToolTip(name);
        nameLabel->setMinimumWidth(72);

        auto* combo = new QComboBox(container);
        const int models = std::max(bodypart.models, 0);
        for (int model = 0; model < models; ++model) {
            combo->addItem(tr("Model %1").arg(model), model);
        }
        if (models > 0) {
            const QSignalBlocker blocker(combo);
            combo->setCurrentIndex(std::clamp(bodypart.current, 0, models - 1));
        } else {
            combo->setEnabled(false);
        }

        connect(combo, &QComboBox::currentIndexChanged, this,
                [this, group = bodypart.index, combo](int index) {
                    if (m_updating || index < 0) {
                        return;
                    }
                    if (m_engine != nullptr) {
                        reportCall(m_engine->set_bodygroup(group, combo->itemData(index).toInt()),
                                   tr("Set bodygroup"));
                    }
                });

        layout->addWidget(nameLabel);
        layout->addWidget(combo, 1);
        m_bodyLayout->addWidget(container);
    }
}

void AnimationDock::rebuildSkinCombo() {
    if (m_skinCombo == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_skinCombo);
    m_skinCombo->clear();
    for (const bridge::ModelSkin& skin : m_scene.skins) {
        const QString name = skin.name.empty()
            ? tr("Skin %1").arg(skin.index)
            : QString::fromStdString(skin.name);
        m_skinCombo->addItem(name, skin.index);
    }
    if (m_skinCombo->count() > 0) {
        // The engine selects skin 0 on model load.
        m_skinCombo->setCurrentIndex(0);
    }
    m_skinCombo->setEnabled(m_skinCombo->count() > 0);
}

void AnimationDock::syncTimelineUi() {
    const SequenceRow* row = rowForEngineIndex(m_currentSequence);
    const int frames = row != nullptr ? std::max(row->frames, 0) : 0;
    const int maximum = std::max(frames - 1, 0);
    {
        const QSignalBlocker sliderBlocker(m_frameSlider);
        const QSignalBlocker spinBlocker(m_frameSpin);
        m_frameSlider->setRange(0, maximum);
        m_frameSpin->setRange(0, maximum);
        m_frameSlider->setValue(0);
        m_frameSpin->setValue(0);
    }
    const bool hasFrames = frames > 0;
    m_frameSlider->setEnabled(hasFrames);
    m_frameSpin->setEnabled(hasFrames);
    {
        const QSignalBlocker loopBlocker(m_loopCheck);
        m_loopCheck->setChecked(row != nullptr && row->looping);
    }
    m_animState.sequence = m_currentSequence;
    m_animState.frame = 0.0f;
    m_animState.cycle = 0.0f;
}

void AnimationDock::updateStatusLabel() {
    if (m_frameStatus == nullptr) {
        return;
    }
    m_animState.paused = !m_playing;
    m_frameStatus->setText(QString::fromStdString(format_frame_status(m_animState, m_scene)));
}

const SequenceRow* AnimationDock::rowForEngineIndex(int index) const {
    for (const SequenceRow& row : m_rows) {
        if (row.index == index) {
            return &row;
        }
    }
    return nullptr;
}

void AnimationDock::selectSequenceRow(int row) {
    const int count = static_cast<int>(m_rows.size());
    if (count == 0) {
        return;
    }
    while (row < 0) {
        row += count;
    }
    while (row >= count) {
        row -= count;
    }
    m_sequenceCombo->setCurrentIndex(row);
}

void AnimationDock::setPlaying(bool playing) {
    m_playing = playing;
    if (m_engine != nullptr) {
        reportCall(m_engine->set_animation(m_playing ? "play" : "pause"),
                   tr("Set animation state"));
    }
    updateStatusLabel();
}

void AnimationDock::applyPlayheadValue(int frame) {
    if (m_frameSlider == nullptr || m_frameSpin == nullptr) {
        return;
    }
    const int clamped = std::clamp(frame, m_frameSlider->minimum(), m_frameSlider->maximum());
    if (m_frameSlider->value() != clamped) {
        const QSignalBlocker sliderBlocker(m_frameSlider);
        m_frameSlider->setValue(clamped);
    }
    if (m_frameSpin->value() != clamped) {
        const QSignalBlocker spinBlocker(m_frameSpin);
        m_frameSpin->setValue(clamped);
    }
    m_animState.frame = static_cast<float>(clamped);
}

// --- sequence handlers ---------------------------------------------------------

void AnimationDock::onSequenceComboChanged(int row) {
    if (m_updating || row < 0 || row >= static_cast<int>(m_rows.size())) {
        return;
    }
    m_currentSequence = m_rows[static_cast<std::size_t>(row)].index;
    syncTimelineUi();
    if (m_engine != nullptr) {
        reportCall(m_engine->set_sequence(m_currentSequence), tr("Select sequence"));
    }
    emit sequenceChanged(m_currentSequence);
    updateStatusLabel();
}

void AnimationDock::onFilterToggled() {
    if (m_updating) {
        return;
    }
    m_filter.show_hidden = m_showHiddenCheck->isChecked();
    m_filter.by_activity = m_byActivityCheck->isChecked();
    m_filter.alphabetical = m_alphabeticalCheck->isChecked();

    const int previous = m_currentSequence;
    m_updating = true;
    rebuildSequenceCombo();
    rebuildOverlayCombos();
    m_updating = false;

    if (m_currentSequence != previous) {
        // A filter hid the playing sequence and the selection moved with the
        // visible list; keep the engine on the sequence the combo now shows.
        syncTimelineUi();
        if (m_engine != nullptr) {
            reportCall(m_engine->set_sequence(m_currentSequence), tr("Select sequence"));
        }
        emit sequenceChanged(m_currentSequence);
    }
    updateStatusLabel();
}

void AnimationDock::onFirstSequenceClicked() {
    selectSequenceRow(0);
}

void AnimationDock::onPreviousSequenceClicked() {
    selectSequenceRow(m_sequenceCombo->currentIndex() - 1);
}

void AnimationDock::onNextSequenceClicked() {
    selectSequenceRow(m_sequenceCombo->currentIndex() + 1);
}

void AnimationDock::onPlayPauseClicked() {
    setPlaying(!m_playing);
}

void AnimationDock::onStopClicked() {
    m_playing = false;
    if (m_engine != nullptr) {
        reportCall(m_engine->stop_animation(), tr("Stop animation"));
    }
    // The backend pauses and rewinds to frame 0; mirror that right away.
    applyPlayheadValue(0);
    updateStatusLabel();
}

// --- timeline ----------------------------------------------------------------

void AnimationDock::onFrameSliderValueChanged(int value) {
    if (m_updating) {
        return;
    }
    if (m_frameSpin->value() != value) {
        const QSignalBlocker spinBlocker(m_frameSpin);
        m_frameSpin->setValue(value);
    }
    if (!m_scrubbing) {
        beginScrub();
    }
    queueFrame(value);
}

void AnimationDock::onFrameSpinValueChanged(int value) {
    if (m_updating) {
        return;
    }
    if (m_frameSlider->value() != value) {
        const QSignalBlocker sliderBlocker(m_frameSlider);
        m_frameSlider->setValue(value);
    }
    if (!m_scrubbing) {
        beginScrub();
    }
    queueFrame(value);
}

void AnimationDock::onFrameSliderPressed() {
    beginScrub();
    // A press does not move the playhead by itself; seeding the throttle with
    // the current value keeps a press+release from sending a stray frame.
    m_pendingFrame = m_frameSlider->value();
    m_lastSentFrame = m_pendingFrame;
    if (m_scrubIdle != nullptr) {
        m_scrubIdle->stop();
    }
}

void AnimationDock::onFrameSliderReleased() {
    endScrub();
}

void AnimationDock::queueFrame(int frame) {
    m_pendingFrame = frame;
    if (m_frameThrottle != nullptr && !m_frameThrottle->isActive()) {
        m_frameThrottle->start();
    }
    if (m_scrubIdle != nullptr) {
        m_scrubIdle->start();
    }
}

void AnimationDock::flushPendingFrame() {
    if (m_pendingFrame == m_lastSentFrame) {
        return;
    }
    m_lastSentFrame = m_pendingFrame;
    if (m_engine != nullptr) {
        reportCall(m_engine->set_frame(m_pendingFrame), tr("Set frame"));
    }
}

void AnimationDock::beginScrub() {
    if (m_scrubbing) {
        return;
    }
    m_scrubbing = true;
    m_wasPlayingBeforeScrub = m_playing;
    if (m_playing) {
        setPlaying(false); // pause once, for the whole drag
    }
}

void AnimationDock::endScrub() {
    if (!m_scrubbing) {
        return;
    }
    m_scrubbing = false;
    flushPendingFrame();
    if (m_wasPlayingBeforeScrub) {
        setPlaying(true);
    }
    m_wasPlayingBeforeScrub = false;
}

void AnimationDock::stepFrame(int delta) {
    if (m_frameSlider == nullptr || !m_frameSlider->isEnabled()) {
        return;
    }
    if (m_engine != nullptr) {
        reportCall(m_engine->step_frame(delta), tr("Step frame"));
    }
    applyPlayheadValue(m_frameSlider->value() + delta);
    updateStatusLabel();
}

bool AnimationDock::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress && (watched == m_frameSlider || watched == m_frameSpin)) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Left || keyEvent->key() == Qt::Key_Right) {
            stepFrame(keyEvent->key() == Qt::Key_Left ? -1 : 1);
            return true;
        }
    }
    return QDockWidget::eventFilter(watched, event);
}

// --- pose parameters -----------------------------------------------------------

void AnimationDock::onPoseSliderChanged(int index, float min_value, float max_value, int step,
                                        QLabel* label) {
    const float value = max_value > min_value
        ? min_value + (static_cast<float>(step) / 100.0f) * (max_value - min_value)
        : min_value;
    if (label != nullptr) {
        label->setText(QString::number(static_cast<double>(value), 'f', 2));
    }
    m_pendingPose[index] = value;
    if (m_poseThrottle != nullptr && !m_poseThrottle->isActive()) {
        m_poseThrottle->start();
    }
}

void AnimationDock::flushPendingPose() {
    if (m_engine != nullptr) {
        for (const auto& entry : m_pendingPose) {
            reportCall(m_engine->set_pose_parameter(entry.first, entry.second),
                       tr("Set pose parameter"));
        }
    }
    m_pendingPose.clear();
}

// --- engine reporting -----------------------------------------------------------

void AnimationDock::reportCall(const vmt::Result<void>& result, const QString& action) {
    if (result.ok()) {
        return;
    }
    QString detail = QString::fromStdString(result.error().message);
    if (detail.isEmpty()) {
        detail = tr("unknown error");
    }
    emit statusMessage(tr("%1 failed: %2").arg(action, detail));
}

} // namespace qt
} // namespace ui
