#pragma once

// Left-hand dock listing every material of the loaded model, with selection,
// walk-through navigation and the export/isolate actions.
//
// Scaffold: the dock lands with the "model materials" work.

#include <QDockWidget>

#include "bridge/engine_view.h"

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

private:
    bridge::ModelScene m_scene;
};

} // namespace qt
} // namespace ui
