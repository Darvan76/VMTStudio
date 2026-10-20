#include "asset_list_model.h"

#include "asset_thumbnail_service.h"
#include "ui/material_info.h"

#include <QDataStream>
#include <QIODevice>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QUrl>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace ui {
namespace qt {

namespace {

// Grid geometry. Kept in one place: the delegate and the visible-range scan
// both rely on every cell occupying exactly this box.
constexpr int kCellWidth = 140;
constexpr int kCellHeight = 158;

// Table decoration size (the icon rebuilt from a grid thumbnail).
constexpr int kIconSize = 18;

char lower_char(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// Case-insensitive comparison over the raw bytes, which is how the index
// orders paths. Done on std::string (not QString) because sorting 150k rows
// would otherwise allocate a temporary string per comparison.
int compare_ci(std::string_view a, std::string_view b) {
    const std::size_t shared = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < shared; ++i) {
        const char ca = lower_char(a[i]);
        const char cb = lower_char(b[i]);
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
    }
    if (a.size() == b.size()) {
        return 0;
    }
    return a.size() < b.size() ? -1 : 1;
}

std::string_view filename_of(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string_view folder_of(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? std::string_view() : path.substr(0, slash);
}

// Sort key of one column. Every case falls back to the path so the order stays
// deterministic and matches the index order for equal keys.
int compare_entries(int column, const ui::AssetEntry& a, const ui::AssetEntry& b) {
    int cmp = 0;
    switch (column) {
    case AssetListModel::NameColumn:
        break; // the path itself
    case AssetListModel::TypeColumn:
        if (a.kind != b.kind) {
            cmp = static_cast<int>(a.kind) < static_cast<int>(b.kind) ? -1 : 1;
        }
        break;
    case AssetListModel::FolderColumn:
        cmp = compare_ci(folder_of(a.virtual_path), folder_of(b.virtual_path));
        if (cmp == 0) {
            cmp = compare_ci(filename_of(a.virtual_path), filename_of(b.virtual_path));
        }
        break;
    case AssetListModel::SourceColumn:
        if (a.sources != b.sources) {
            cmp = a.sources < b.sources ? -1 : 1;
        }
        break;
    case AssetListModel::SizeColumn:
        if (a.size != b.size) {
            cmp = a.size < b.size ? -1 : 1;
        }
        break;
    default:
        break;
    }
    if (cmp == 0) {
        cmp = compare_ci(a.virtual_path, b.virtual_path);
    }
    if (cmp == 0) {
        // Plain byte order as the last tie-break: mixed-case paths with the
        // same lowercase spelling cannot swap places between sorts.
        cmp = a.virtual_path.compare(b.virtual_path);
        cmp = cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
    }
    return cmp;
}

// Short, untranslated badge used by the grid delegate ("VMT"/"VTF"/"MDL").
QString kind_badge(int kind) {
    switch (kind) {
    case static_cast<int>(ui::AssetKind::Material):
        return QStringLiteral("VMT");
    case static_cast<int>(ui::AssetKind::Texture):
        return QStringLiteral("VTF");
    case static_cast<int>(ui::AssetKind::Model):
        return QStringLiteral("MDL");
    default:
        break;
    }
    return QStringLiteral("?");
}

QColor kind_badge_color(int kind) {
    switch (kind) {
    case static_cast<int>(ui::AssetKind::Material):
        return QColor(78, 201, 176); // teal, same family as the theme accents
    case static_cast<int>(ui::AssetKind::Texture):
        return QColor(220, 220, 170);
    case static_cast<int>(ui::AssetKind::Model):
        return QColor(86, 156, 214);
    default:
        break;
    }
    return QColor(150, 150, 150);
}

} // namespace

// --- AssetListModel -----------------------------------------------------------

const char* AssetListModel::mimeType() {
    return "application/x-vmtstudio-asset";
}

AssetListModel::AssetListModel(QObject* parent) : QAbstractTableModel(parent) {
    m_icons.setMaxCost(8192);
}

void AssetListModel::setIndex(const ui::AssetIndex* index) {
    m_index = index;
    m_rows.clear();
    // Paths can repeat between games ("materials/brick/brick.vmt"); an icon
    // left from the previous index would show the wrong picture.
    m_icons.clear();
}

void AssetListModel::setThumbnailService(AssetThumbnailService* service) {
    if (m_thumbnails == service) {
        return;
    }
    if (m_thumbnails != nullptr) {
        disconnect(m_thumbnails, nullptr, this, nullptr);
    }
    m_thumbnails = service;
    if (m_thumbnails != nullptr) {
        connect(m_thumbnails, &AssetThumbnailService::thumbnailReady, this,
                &AssetListModel::onThumbnailReady);
    }
}

void AssetListModel::setFilter(const ui::AssetFilter& filter) {
    m_filter = filter;
}

void AssetListModel::refresh() {
    beginResetModel();
    m_rows.clear();
    if (m_index != nullptr) {
        m_rows = m_index->filter(m_filter);
    }
    if (needsSorting()) {
        sortRows();
    }
    endResetModel();
    emit filterResultCountChanged(visibleRowCount());
}

void AssetListModel::setSort(int column, Qt::SortOrder order) {
    if (column < 0 || column >= ColumnCount) {
        column = NameColumn;
    }
    if (m_sortColumn == column && m_sortOrder == order) {
        return;
    }
    if (m_rows.empty() || m_index == nullptr) {
        m_sortColumn = column;
        m_sortOrder = order;
        return;
    }
    sort(column, order);
}

bool AssetListModel::needsSorting() const {
    // The index is already sorted by path, so the default state is a no-op.
    return !(m_sortColumn == NameColumn && m_sortOrder == Qt::AscendingOrder);
}

void AssetListModel::sortRows() {
    if (m_index == nullptr) {
        return;
    }
    const std::vector<ui::AssetEntry>& entries = m_index->entries();
    const int column = m_sortColumn;
    const bool ascending = m_sortOrder == Qt::AscendingOrder;
    std::stable_sort(m_rows.begin(), m_rows.end(),
                     [&entries, column, ascending](std::size_t a, std::size_t b) {
                         const int cmp = compare_entries(column, entries[a], entries[b]);
                         return ascending ? cmp < 0 : cmp > 0;
                     });
}

void AssetListModel::sort(int column, Qt::SortOrder order) {
    if (column < 0 || column >= ColumnCount) {
        return;
    }
    if (m_sortColumn == column && m_sortOrder == order) {
        return; // the header can re-notify the current order after a reset
    }
    m_sortColumn = column;
    m_sortOrder = order;
    if (m_rows.empty() || m_index == nullptr) {
        return;
    }

    emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
    const QModelIndexList oldIndexes = persistentIndexList();
    std::vector<QString> oldPaths;
    oldPaths.reserve(static_cast<std::size_t>(oldIndexes.size()));
    for (const QModelIndex& old : oldIndexes) {
        const ui::AssetEntry* entry = entryAt(old);
        oldPaths.push_back(entry != nullptr ? QString::fromStdString(entry->virtual_path)
                                            : QString());
    }

    sortRows();

    // Follow the moved rows by path (a row index would point at whatever
    // landed there after the sort).
    QModelIndexList newIndexes;
    newIndexes.reserve(oldIndexes.size());
    const int count = static_cast<int>(oldIndexes.size());
    for (int i = 0; i < count; ++i) {
        const QModelIndex& old = oldIndexes[i];
        const QString& path = oldPaths[static_cast<std::size_t>(i)];
        const QModelIndex fresh = path.isEmpty() ? QModelIndex() : indexForPath(path);
        newIndexes.append(fresh.isValid() ? index(fresh.row(), old.column()) : QModelIndex());
    }
    changePersistentIndexList(oldIndexes, newIndexes);
    emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
}

const ui::AssetEntry* AssetListModel::entryAt(const QModelIndex& index) const {
    if (m_index == nullptr || !index.isValid()) {
        return nullptr;
    }
    const int row = index.row();
    if (row < 0 || row >= static_cast<int>(m_rows.size())) {
        return nullptr;
    }
    const std::size_t entry = m_rows[static_cast<std::size_t>(row)];
    if (entry >= m_index->entries().size()) {
        return nullptr;
    }
    return &m_index->entries()[entry];
}

QString AssetListModel::virtualPathAt(const QModelIndex& index) const {
    const ui::AssetEntry* entry = entryAt(index);
    return entry != nullptr ? QString::fromStdString(entry->virtual_path) : QString();
}

QModelIndex AssetListModel::indexForPath(const QString& virtualPath) const {
    if (m_index == nullptr || virtualPath.isEmpty()) {
        return {};
    }
    // The index precomputes path_lower for exactly this kind of lookup.
    const std::string needle = virtualPath.toLower().toStdString();
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        if (m_index->entries()[m_rows[i]].path_lower == needle) {
            return createIndex(static_cast<int>(i), 0);
        }
    }
    return {};
}

int AssetListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_rows.size());
}

int AssetListModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return ColumnCount;
}

QString AssetListModel::kindLabel(ui::AssetKind kind) const {
    switch (kind) {
    case ui::AssetKind::Material:
        return tr("Material");
    case ui::AssetKind::Texture:
        return tr("Texture");
    case ui::AssetKind::Model:
        return tr("Model");
    case ui::AssetKind::Other:
        break;
    }
    return tr("Other");
}

QString AssetListModel::sourceLabel(std::uint8_t sources) const {
    QStringList parts;
    if ((sources & ui::SourceLoose) != 0) {
        parts.append(tr("Loose"));
    }
    if ((sources & ui::SourcePacked) != 0) {
        parts.append(tr("VPK"));
    }
    if ((sources & ui::SourceFolder) != 0) {
        parts.append(tr("Folder"));
    }
    return parts.isEmpty() ? tr("Unknown") : parts.join(QStringLiteral(" + "));
}

QVariant AssetListModel::data(const QModelIndex& index, int role) const {
    const ui::AssetEntry* entry = entryAt(index);
    if (entry == nullptr) {
        return {};
    }

    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case NameColumn:
            return QString::fromStdString(std::string(filename_of(entry->virtual_path)));
        case TypeColumn:
            return kindLabel(entry->kind);
        case FolderColumn: {
            const std::string_view folder = folder_of(entry->virtual_path);
            return folder.empty() ? QStringLiteral("-") : QString::fromStdString(std::string(folder));
        }
        case SourceColumn:
            return sourceLabel(entry->sources);
        case SizeColumn:
            return entry->size == 0 ? QStringLiteral("-")
                                    : QString::fromStdString(ui::format_bytes(entry->size));
        default:
            break;
        }
        break;
    case Qt::ToolTipRole: {
        QString tip = QString::fromStdString(entry->virtual_path);
        tip += QLatin1Char('\n') + kindLabel(entry->kind) + QStringLiteral(" - ") +
               sourceLabel(entry->sources);
        if (entry->size != 0) {
            tip += QLatin1Char('\n') + QString::fromStdString(ui::format_bytes(entry->size));
        }
        if (!entry->disk_path.empty()) {
            tip += QLatin1Char('\n') + QString::fromStdWString(entry->disk_path.wstring());
        }
        return tip;
    }
    case Qt::DecorationRole:
        if (index.column() == NameColumn) {
            const QString path = QString::fromStdString(entry->virtual_path);
            if (const QIcon* icon = m_icons.object(path)) {
                return *icon;
            }
            // No icon yet: build the small one from the decoded grid pixmap
            // (in-memory only, never a decode).
            if (m_thumbnails != nullptr) {
                if (const QPixmap* pixmap = m_thumbnails->pixmap(path)) {
                    const QPixmap small = pixmap->scaled(kIconSize, kIconSize,
                                                         Qt::KeepAspectRatio,
                                                         Qt::SmoothTransformation);
                    m_icons.insert(path, new QIcon(small), 1);
                    if (const QIcon* stored = m_icons.object(path)) {
                        return *stored;
                    }
                }
            }
        }
        break;
    case Qt::TextAlignmentRole:
        if (index.column() == SizeColumn) {
            return QVariant(static_cast<int>(Qt::AlignRight | Qt::AlignVCenter));
        }
        break;
    case AssetIndexRole:
        return QVariant::fromValue<qulonglong>(m_rows[static_cast<std::size_t>(index.row())]);
    case VirtualPathRole:
        return QString::fromStdString(entry->virtual_path);
    case DiskPathRole:
        return entry->disk_path.empty() ? QString()
                                        : QString::fromStdWString(entry->disk_path.wstring());
    case KindRole:
        return static_cast<int>(entry->kind);
    case SourcesRole:
        return static_cast<int>(entry->sources);
    default:
        break;
    }
    return {};
}

