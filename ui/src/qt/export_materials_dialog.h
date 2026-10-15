#pragma once

// Export dialog: writes every material of the loaded model into one folder as
// .vmt files, either flat or mirroring materials/ (ready to copy into a game).
//
// The dialog is self-contained: it plans with ui::plan_material_export(), reads
// the VMT text through the EngineView it was given and writes each file
// atomically (".tmp" + rename). Results stay visible in the dialog (summary,
// error log, "Open Folder") instead of closing it, so a partial export can be
// inspected and retried.

#include <QDialog>
#include <QStringList>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;

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
    // "Browse..." for the destination folder.
    void browseForDestination();
    // Rebuilds the plan preview, the status line and the Export button state.
    void refreshPreview();
    // Runs the export loop with a cancellable progress dialog.
    void exportAll();
    // Appends one line to the read-only result log.
    void appendLog(const QString& line);

    bridge::EngineView* m_engine = nullptr;
    QStringList m_materials;
    QString m_modelName;

    QLineEdit* m_destinationEdit = nullptr;
    QPushButton* m_browseButton = nullptr;
    QRadioButton* m_flatRadio = nullptr;
    QRadioButton* m_treeRadio = nullptr;
    QListWidget* m_previewList = nullptr;
    QCheckBox* m_overwriteCheck = nullptr;
    QLabel* m_statusLabel = nullptr;
    QPlainTextEdit* m_logEdit = nullptr;
    QPushButton* m_openFolderButton = nullptr;
    QPushButton* m_exportButton = nullptr;
    QPushButton* m_closeButton = nullptr;

    QString m_lastDestination; // remembered for "Open Folder"
};

} // namespace qt
} // namespace ui
