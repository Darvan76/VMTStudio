#pragma once

// AssetBrowserDialog - one modeless window over the whole game's assets.
//
// It replaces the two old browsers (materials and models) with a single list
// backed by ui::AssetIndex (W2) and bridge::EngineView::list_assets (W1):
//   - the index is loaded from a per-game cache on open; a scan happens only
//     when the user asks (Refresh), because the backend blocks while it walks
//     the game's file system;
//   - scanning and cache loading run on worker threads and the finished index
//     is swapped in whole, so the UI thread never walks 150k entries;
//   - thumbnails are requested only for visible rows through
//     AssetThumbnailService (bounded pool, per-game cache.pack).
//
// The dialog knows nothing about MainWindow. Opening a VMT, loading an MDL and
// picking a texture are signals: W4 connects them to the editor/viewport.
//
// Public surface used by W4:
//   - constructor(engine, appDataDir) + setEngine()/setGame();
//   - showAll()/showKind(kind)/beginPick(kind)/endPick();
//   - state()/setState() so filters, folders, view mode and geometry survive a
//     restart (W4 stores them with UISettings; this class does not touch it).
//
// Drag payload: the model exports "application/x-vmtstudio-asset" as a
// QDataStream (Qt_6_0 layout): quint32 version (1), qint32 kind, QString
// virtual path, QString disk path. Loose rows also carry text/uri-list.

#include "asset_thumbnail_service.h"

#include <QDialog>
#include <QString>
#include <QStringList>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>

class QAbstractItemView;
class QButtonGroup;
class QComboBox;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QListView;
class QModelIndex;
class QPoint;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QSplitter;
class QStackedWidget;
class QTableView;
class QTimer;
class QToolButton;
class QWidget;

namespace ui {
class AssetIndex;
class AssetPack;
}

namespace ui {
namespace qt {

class AssetListModel;
class AssetPreviewPanel;
class ScanDelivery;
struct AssetScanOutcome;

// Everything the dialog needs to come back exactly as the user left it.
// `kind` is -1 for "All" or a ui::AssetKind value; `source` is the combo index
// (0 = all, 1 = loose, 2 = VPK, 3 = folder). The search text is deliberately
// not persisted: a browser that opens already filtered looks empty.
struct AssetBrowserState {
    int kind = -1;
    int source = 0;
    bool grid = false;
    int sortColumn = 0;
    bool sortAsc = true;
    QStringList folders;
    QString geometryBase64; // QWidget::saveGeometry(), base64
};

class AssetBrowserDialog : public QDialog {
    Q_OBJECT

public:
    AssetBrowserDialog(bridge::EngineView* engine, std::filesystem::path appDataDir,
                       QWidget* parent = nullptr);
    ~AssetBrowserDialog() override;

    // The engine may be null (folder-only session). Changing it invalidates
    // thumbnails but never starts a scan.
    void setEngine(bridge::EngineView* engine);
    bridge::EngineView* engine() const noexcept { return m_engine; }

    // Points the browser at one game: reloads that game's cache (off the UI
    // thread), retargets the per-game pack and clears the current view. The
    // bin directory is only used to launch model-snapshot helpers. An empty
    // path leaves the browser with only user folders.
    void setGame(const std::filesystem::path& gameDir,
                 const std::filesystem::path& binDir = {});
    std::filesystem::path gameDir() const { return m_gameDir; }
    std::filesystem::path binDir() const { return m_binDir; }

    // Pick mode: locks the type chips to `kind`, shows the "Seleccionar"
    // button and makes double-click/Enter emit assetPicked() instead of
    // assetActivated(). `kind` is a ui::AssetKind value (Material, Texture or
    // Model).
    void beginPick(int kind);
    void endPick();
    bool pickMode() const noexcept { return m_pickKind >= 0; }
    int pickKind() const noexcept { return m_pickKind; }

    // Reset the filters: All kinds/sources (showAll) or one kind (showKind).
    // Both leave the search text alone and end pick mode.
    void showAll();
    void showKind(int kind);

    // User folders added with "Add folder…". W4 persists them; setFolders()
    // does not scan (the user's Refresh does).
    QStringList folders() const { return m_folders; }
    void setFolders(const QStringList& folders);

