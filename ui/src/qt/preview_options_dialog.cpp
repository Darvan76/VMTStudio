#include "preview_options_dialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QDialogButtonBox>

namespace ui {
namespace qt {

PreviewOptionsDialog::PreviewOptionsDialog(const ui::PreviewOptions& currentOptions, QWidget* parent)
    : QDialog(parent), m_options(currentOptions) {
    setWindowTitle(tr("Preview Options"));
    setMinimumWidth(380);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);

    // Group: Geometry & Presets
    auto* geomGroup = new QGroupBox(tr("Environment & Geometry"), this);
    auto* geomLayout = new QFormLayout(geomGroup);

    m_envCombo = new QComboBox(geomGroup);
    m_envCombo->addItem(tr("Studio Dark (Neutral)"), static_cast<int>(ui::PreviewEnvironment::StudioDark));
    m_envCombo->addItem(tr("Studio Light"), static_cast<int>(ui::PreviewEnvironment::StudioLight));
    m_envCombo->addItem(tr("Outdoor (Sunlight)"), static_cast<int>(ui::PreviewEnvironment::Outdoor));
    m_envCombo->addItem(tr("Flat (Unshaded Albedo)"), static_cast<int>(ui::PreviewEnvironment::Flat));
    m_envCombo->addItem(tr("Custom"), static_cast<int>(ui::PreviewEnvironment::Custom));
    geomLayout->addRow(tr("Preset:"), m_envCombo);

    m_shapeCombo = new QComboBox(geomGroup);
    m_shapeCombo->addItem(tr("Model: Sphere"), static_cast<int>(ui::PreviewShape::Sphere));
    m_shapeCombo->addItem(tr("Model: Cube"), static_cast<int>(ui::PreviewShape::Cube));
    m_shapeCombo->addItem(tr("Model: Cylinder"), static_cast<int>(ui::PreviewShape::Cylinder));
    m_shapeCombo->addItem(tr("Model: Plane"), static_cast<int>(ui::PreviewShape::Plane));
    m_shapeCombo->addItem(tr("Brush: Wall (128x128)"), static_cast<int>(ui::PreviewShape::BrushWall));
    m_shapeCombo->addItem(tr("Brush: Floor (256x256)"), static_cast<int>(ui::PreviewShape::BrushFloor));
    m_shapeCombo->addItem(tr("Brush: Cube"), static_cast<int>(ui::PreviewShape::BrushCube));
    m_shapeCombo->addItem(tr("Brush: Displacement (Terrain)"), static_cast<int>(ui::PreviewShape::BrushDisplacement));
    geomLayout->addRow(tr("Shape:"), m_shapeCombo);

    m_loadModelBtn = new QPushButton(tr("Load Custom Model (.mdl)..."), geomGroup);
    geomLayout->addRow(m_loadModelBtn);

    mainLayout->addWidget(geomGroup);

    // Group: Lighting Controls
    auto* lightGroup = new QGroupBox(tr("Light Rig"), this);
    auto* lightLayout = new QFormLayout(lightGroup);

    auto makeSliderRow = [this](QFormLayout* layout, const QString& label,
                                double min, double max, double step,
                                QSlider*& slider, QDoubleSpinBox*& spin) {
        auto* container = new QWidget(this);
        auto* hBox = new QHBoxLayout(container);
        hBox->setContentsMargins(0, 0, 0, 0);
        hBox->setSpacing(6);

        slider = new QSlider(Qt::Horizontal, container);
        slider->setRange(0, 1000);

        spin = new QDoubleSpinBox(container);
        spin->setRange(min, max);
        spin->setSingleStep(step);
        spin->setMaximumWidth(70);

        hBox->addWidget(slider);
        hBox->addWidget(spin);
        layout->addRow(label, container);

        connect(slider, &QSlider::valueChanged, this, [this, slider, spin, min, max](int val) {
            if (m_updating) return;
            double realVal = min + (val / 1000.0) * (max - min);
            spin->blockSignals(true);
            spin->setValue(realVal);
            spin->blockSignals(false);
            onRigSliderChanged();
        });

        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, slider, spin, min, max](double val) {
            if (m_updating) return;
            double norm = (max > min) ? (val - min) / (max - min) : 0.0;
            slider->blockSignals(true);
            slider->setValue(static_cast<int>(norm * 1000.0));
            slider->blockSignals(false);
            onRigSliderChanged();
        });
    };

    makeSliderRow(lightLayout, tr("Ambient:"), 0.0, 1.0, 0.05, m_ambientSlider, m_ambientSpin);
    makeSliderRow(lightLayout, tr("Azimuth:"), -180.0, 180.0, 5.0, m_azimuthSlider, m_azimuthSpin);
    makeSliderRow(lightLayout, tr("Elevation:"), -90.0, 90.0, 5.0, m_elevationSlider, m_elevationSpin);
    makeSliderRow(lightLayout, tr("Intensity:"), 0.0, 2.0, 0.1, m_intensitySlider, m_intensitySpin);
    makeSliderRow(lightLayout, tr("Background:"), 0.0, 1.0, 0.05, m_backgroundSlider, m_backgroundSpin);

    mainLayout->addWidget(lightGroup);

    // Dialog Button Box
    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::accept);
    mainLayout->addWidget(buttonBox);

    // Connect signals
    connect(m_envCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &PreviewOptionsDialog::onEnvironmentChanged);
    connect(m_shapeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &PreviewOptionsDialog::onShapeChanged);
    connect(m_loadModelBtn, &QPushButton::clicked,
            this, &PreviewOptionsDialog::onLoadModelClicked);

    syncUiFromOptions();
}