QVariant AssetListModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case NameColumn:
        return tr("Name");
    case TypeColumn:
        return tr("Type");
    case FolderColumn:
        return tr("Folder");
    case SourceColumn:
        return tr("Source");
    case SizeColumn:
        return tr("Size");
    default:
        break;
    }
    return {};
}

Qt::ItemFlags AssetListModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags base = QAbstractTableModel::flags(index);
    if (index.isValid() && entryAt(index) != nullptr) {
        base |= Qt::ItemIsDragEnabled;
    }
    return base;
}

QStringList AssetListModel::mimeTypes() const {
    return {QString::fromLatin1(mimeType()), QStringLiteral("text/uri-list")};
}

QMimeData* AssetListModel::mimeData(const QModelIndexList& indexes) const {
    const ui::AssetEntry* entry = nullptr;
    for (const QModelIndex& index : indexes) {
        if (index.column() != NameColumn) {
            continue;
        }
        entry = entryAt(index);
        if (entry != nullptr) {
            break;
        }
    }
    if (entry == nullptr) {
        return nullptr;
    }

    const QString path = QString::fromStdString(entry->virtual_path);
    const QString disk = entry->disk_path.empty()
                             ? QString()
                             : QString::fromStdWString(entry->disk_path.wstring());

    auto* mime = new QMimeData();
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    // Explicit version so the drop side (W4) reads the same layout whatever Qt
    // version it was compiled against.
    stream.setVersion(QDataStream::Qt_6_0);
    stream << quint32(1) << qint32(static_cast<int>(entry->kind)) << path << disk;
    mime->setData(QString::fromLatin1(mimeType()), payload);

    if (!entry->disk_path.empty()) {
        // Standard format for the loose copy, so external tools (and the
        // viewport) get a normal file drop.
        mime->setUrls({QUrl::fromLocalFile(QString::fromStdWString(entry->disk_path.wstring()))});
    }
    return mime;
}