    AssetBrowserState state() const;
    void setState(const AssetBrowserState& state);

signals:
    // Browse mode: open the VMT in the editor, load the MDL, or let W4 decide
    // what to do with a VTF. Pick mode: the chosen asset.
    void assetActivated(const QString& virtualPath, int kind);
    void assetPicked(const QString& virtualPath, int kind);

    // Emitted when the user adds a folder (not by setFolders()).
    void foldersChanged(const QStringList& folders);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    void connectSignals();

    // Filters and presentation.
    void applyFilter();
    void setKindFilter(int kind);
    void setGridMode(bool grid);
    void updateChips();
    void updateViewStack();
    void updateStatus();
    void updateRefreshState();

    // Cache and scans.
    std::filesystem::path cachePackFor(const QString& gameKey) const;
    std::filesystem::path legacyCacheFileFor(const QString& gameKey) const;
    std::filesystem::path legacyThumbnailDirFor(const QString& gameKey) const;
    void loadCacheAsync();
    void refresh();
    void startScan(const std::shared_ptr<ui::AssetIndex>& base);
    void applyScanOutcome(const AssetScanOutcome& outcome);
    void applyIndex(const std::shared_ptr<ui::AssetIndex>& index, std::int64_t generated,
                    bool stale, bool truncated, const std::array<int, 4>& counts,
                    const QString& error, bool reloadPreview);

    // Selection, activation and actions.
    QModelIndex currentIndex() const;
    void selectPath(const QString& virtualPath);
    void onCurrentChanged(const QModelIndex& current);
    void clearPreview();
    void activateCurrent();
    void activatePath(const QString& virtualPath, int kind);
    void pickCurrent();
    void chooseFolder();
    void showContextMenu(QAbstractItemView* view, const QPoint& pos);
    void revealInExplorer(const QString& diskPath);
    void copyToClipboard(const QString& text);
    void focusSearch();

    // Thumbnails.
    void scheduleThumbnailPump();
    void requestVisibleThumbnails();

    void updateSearchRoots();
    void updateModelContext();

    bridge::EngineView* m_engine = nullptr;
    std::filesystem::path m_appDataDir;
    std::filesystem::path m_gameDir;
    std::filesystem::path m_binDir;
    QString m_gameKey;
    QString m_loadedCacheKey;

    std::shared_ptr<EngineAccess> m_access;
    std::shared_ptr<ScanDelivery> m_scanDelivery;
    std::shared_ptr<ui::AssetPack> m_pack;
    AssetThumbnailService* m_thumbnails = nullptr;
    AssetListModel* m_model = nullptr;

    QLineEdit* m_search = nullptr;
    QButtonGroup* m_chipGroup = nullptr;
    QToolButton* m_chipAll = nullptr;
    QToolButton* m_chipMaterial = nullptr;
    QToolButton* m_chipTexture = nullptr;
    QToolButton* m_chipModel = nullptr;
    QComboBox* m_source = nullptr;
    QToolButton* m_listButton = nullptr;
    QToolButton* m_gridButton = nullptr;
    QPushButton* m_addFolderButton = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QPushButton* m_pickButton = nullptr;

    QSplitter* m_splitter = nullptr;
    QStackedWidget* m_views = nullptr;
    QTableView* m_table = nullptr;
    QListView* m_grid = nullptr;
    QWidget* m_emptyPage = nullptr;
    QLabel* m_emptyTitle = nullptr;
    QLabel* m_emptyHint = nullptr;
    QPushButton* m_emptyScan = nullptr;
    QPushButton* m_emptyAdd = nullptr;
    QPushButton* m_emptyClear = nullptr;

    QLabel* m_status = nullptr;
    AssetPreviewPanel* m_preview = nullptr;
    QTimer* m_searchTimer = nullptr;
    QTimer* m_thumbnailTimer = nullptr;

    QStringList m_folders;
    QString m_currentPath;
    std::shared_ptr<ui::AssetIndex> m_index;
    std::array<int, 4> m_counts{};
    std::int64_t m_generated = 0;
    QString m_lastError;

    int m_kind = -1;      // -1 = all, else ui::AssetKind
    int m_pickKind = -1;  // -1 = browse mode
    int m_scanGeneration = 0;
    int m_cacheGeneration = 0;
    bool m_gridMode = false;
    bool m_scanRunning = false;
    bool m_cacheLoading = false;
    bool m_stale = false;
    bool m_truncated = false;
    bool m_pumpingThumbnails = false;
    bool m_firstShow = true;
};

} // namespace qt
} // namespace ui
