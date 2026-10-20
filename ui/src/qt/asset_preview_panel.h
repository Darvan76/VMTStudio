#pragma once

// AssetPreviewPanel - the right-hand column of the Asset Browser.
//
// It answers "what is this file?" for the three indexed kinds without ever
// blocking the UI thread:
//   - VMT: shader, parameters and every referenced texture with its size,
//     format and VRAM cost (ui::collect_material_info + format_material_info);
//   - VTF: the decoded picture, format and mip/frame/cubemap metadata;
//   - MDL: path, source and size plus the action that loads it.
//
// Threading contract:
//   - showAsset() starts at most one worker thread (std::thread, detached). A
//     newer request bumps a generation counter, so a slow decode from a
//     previous selection is dropped when it finishes.
//   - The worker reaches the engine through GameTextureSource in Blocking
//     mode: the preview is the one consumer that may wait for the scan gate,
//     because the user is looking at this asset and a "busy" answer would only
//     produce a broken panel.
//   - Results travel back through a small QObject delivery (shared_ptr kept by
//     the job), never through `this`. Closing the browser mid-decode clears the
//     callback; the job then finishes into nothing.
//
// The panel knows nothing about MainWindow: activation is a signal, and the
// dialog (or W4) decides what opening a VMT or loading an MDL means.

#include "asset_thumbnail_service.h"

#include <QImage>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

class QLabel;
class QPushButton;
class QScrollArea;

namespace ui {
namespace qt {

class PreviewDelivery;

// Filled by the worker, applied by the panel (defined in the .cpp).
struct PreviewResult;

class AssetPreviewPanel : public QWidget {
    Q_OBJECT

public:
    explicit AssetPreviewPanel(std::shared_ptr<EngineAccess> access, QWidget* parent = nullptr);
    ~AssetPreviewPanel() override;

    // Disk roots searched before the engine (game dir + user folders). Copied;
    // may be called from the UI thread at any time.
    void setSearchRoots(std::vector<std::filesystem::path> roots);

    // One selected asset. An empty path clears the panel. `sources` is the
    // ui::AssetSource mask, `size` the on-disk size when known (0 = unknown).
    // A repeated call for the same still-loaded asset is a no-op.
    void showAsset(const QString& virtualPath, int kind, const QString& diskPath,
                   int sources, quint64 size);

    // Drops the selection and invalidates any decode in flight.
    void clear();

    // Pick mode hides the activation button: in that mode the dialog owns the
    // primary action ("Seleccionar"), and two competing buttons would blur it.
    void setPickMode(bool enabled);

signals:
    // "Abrir en editor" (VMT) / "Cargar modelo" (MDL). Only emitted when the
    // panel is not in pick mode.
    void activateRequested(const QString& virtualPath, int kind);

private:
    void applyResult(const PreviewResult& result);
    void updateActions();

    std::shared_ptr<EngineAccess> m_access;
    std::shared_ptr<PreviewDelivery> m_delivery;
    std::vector<std::filesystem::path> m_roots;

    QLabel* m_title = nullptr;
    QLabel* m_image = nullptr;
    QLabel* m_info = nullptr;
    QScrollArea* m_scroll = nullptr;
    QPushButton* m_action = nullptr;
    QPushButton* m_copyName = nullptr;

    QString m_path;
    int m_kind = 3; // ui::AssetKind::Other
    int m_generation = 0; // UI thread only

    bool m_loading = false;
    bool m_hasResult = false;
    bool m_pickMode = false;
};

} // namespace qt
} // namespace ui
