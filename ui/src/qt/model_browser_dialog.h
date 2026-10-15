#pragma once

// Model browser: lists every .mdl reachable through the engine file system
// (loose folders and VPKs) and hands the selection back to the main window.
//
// Scaffold: the dialog lands with the "load 3D models" work.

#include <QDialog>

namespace bridge {
class EngineView;
}

namespace ui {
namespace qt {

class ModelBrowserDialog : public QDialog {
    Q_OBJECT

public:
    explicit ModelBrowserDialog(bridge::EngineView* engine, QWidget* parent = nullptr);
    ~ModelBrowserDialog() override = default;

signals:
    // `gameRelative` always addresses the model through the engine
    // ("models/props_c17/oildrum001.mdl"); `diskPath` is filled for loose files.
    void modelSelected(const QString& gameRelative, bool loose, const QString& diskPath);

private:
    bridge::EngineView* m_engine = nullptr;
};

} // namespace qt
} // namespace ui