void PreviewOptionsDialog::syncUiFromOptions() {
    m_updating = true;

    // Environment
    for (int i = 0; i < m_envCombo->count(); ++i) {
        if (m_envCombo->itemData(i).toInt() == static_cast<int>(m_options.environment)) {
            m_envCombo->setCurrentIndex(i);
            break;
        }
    }

    // Shape
    for (int i = 0; i < m_shapeCombo->count(); ++i) {
        if (m_shapeCombo->itemData(i).toInt() == static_cast<int>(m_options.shape)) {
            m_shapeCombo->setCurrentIndex(i);
            break;
        }
    }

    // Rig
    auto setSliderVal = [](QSlider* slider, QDoubleSpinBox* spin, double val, double min, double max) {
        spin->setValue(val);
        double norm = (max > min) ? (val - min) / (max - min) : 0.0;
        slider->setValue(static_cast<int>(norm * 1000.0));
    };

    setSliderVal(m_ambientSlider, m_ambientSpin, m_options.rig.ambient, 0.0, 1.0);
    setSliderVal(m_azimuthSlider, m_azimuthSpin, m_options.rig.key_azimuth, -180.0, 180.0);
    setSliderVal(m_elevationSlider, m_elevationSpin, m_options.rig.key_elevation, -90.0, 90.0);
    setSliderVal(m_intensitySlider, m_intensitySpin, m_options.rig.key_intensity, 0.0, 2.0);
    setSliderVal(m_backgroundSlider, m_backgroundSpin, m_options.rig.background, 0.0, 1.0);

    m_updating = false;
}

void PreviewOptionsDialog::onEnvironmentChanged(int index) {
    if (m_updating) return;
    auto env = static_cast<ui::PreviewEnvironment>(m_envCombo->itemData(index).toInt());
    if (env != ui::PreviewEnvironment::Custom) {
        ui::apply_environment(m_options, env);
        syncUiFromOptions();
        emit optionsChanged(m_options);
    }
}

void PreviewOptionsDialog::onShapeChanged(int index) {
    if (m_updating) return;
    m_options.shape = static_cast<ui::PreviewShape>(m_shapeCombo->itemData(index).toInt());
    emit optionsChanged(m_options);
}

void PreviewOptionsDialog::onRigSliderChanged() {
    if (m_updating) return;
    ui::PreviewRig rig;
    rig.ambient = static_cast<float>(m_ambientSpin->value());
    rig.key_azimuth = static_cast<float>(m_azimuthSpin->value());
    rig.key_elevation = static_cast<float>(m_elevationSpin->value());
    rig.key_intensity = static_cast<float>(m_intensitySpin->value());
    rig.background = static_cast<float>(m_backgroundSpin->value());

    ui::apply_rig(m_options, rig);

    // Switch combo to custom if not already
    for (int i = 0; i < m_envCombo->count(); ++i) {
        if (m_envCombo->itemData(i).toInt() == static_cast<int>(ui::PreviewEnvironment::Custom)) {
            m_envCombo->blockSignals(true);
            m_envCombo->setCurrentIndex(i);
            m_envCombo->blockSignals(false);
            break;
        }
    }

    emit optionsChanged(m_options);
}

void PreviewOptionsDialog::onLoadModelClicked() {
    emit loadModelRequested();
}

} // namespace qt
} // namespace ui
