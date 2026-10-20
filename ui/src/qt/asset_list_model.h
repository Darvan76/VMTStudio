#pragma once

// AssetListModel - the only Qt model the Asset Browser needs.
//
// It is a thin, virtualized view over ui::AssetIndex: a vector of filtered
// entry indices (the index itself is sorted by path), so a 150k-entry game
// costs one filter pass and no per-row widgets. The same model feeds the
// QTableView and the QListView (IconMode); the grid delegate lives here too
// because it is presentation of this model and nothing else.
//
// Sorting is implemented (the table header is clickable and the sort is
// persisted in AssetBrowserState). Name sorts by the full virtual path, which
// is exactly the index order and therefore the default.
//
// Thumbnails are never computed here. The model asks the injected
// AssetThumbnailService for an already decoded pixmap and turns it into a
// small QIcon; the grid delegate reads the full pixmap from the same service.
// data() therefore never blocks on I/O or on the engine.

#include "ui/asset_index.h"

#include <QAbstractTableModel>
#include <QCache>
#include <QIcon>
#include <QString>
#include <QStringList>
#include <QStyledItemDelegate>

#include <cstddef>
#include <cstdint>
#include <vector>

class QMimeData;
class QPainter;

namespace ui {
namespace qt {

class AssetThumbnailService;

class AssetListModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        NameColumn = 0,
        TypeColumn,
        FolderColumn,
        SourceColumn,
        SizeColumn,
        ColumnCount,
    };

    enum Role {
        // Index into ui::AssetIndex::entries() for the filtered row.
        AssetIndexRole = Qt::UserRole + 1,
        VirtualPathRole,   // "materials/brick/brick001.vmt"
        DiskPathRole,      // native path when a loose copy exists, empty otherwise
        KindRole,          // int(ui::AssetKind)
        SourcesRole,       // int of the AssetSource mask
    };

    // Drag payload: an associative QDataStream with
    //   quint32 version (1), qint32 kind, QString virtual_path, QString disk_path.
    // ViewportWidget/MainWindow decode it in W4. Loose rows additionally expose
    // a standard text/uri-list so they can be dropped on external tools.
    static const char* mimeType();

    explicit AssetListModel(QObject* parent = nullptr);

    // The index stays owned by the dialog and must outlive this model's use of
    // it. Changing it invalidates the rows; call refresh() when the data is
    // ready.
    void setIndex(const ui::AssetIndex* index);
    void setThumbnailService(AssetThumbnailService* service);

    // Stores the filter only; refresh() rebuilds the row vector so a dialog
    // can coalesce several changes into one reset.
    void setFilter(const ui::AssetFilter& filter);
    const ui::AssetFilter& filter() const noexcept { return m_filter; }

    void refresh();

    // Programmatic sort used by AssetBrowserState restoration; the table header
    // calls sort() directly.
    void setSort(int column, Qt::SortOrder order);
    int sortColumn() const noexcept { return m_sortColumn; }
    Qt::SortOrder sortOrder() const noexcept { return m_sortOrder; }

    const ui::AssetEntry* entryAt(const QModelIndex& index) const;
    QString virtualPathAt(const QModelIndex& index) const;
    QModelIndex indexForPath(const QString& virtualPath) const;
    int visibleRowCount() const noexcept { return static_cast<int>(m_rows.size()); }

    // QAbstractTableModel
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

signals:
    void filterResultCountChanged(int visibleRows);

private slots:
    void onThumbnailReady(const QString& virtualPath, const QImage& image,
                          const QString& diskPath, bool retryable);

private:
    QString kindLabel(ui::AssetKind kind) const;
    QString sourceLabel(std::uint8_t sources) const;
    bool needsSorting() const;
    void sortRows();

    const ui::AssetIndex* m_index = nullptr;
    const AssetThumbnailService* m_thumbnails = nullptr;
    ui::AssetFilter m_filter;
    std::vector<std::size_t> m_rows;

    int m_sortColumn = NameColumn;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;

    // Small table icons built from the (large) grid pixmaps. Bounded: the
    // service keeps the full-size pictures and can regenerate an icon.
    // Mutable because data() inserts the icon on demand.
    mutable QCache<QString, QIcon> m_icons;
};

// Grid cell painter: thumbnail (or a type placeholder) plus the elided name.
// It reads decoded pixmaps from the thumbnail service, so paint() does no I/O.
class AssetGridDelegate : public QStyledItemDelegate {
public:
    explicit AssetGridDelegate(const AssetThumbnailService* thumbnails,
                               QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

    // The dialog sets QListView::setGridSize() to the same size; keeping the
    // two in sync is what makes indexAt() reliable during the visible-range
    // scan.
    static QSize cellSize();

private:
    const AssetThumbnailService* m_thumbnails = nullptr;
};

} // namespace qt
} // namespace ui
