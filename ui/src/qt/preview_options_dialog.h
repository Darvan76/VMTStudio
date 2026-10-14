#pragma once

#include <QDialog>
#include <QComboBox>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>

#include "ui/preview_options.h"

namespace ui {
namespace qt {

class PreviewOptionsDialog : public QDialog {
    Q_OBJECT

public:
    explicit PreviewOptionsDialog(const ui::PreviewOptions& currentOptions, bool isBrushMaterial = false, QWidget* parent = nullptr);
    ~PreviewOptionsDialog() override = default;

    ui::PreviewOptions options() const { return m_options; }

signals:
    void optionsChanged(const ui::PreviewOptions& options);
    void loadModelRequested();

private slots:
    void onEnvironmentChanged(int index);
    void onShapeChanged(int index);
    void onRigSliderChanged();
    void onLoadModelClicked();

private:
    void syncUiFromOptions();

    ui::PreviewOptions m_options;
    bool m_updating = false;
    bool m_isBrushMaterial = false;

    QComboBox* m_envCombo = nullptr;
    QComboBox* m_shapeCombo = nullptr;

    QSlider* m_ambientSlider = nullptr;
    QDoubleSpinBox* m_ambientSpin = nullptr;

    QSlider* m_azimuthSlider = nullptr;
    QDoubleSpinBox* m_azimuthSpin = nullptr;

    QSlider* m_elevationSlider = nullptr;
    QDoubleSpinBox* m_elevationSpin = nullptr;

    QSlider* m_intensitySlider = nullptr;
    QDoubleSpinBox* m_intensitySpin = nullptr;

    QSlider* m_backgroundSlider = nullptr;
    QDoubleSpinBox* m_backgroundSpin = nullptr;

    QPushButton* m_loadModelBtn = nullptr;
};

} // namespace qt
} // namespace ui
