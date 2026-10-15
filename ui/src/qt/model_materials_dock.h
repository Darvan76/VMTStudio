#pragma once

// Left-hand dock listing every material of the loaded model, with selection,
// walk-through navigation and the export/isolate actions.
//
// The dock is presentation only: the main window feeds it a bridge::ModelScene
// dump through setScene(), the grouping and walk order come from
// ui/model_scene.h, and everything the user does (select, activate, export,
// isolate) travels back as signals. The dock never talks to the engine itself.

#include <QDockWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPoint>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QTreeWidget>

#include "bridge/engine_view.h"
#include "ui/model_scene.h"

#include <vector>

namespace ui {
namespace qt {

class ModelMaterialsDock : public QDockWidget {
    Q_OBJECT

public:
    explicit ModelMaterialsDock(QWidget* parent = nullptr);
    ~ModelMaterialsDock() override = default;

    // Repopulates the tree from a model scene dump; empty model_name clears it.
    void setScene(const bridge::ModelScene& scene);
    void clear();

    // Game-relative name of the selected material, empty when none.
    QString currentMaterial() const;

public slots:
    // Walk-through navigation (also bound to Alt+Left / Alt+Right).
    void selectNextMaterial();
    void selectPreviousMaterial();

signals:
    // The user picked a material: preview it in place / open it for editing.
    void materialSelected(const QString& gameMaterialName);
    // Double click or Enter: open the material in the VMT editor.
    void materialActivated(const QString& gameMaterialName);
    // "Export this material..." / "Export selected materials..."
    void materialExportRequested(const QStringList& materials);
    // "Isolate" toggle for one material (hide the rest of the model).
    void isolateToggled(const QString& gameMaterialName, bool enabled);

protected:
    // Enter on the tree activates the current material; double click is wired
    // straight to the same handler.
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onSearchTextChanged(const QString& text);
    void onSelectionChanged();
    void onItemDoubleClicked(QTreeWidgetItem* item);
    void onIsolateToggled(bool checked);
    void onExportAllClicked();
    void onExportSelectedClicked();
    void onTreeContextMenu(const QPoint& pos);

private:
    void buildUi();
    void rebuildTree();
    void applyFilter(const QString& text);
    void updateHeader();
    void updateActions();

    void resetSelection();
    void suspendIsolate(const QString& material);
    void stepMaterial(int delta);
    void selectMaterialByName(const QString& name);
    void activateCurrentItem();

    QString selectedMaterialName() const;
    QTreeWidgetItem* findMaterialItem(const QString& name) const;

    bridge::ModelScene m_scene;
    std::vector<ui::ModelMaterialGroup> m_groups;
    // Game name of the material the dock last reported as selected.
    QString m_selectedMaterial;

    QLabel* m_headerLabel = nullptr;
    QLabel* m_hintLabel = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QPushButton* m_previousButton = nullptr;
    QPushButton* m_nextButton = nullptr;
    QPushButton* m_exportAllButton = nullptr;
    QPushButton* m_exportSelectedButton = nullptr;
    QPushButton* m_isolateButton = nullptr;
    QTreeWidget* m_tree = nullptr;
};

} // namespace qt
} // namespace ui