void AssetListModel::onThumbnailReady(const QString& virtualPath, const QImage& image,
                                      const QString& diskPath, bool retryable) {
    Q_UNUSED(diskPath);
    Q_UNUSED(retryable);
    if (image.isNull()) {
        return;
    }
    const QPixmap pixmap = QPixmap::fromImage(image);
    if (pixmap.isNull()) {
        return;
    }
    const QPixmap small =
        pixmap.scaled(kIconSize, kIconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_icons.insert(virtualPath, new QIcon(small), 1);

    if (m_rows.empty()) {
        return;
    }
    // The delegate reads the full pixmap from the service; this notification
    // only tells the views to repaint the row.
    emit dataChanged(index(0, NameColumn), index(static_cast<int>(m_rows.size()) - 1, NameColumn),
                     {Qt::DecorationRole});
}

// --- AssetGridDelegate --------------------------------------------------------

AssetGridDelegate::AssetGridDelegate(const AssetThumbnailService* thumbnails, QObject* parent)
    : QStyledItemDelegate(parent), m_thumbnails(thumbnails) {}

QSize AssetGridDelegate::cellSize() {
    return QSize(kCellWidth, kCellHeight);
}

QSize AssetGridDelegate::sizeHint(const QStyleOptionViewItem& option,
                                  const QModelIndex& index) const {
    Q_UNUSED(option);
    Q_UNUSED(index);
    return cellSize();
}

void AssetGridDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const {
    const int kind = index.data(AssetListModel::KindRole).toInt();
    const QString path = index.data(AssetListModel::VirtualPathRole).toString();
    const bool selected = (option.state & QStyle::State_Selected) != 0;

    painter->save();
    const QRect box = option.rect.adjusted(3, 3, -3, -3);
    if (selected) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(option.palette.highlight());
        painter->drawRoundedRect(box, 4, 4);
    } else if ((option.state & QStyle::State_MouseOver) != 0) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(255, 255, 255, 18));
        painter->drawRoundedRect(box, 4, 4);
    }

    const int labelHeight = option.fontMetrics.height() + 4;
    const QRect imageBox(box.left() + 4, box.top() + 4, box.width() - 8,
                         box.height() - labelHeight - 8);

    const QPixmap* pixmap = m_thumbnails != nullptr ? m_thumbnails->pixmap(path) : nullptr;
    if (pixmap != nullptr && !pixmap->isNull()) {
        const QSize scaled = pixmap->size().scaled(imageBox.size(), Qt::KeepAspectRatio);
        const QRect target(imageBox.left() + (imageBox.width() - scaled.width()) / 2,
                           imageBox.top() + (imageBox.height() - scaled.height()) / 2,
                           scaled.width(), scaled.height());
        painter->drawPixmap(target, *pixmap);
    } else {
        // Placeholder: no thumbnail yet (or the format has no 8-bit preview).
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(45, 45, 48));
        painter->drawRoundedRect(imageBox, 4, 4);
        painter->setPen(kind_badge_color(kind));
        QFont badgeFont = option.font;
        badgeFont.setBold(true);
        painter->setFont(badgeFont);
        painter->drawText(imageBox, Qt::AlignCenter, kind_badge(kind));
        painter->setFont(option.font);
    }

    // Type badge over the picture so materials and textures stay tellable
    // apart even when the thumbnails look similar.
    const QString badge = kind_badge(kind);
    const QRect badgeRect(imageBox.left() + 4, imageBox.top() + 4,
                          option.fontMetrics.horizontalAdvance(badge) + 8,
                          option.fontMetrics.height() + 2);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(0, 0, 0, 150)); 
    painter->drawRoundedRect(badgeRect, 3, 3);
    painter->setPen(kind_badge_color(kind));
    QFont badgeFont2 = option.font;
    badgeFont2.setBold(true);
    badgeFont2.setPointSizeF(badgeFont2.pointSizeF() - 0.5);
    painter->setFont(badgeFont2);
    painter->drawText(badgeRect, Qt::AlignCenter, badge);
    painter->setFont(option.font);

    // Name, elided in the middle: the extension and the start of the name are
    // the informative halves.
    const QString name = index.data(Qt::DisplayRole).toString();
    const QRect nameRect(box.left() + 3, imageBox.bottom() + 3, box.width() - 6, labelHeight);
    const QString elided = option.fontMetrics.elidedText(name, Qt::ElideMiddle, nameRect.width());
    painter->setPen(selected ? option.palette.color(QPalette::HighlightedText)
                             : option.palette.color(QPalette::Text));
    painter->drawText(nameRect, Qt::AlignHCenter | Qt::AlignVCenter, elided);

    // Selection focus ring (keyboard users need to see the current cell even
    // when the palette highlight is subtle).
    if (selected && (option.state & QStyle::State_HasFocus) != 0) {
        painter->setPen(option.palette.color(QPalette::HighlightedText));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(box.adjusted(1, 1, -1, -1), 4, 4);
    }
    painter->restore();
}

} // namespace qt
} // namespace ui