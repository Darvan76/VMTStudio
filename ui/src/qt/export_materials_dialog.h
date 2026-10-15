#pragma once

// Export dialog: writes every material of the loaded model into one folder as
// .vmt files, either flat or mirroring materials/ (ready to copy into a game).
//
// Scaffold: the dialog lands with the export work.

#include <QDialog>
#include <QStringList>

namespace bridge {
class EngineView;
}

namespace ui {
namespace qt {

class ExportMaterialsDialog : public QDialog {
    Q_OBJECT

public:
    // `materials` are game-relative names without extension; `modelName` feeds
    // the default destination folder name.
    explicit ExportMaterialsDialog(bridge::EngineView* engine,
                                   QStringList materials,
                                   QString modelName,
                                   QWidget* parent = nullptr);
    ~ExportMaterialsDialog() override = default;

private:
    bridge::EngineView* m_engine = nullptr;
    QStringList m_materials;
    QString m_modelName;
};

} // namespace qt
} // namespace ui
