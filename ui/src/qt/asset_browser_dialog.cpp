#include "asset_browser_dialog.h"

#include "asset_list_model.h"
#include "asset_preview_panel.h"

#include "app_log.h"
#include "ui/asset_cache.h"
#include "ui/asset_index.h"
#include "ui/asset_pack.h"
#include "ui/material_info.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QImage>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QMetaObject>
#include <QProcess>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ui {
namespace qt {

namespace {

constexpr int kSearchDebounceMs = 150;
constexpr int kThumbnailPumpMs = 120;
constexpr int kVisibleThumbnailCap = 300;

// Strings composed by helpers that have no access to the dialog object.
class DialogText {
    Q_DECLARE_TR_FUNCTIONS(AssetBrowserDialog)
};

std::string to_lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

QString absolute_game_dir_string(const std::filesystem::path& game_dir) {
    if (game_dir.empty()) {
        return {};
    }
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(game_dir, ec);
    return QString::fromStdWString((ec ? game_dir : absolute).wstring());
}

QString format_age(std::int64_t generated) {
    if (generated <= 0) {
        return DialogText::tr("sin caché");
    }
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const std::int64_t seconds = std::max<std::int64_t>(0, now - generated);
    const QLocale locale;
    if (seconds < 90) {
        return DialogText::tr("hace %1 s").arg(locale.toString(seconds));
    }
    if (seconds < 90 * 60) {
        return DialogText::tr("hace %1 min").arg(locale.toString(seconds / 60));
    }
    if (seconds < 48 * 3600) {
        return DialogText::tr("hace %1 h").arg(locale.toString(seconds / 3600));
    }
    return DialogText::tr("hace %1 días").arg(locale.toString(seconds / 86400));
}

// Cheap stale check from §7.1: a newer *.vpk or a newer materials/models
// folder means the cached listing may be incomplete. It never triggers a scan
// on its own; the status simply suggests Refresh.
//
// The reference point is the cache file's own mtime (written right after
// `generated` is stamped), which avoids converting file_time_type to time_t:
// that conversion is not portable to MSVC's file clock.
bool cache_is_stale(const std::filesystem::path& game_dir,
                    const std::filesystem::path& cache_file) {
    if (game_dir.empty() || cache_file.empty()) {
        return false;
    }
    std::error_code cache_ec;
    const auto cache_time = std::filesystem::last_write_time(cache_file, cache_ec);
    if (cache_ec) {
        return false;
    }
    const auto newer = [cache_time](const std::filesystem::path& path) {
        std::error_code time_ec;
        const auto write_time = std::filesystem::last_write_time(path, time_ec);
        return !time_ec && write_time > cache_time;
    };

    std::error_code iter_ec;
    std::filesystem::directory_iterator it(game_dir, iter_ec);
    const std::filesystem::directory_iterator end;
    int inspected = 0;
    while (!iter_ec && it != end && inspected < 500) {
        std::error_code type_ec;
        if (it->is_regular_file(type_ec)) {
            const std::string extension = to_lower(it->path().extension().string());
            if (extension == ".vpk" && newer(it->path())) {
                return true;
            }
        }
        it.increment(iter_ec);
        ++inspected;
    }

    return newer(game_dir / "materials") || newer(game_dir / "models");
}

std::array<int, 4> count_kinds(const ui::AssetIndex& index) {
    std::array<int, 4> counts{};
    for (const ui::AssetEntry& entry : index.entries()) {
        ++counts[0];
        const std::size_t slot = static_cast<std::size_t>(entry.kind) + 1;
        if (slot < counts.size()) {
            ++counts[slot];
        }
    }
    return counts;
}

QString kind_label(int kind) {
    switch (kind) {
    case static_cast<int>(ui::AssetKind::Material):
        return DialogText::tr("Material");
    case static_cast<int>(ui::AssetKind::Texture):
        return DialogText::tr("Texture");
    case static_cast<int>(ui::AssetKind::Model):
        return DialogText::tr("Model");
    case static_cast<int>(ui::AssetKind::Other):
        break;
    }
    return DialogText::tr("Other");
}

QString disk_path_of(const ui::AssetEntry& entry) {
    return entry.disk_path.empty() ? QString()
                                   : QString::fromStdWString(entry.disk_path.wstring());
}

// Disk path of a row for preview/actions. Engine dumps only say "loose"; the
// index does not keep the absolute path, so resolve it against the game root
// on demand (one stat, and only when the user selects or right-clicks).
QString resolved_disk_path(const ui::AssetEntry& entry,
                           const std::filesystem::path& game_dir) {
    if (!entry.disk_path.empty()) {
        return QString::fromStdWString(entry.disk_path.wstring());
    }
    if ((entry.sources & ui::SourceLoose) == 0 || game_dir.empty()) {
        return {};
    }
    const std::filesystem::path candidate =
        game_dir / std::filesystem::path(entry.virtual_path);
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec)) {
        return QString::fromStdWString(candidate.wstring());
    }
    return {};
}

std::vector<std::filesystem::path> search_roots(const std::filesystem::path& game_dir,
                                                const QStringList& folders) {
    std::vector<std::filesystem::path> roots;
    if (!game_dir.empty()) {
        roots.push_back(game_dir);
    }
    for (const QString& folder : folders) {
        if (!folder.isEmpty()) {
            roots.emplace_back(folder.toStdWString());
        }
    }
    return roots;
}

// qCompress/qUncompress are the UI-side codec. AssetPack itself stays Qt-free
// so ui_tests can exercise the format without pulling in Qt.
std::vector<unsigned char> q_compress_bytes(const std::vector<unsigned char>& input) {
    if (input.empty()) {
        return {};
    }
    const QByteArray source(reinterpret_cast<const char*>(input.data()),
                            static_cast<int>(input.size()));
    const QByteArray compressed = qCompress(source, 9);
    return std::vector<unsigned char>(compressed.begin(), compressed.end());
}

bool q_decompress_bytes(const std::vector<unsigned char>& input,
                        std::vector<unsigned char>& out) {
    if (input.empty()) {
        out.clear();
        return true;
    }
    const QByteArray source(reinterpret_cast<const char*>(input.data()),
                            static_cast<int>(input.size()));
    const QByteArray raw = qUncompress(source);
    if (raw.isNull()) {
        return false;
    }
    out.assign(raw.begin(), raw.end());
    return true;
}

std::shared_ptr<ui::AssetPack> make_asset_pack() {
    return std::make_shared<ui::AssetPack>(q_compress_bytes, q_decompress_bytes);
}

std::filesystem::path backend_executable() {
    const QString path =
        QCoreApplication::applicationDirPath() + QStringLiteral("/hlmv_backend.exe");
    return std::filesystem::path(path.toStdWString());
}

// One-time import of the old assets-v1.tsv plus the loose *.bmp thumbnails.
// The pack is the only cache from now on; re-encoding the BMPs as PNG keeps
// the migration from inflating the archive with raw bitmaps.
void migrate_legacy_cache(const std::shared_ptr<ui::AssetPack>& pack,
                          const std::filesystem::path& legacy_index,
                          const std::filesystem::path& legacy_thumbs,
                          const std::string& game_dir_string) {
    if (!pack || !pack->is_open()) {
        return;
    }
    std::error_code ec;

    if (!legacy_index.empty() && std::filesystem::is_regular_file(legacy_index, ec)) {
        bool remove_index = true;
        if (!pack->contains(ui::kAssetPackTagMeta, "index")) {
            std::ifstream stream(legacy_index, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(stream)),
                                   std::istreambuf_iterator<char>());
            const ui::AssetCacheLoad loaded = ui::parse_asset_cache(text, game_dir_string);
            if (loaded.loaded) {
                const std::string data = ui::serialize_asset_cache(loaded.index, loaded.meta);
                const std::vector<unsigned char> bytes(data.begin(), data.end());
                remove_index = pack->put(ui::kAssetPackTagMeta, "index", bytes, true);
            }
        }
        if (remove_index) {
            std::filesystem::remove(legacy_index, ec);
        }
    }

    if (!legacy_thumbs.empty() && std::filesystem::is_directory(legacy_thumbs, ec)) {
        for (const auto& item : std::filesystem::directory_iterator(legacy_thumbs, ec)) {
            if (ec || !item.is_regular_file()) {
                continue;
            }
            std::string extension = item.path().extension().string();
            for (char& c : extension) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (extension != ".bmp") {
                continue;
            }
            const std::string key = item.path().stem().string();
            if (pack->contains(ui::kAssetPackTagThumb, key)) {
                std::filesystem::remove(item.path(), ec);
                continue;
            }
            std::ifstream stream(item.path(), std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(stream)),
                                    std::istreambuf_iterator<char>());
            bool stored = false;
            QImage image;
            if (image.loadFromData(reinterpret_cast<const uchar*>(bytes.data()),
                                   static_cast<int>(bytes.size()))) {
                QByteArray png;
                QBuffer buffer(&png);
                if (buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG")) {
                    const std::vector<unsigned char> data(png.begin(), png.end());
                    stored = pack->put(ui::kAssetPackTagThumb, key, data, false);
                }
            }
            if (stored) {
                std::filesystem::remove(item.path(), ec);
            }
        }
        std::filesystem::remove(legacy_thumbs, ec); // only if now empty
    }
}

} // namespace

// --- worker results and delivery ---------------------------------------------

struct AssetScanOutcome {
    std::shared_ptr<ui::AssetIndex> index;
    std::shared_ptr<ui::AssetPack> pack;
    QString error;
    bool loadedFromCache = false;
    bool engineScanned = false;
    bool truncated = false;
    bool stale = false;
    std::int64_t generated = 0;
    std::array<int, 4> counts{};
    int generation = 0;
};

// Lives on the UI thread. Workers keep it alive with a shared_ptr and post
// through it; a destroyed dialog clears the handler, so the queued call is a
// no-op instead of touching freed memory.
class ScanDelivery : public QObject {
public:
    std::function<void(const AssetScanOutcome&)> handler;
};

namespace {

AssetScanOutcome load_cache_worker(const std::filesystem::path& pack_file,
                                   const std::filesystem::path& legacy_index,
                                   const std::filesystem::path& legacy_thumbs,
                                   const std::string& game_dir_string,
                                   const std::filesystem::path& game_dir) {
    AssetScanOutcome outcome;
    outcome.loadedFromCache = true;
    if (pack_file.empty() || game_dir_string.empty()) {
        return outcome;
    }

    std::shared_ptr<ui::AssetPack> pack = make_asset_pack();
    if (!pack->open(pack_file, true)) {
        return outcome; // unwritable folder; the browser still works in memory
    }
    migrate_legacy_cache(pack, legacy_index, legacy_thumbs, game_dir_string);
    outcome.pack = pack;

    std::vector<unsigned char> bytes;
    if (!pack->get(ui::kAssetPackTagMeta, "index", bytes) || bytes.empty()) {
        return outcome; // first run: an empty pack is still usable for thumbs
    }
    const std::string text(bytes.begin(), bytes.end());
    ui::AssetCacheLoad loaded = ui::parse_asset_cache(text, game_dir_string);
    if (!loaded.loaded) {
        return outcome; // another game, or a damaged index
    }

    outcome.generated = loaded.meta.generated;
    outcome.truncated = loaded.index.truncated();
    outcome.counts = count_kinds(loaded.index);
    outcome.stale = cache_is_stale(game_dir, pack_file);
    outcome.index = std::make_shared<ui::AssetIndex>(std::move(loaded.index));
    return outcome;
}

AssetScanOutcome scan_worker(bridge::EngineView* engine, bool engine_ready,
                             const std::shared_ptr<EngineAccess>& access,
                             const std::string& game_dir_string, const QStringList& folders,
                             const std::filesystem::path& pack_file,
                             const std::filesystem::path& legacy_index,
                             const std::filesystem::path& legacy_thumbs,
                             std::shared_ptr<ui::AssetPack> pack,
                             const std::shared_ptr<ui::AssetIndex>& base) {
    AssetScanOutcome outcome;
    std::shared_ptr<ui::AssetIndex> built =
        base != nullptr ? std::make_shared<ui::AssetIndex>(*base)
                        : std::make_shared<ui::AssetIndex>();

    if ((!pack || !pack->is_open()) && !pack_file.empty()) {
        pack = make_asset_pack();
        if (pack->open(pack_file, true)) {
            migrate_legacy_cache(pack, legacy_index, legacy_thumbs, game_dir_string);
        } else {
            pack.reset();
        }
    }

    if (engine != nullptr) {
        if (!engine_ready) {
            outcome.error = DialogText::tr("El motor no está conectado todavía.");
        } else {
            // The backend answers one request at a time; the gate is shared
            // with thumbnails and the preview panel.
            vmt::Result<bridge::AssetList> list = vmt::Err<bridge::AssetList>(
                vmt::make_error(vmt::ErrorCode::Internal, "the engine did not answer"));
            {
                const std::lock_guard<EngineAccess> guard(*access);
                // Never trust the captured pointer: setEngine(nullptr) waits
                // for this gate before the owner destroys the EngineView, and
                // a queued scan must observe the cleared pointer.
                bridge::EngineView* active = access->engine();
                if (active != nullptr && active->initialized()) {
                    list = active->list_assets({"materials", "models"},
                                               {".vmt", ".vtf", ".mdl"});
                } else {
                    outcome.error = DialogText::tr("El motor no está conectado todavía.");
                }
            }
            if (!list) {
                outcome.error = QString::fromStdString(list.error().message);
            } else {
                std::vector<ui::ScannedAsset> scanned;
                scanned.reserve(list.value().entries.size());
                for (const bridge::AssetRecord& record : list.value().entries) {
                    scanned.push_back({record.path, record.loose});
                }
                built->add_scanned_assets(scanned, ui::SourcePacked);
                outcome.engineScanned = true;
                outcome.truncated = list.value().truncated;
            }
        }
    }

    for (const QString& folder : folders) {
        if (!folder.isEmpty()) {
            built->add_directory(std::filesystem::path(folder.toStdWString()),
                                 ui::SourceFolder);
        }
    }
    outcome.truncated = outcome.truncated || built->truncated();

    if (outcome.engineScanned && !game_dir_string.empty() && pack && pack->is_open()) {
        ui::AssetCacheMeta meta;
        meta.game_dir = game_dir_string;
        meta.generated = static_cast<std::int64_t>(std::time(nullptr));
        meta.roots = {"materials", "models"};
        meta.extensions = {".vmt", ".vtf", ".mdl"};
        outcome.generated = meta.generated;
        const std::string data = ui::serialize_asset_cache(*built, meta);
        const std::vector<unsigned char> bytes(data.begin(), data.end());
        if (!pack->put(ui::kAssetPackTagMeta, "index", bytes, true)) {
            ui::log_write("assets", "asset pack index write failed");
        } else {
            // Refresh means the old pictures may describe files that changed or
            // disappeared. They are regenerated lazily on the new generation.
            (void)pack->remove_tag(ui::kAssetPackTagThumb);
            (void)pack->remove_tag(ui::kAssetPackTagModel);
        }
    }

    outcome.pack = std::move(pack);
    outcome.counts = count_kinds(*built);
    outcome.index = std::move(built);
    return outcome;
}

} // namespace

// --- AssetBrowserDialog -------------------------------------------------------

AssetBrowserDialog::AssetBrowserDialog(bridge::EngineView* engine,
                                       std::filesystem::path appDataDir, QWidget* parent)
    : QDialog(parent),
      m_engine(engine),
      m_appDataDir(std::move(appDataDir)),
      m_access(std::make_shared<EngineAccess>()),
      m_scanDelivery(std::make_shared<ScanDelivery>()) {
    m_access->setEngine(engine);

    setWindowTitle(tr("Asset Browser"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    resize(1100, 720);

    m_thumbnails = new AssetThumbnailService(m_access, this);
    m_model = new AssetListModel(this);
    m_model->setThumbnailService(m_thumbnails);

    buildUi();
    connectSignals();
    updateChips();
    applyFilter();
    updateRefreshState();

    m_scanDelivery->handler = [this](const AssetScanOutcome& outcome) {
        applyScanOutcome(outcome);
    };
}

AssetBrowserDialog::~AssetBrowserDialog() {
    // In-flight workers keep their generation/dispatch objects alive; they
    // must not reach the dialog after this point.
    ++m_scanGeneration;
    ++m_cacheGeneration;
    if (m_scanDelivery) {
        m_scanDelivery->handler = {};
    }
    if (m_model != nullptr) {
        m_model->setThumbnailService(nullptr);
    }
}

void AssetBrowserDialog::buildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    // Row 1: search, type chips, source.
    auto* top_row = new QHBoxLayout();
    top_row->setSpacing(6);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Buscar assets…"));
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(220);
    top_row->addWidget(m_search, 1);

    m_chipGroup = new QButtonGroup(this);
    m_chipGroup->setExclusive(true);
    const auto make_chip = [this, top_row](const QString& text, int id) {
        auto* chip = new QToolButton(this);
        chip->setText(text);
        chip->setCheckable(true);
        chip->setAutoRaise(true);
        chip->setToolButtonStyle(Qt::ToolButtonTextOnly);
        m_chipGroup->addButton(chip, id);
        top_row->addWidget(chip);
        return chip;
    };
    m_chipAll = make_chip(tr("All"), 0);
    m_chipMaterial = make_chip(tr("Materials"), 1);
    m_chipTexture = make_chip(tr("Textures"), 2);
    m_chipModel = make_chip(tr("Models"), 3);
    m_chipAll->setChecked(true);

    m_source = new QComboBox(this);
    m_source->addItem(tr("Todas las fuentes"), 0);
    m_source->addItem(tr("Loose"), static_cast<int>(ui::SourceLoose));
    m_source->addItem(tr("VPK"), static_cast<int>(ui::SourcePacked));
    m_source->addItem(tr("Carpeta"), static_cast<int>(ui::SourceFolder));
    top_row->addWidget(m_source);
    layout->addLayout(top_row);

    // Row 2: view toggle and the two scans.
    auto* action_row = new QHBoxLayout();
    action_row->setSpacing(6);
    m_listButton = new QToolButton(this);
    m_listButton->setText(tr("List"));
    m_listButton->setCheckable(true);
    m_listButton->setChecked(true);
    m_listButton->setAutoRaise(true);
    m_listButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_gridButton = new QToolButton(this);
    m_gridButton->setText(tr("Grid"));
    m_gridButton->setCheckable(true);
    m_gridButton->setAutoRaise(true);
    m_gridButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto* view_group = new QButtonGroup(this);
    view_group->setExclusive(true);
    view_group->addButton(m_listButton);
    view_group->addButton(m_gridButton);
    action_row->addWidget(m_listButton);
    action_row->addWidget(m_gridButton);

    action_row->addSpacing(10);
    m_addFolderButton = new QPushButton(tr("Add folder…"), this);
    m_refreshButton = new QPushButton(tr("Refresh"), this);
    m_refreshButton->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    m_refreshButton->setToolTip(tr("Vuelve a escanear el juego y las carpetas (Ctrl+R)"));
    action_row->addWidget(m_addFolderButton);
    action_row->addWidget(m_refreshButton);
    action_row->addStretch(1);

    m_pickButton = new QPushButton(tr("Select"), this);
    m_pickButton->setDefault(true);
    m_pickButton->setVisible(false);
    action_row->addWidget(m_pickButton);
    layout->addLayout(action_row);

    // Splitter: list/grid on the left, preview on the right (3:2).
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_views = new QStackedWidget(m_splitter);

    m_table = new QTableView(m_views);
    m_table->setModel(m_model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setSortingEnabled(true);
    m_table->setWordWrap(false);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setDragEnabled(true);
    m_table->setDragDropMode(QAbstractItemView::DragOnly);
    m_table->setDefaultDropAction(Qt::CopyAction);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(22);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(AssetListModel::NameColumn,
                                                      QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(AssetListModel::TypeColumn,
                                                      QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(AssetListModel::FolderColumn,
                                                      QHeaderView::Interactive);
    m_table->horizontalHeader()->resizeSection(AssetListModel::FolderColumn, 220);
    m_table->horizontalHeader()->setSectionResizeMode(AssetListModel::SourceColumn,
                                                      QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(AssetListModel::SizeColumn,
                                                      QHeaderView::ResizeToContents);
    m_views->addWidget(m_table);

    m_grid = new QListView(m_views);
    m_grid->setModel(m_model);
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setFlow(QListView::LeftToRight);
    m_grid->setWrapping(true);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setUniformItemSizes(true);
    // Must match the delegate, or indexAt() during the visible-range scan
    // stops resolving cells.
    m_grid->setGridSize(AssetGridDelegate::cellSize());
    m_grid->setMovement(QListView::Static);
    m_grid->setSelectionMode(QAbstractItemView::SingleSelection);
    m_grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_grid->setItemDelegate(new AssetGridDelegate(m_thumbnails, m_grid));
    m_grid->setDragEnabled(true);
    m_grid->setDragDropMode(QAbstractItemView::DragOnly);
    m_grid->setDefaultDropAction(Qt::CopyAction);
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    m_views->addWidget(m_grid);

    // Empty state: either nothing indexed yet or the filters exclude everything.
    m_emptyPage = new QWidget(m_views);
    auto* empty_layout = new QVBoxLayout(m_emptyPage);
    empty_layout->setContentsMargins(24, 24, 24, 24);
    empty_layout->setSpacing(8);
    empty_layout->addStretch(1);
    m_emptyTitle = new QLabel(tr("No hay assets"), m_emptyPage);
    QFont empty_font = m_emptyTitle->font();
    empty_font.setPointSizeF(empty_font.pointSizeF() + 3.0);
    empty_font.setBold(true);
    m_emptyTitle->setFont(empty_font);
    m_emptyTitle->setAlignment(Qt::AlignHCenter);
    empty_layout->addWidget(m_emptyTitle);
    m_emptyHint = new QLabel(m_emptyPage);
    m_emptyHint->setWordWrap(true);
    m_emptyHint->setAlignment(Qt::AlignHCenter);
    m_emptyHint->setStyleSheet(QStringLiteral("color: #888888;"));
    empty_layout->addWidget(m_emptyHint);
    auto* empty_buttons = new QHBoxLayout();
    empty_buttons->addStretch(1);
    m_emptyScan = new QPushButton(tr("Scan game"), m_emptyPage);
    m_emptyAdd = new QPushButton(tr("Add folder…"), m_emptyPage);
    m_emptyClear = new QPushButton(tr("Limpiar filtros"), m_emptyPage);
    empty_buttons->addWidget(m_emptyScan);
    empty_buttons->addWidget(m_emptyAdd);
    empty_buttons->addWidget(m_emptyClear);
    empty_buttons->addStretch(1);
    empty_layout->addLayout(empty_buttons);
    empty_layout->addStretch(2);
    m_views->addWidget(m_emptyPage);

    m_splitter->addWidget(m_views);
    m_preview = new AssetPreviewPanel(m_access, m_splitter);
    m_splitter->addWidget(m_preview);
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 2);
    m_splitter->setCollapsible(0, false);
    m_splitter->setSizes({660, 420});
    layout->addWidget(m_splitter, 1);

    m_status = new QLabel(this);
    m_status->setStyleSheet(QStringLiteral("color: #888888; font-size: 8.5pt;"));
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(m_status);
}

void AssetBrowserDialog::connectSignals() {
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(kSearchDebounceMs);
    connect(m_searchTimer, &QTimer::timeout, this, &AssetBrowserDialog::applyFilter);

    m_thumbnailTimer = new QTimer(this);
    m_thumbnailTimer->setSingleShot(true);
    m_thumbnailTimer->setInterval(kThumbnailPumpMs);
    connect(m_thumbnailTimer, &QTimer::timeout, this,
            &AssetBrowserDialog::requestVisibleThumbnails);

    connect(m_search, &QLineEdit::textChanged, this, [this]() {
        m_searchTimer->start();
    });
    connect(m_chipGroup, &QButtonGroup::idClicked, this, [this](int id) {
        if (m_pickKind >= 0) {
            return; // locked while picking
        }
        setKindFilter(id <= 0 ? -1 : id - 1);
    });
    connect(m_source, &QComboBox::currentIndexChanged, this, [this](int) {
        applyFilter();
    });

    connect(m_listButton, &QToolButton::clicked, this, [this]() { setGridMode(false); });
    connect(m_gridButton, &QToolButton::clicked, this, [this]() { setGridMode(true); });
    connect(m_addFolderButton, &QPushButton::clicked, this, &AssetBrowserDialog::chooseFolder);
    connect(m_refreshButton, &QPushButton::clicked, this, &AssetBrowserDialog::refresh);
    connect(m_pickButton, &QPushButton::clicked, this, &AssetBrowserDialog::pickCurrent);

    connect(m_emptyScan, &QPushButton::clicked, this, &AssetBrowserDialog::refresh);
    connect(m_emptyAdd, &QPushButton::clicked, this, &AssetBrowserDialog::chooseFolder);
    connect(m_emptyClear, &QPushButton::clicked, this, [this]() {
        m_search->clear();
        m_source->setCurrentIndex(0);
        showAll();
    });

    connect(m_model, &AssetListModel::filterResultCountChanged, this, [this](int) {
        updateViewStack();
        updateStatus();
    });

    const auto connect_selection = [this](QAbstractItemView* view) {
        connect(view->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this](const QModelIndex& current, const QModelIndex&) {
                    onCurrentChanged(current);
                });
        connect(view, &QAbstractItemView::activated, this, &AssetBrowserDialog::activateCurrent);
        connect(view, &QWidget::customContextMenuRequested, this,
                [this, view](const QPoint& pos) { showContextMenu(view, pos); });
        connect(view->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
            scheduleThumbnailPump();
        });
    };
    connect_selection(m_table);
    connect_selection(m_grid);

    connect(m_preview, &AssetPreviewPanel::activateRequested, this,
            &AssetBrowserDialog::activatePath);

    connect(m_thumbnails, &AssetThumbnailService::pendingChanged, this, [this](int) {
        updateStatus();
        if (m_thumbnails->pendingCount() == 0) {
            scheduleThumbnailPump();
        }
    });
}

void AssetBrowserDialog::setEngine(bridge::EngineView* engine) {
    if (m_engine == engine) {
        m_access->setEngine(engine);
        return;
    }
    m_engine = engine;
    m_access->setEngine(engine);
    m_thumbnails->invalidate();
    updateRefreshState();
    updateStatus();
}

void AssetBrowserDialog::setGame(const std::filesystem::path& gameDir,
                                 const std::filesystem::path& binDir) {
    const QString key = game_key_for(gameDir);
    const bool same = key == m_gameKey && m_loadedCacheKey == key;
    const bool same_bin = m_binDir == binDir;
    m_gameDir = gameDir;
    m_binDir = binDir;
    m_gameKey = key;
    updateSearchRoots();
    if (same) {
        if (!same_bin) {
            updateModelContext();
        }
        return;
    }

    // A different game: drop in-flight work by moving both generations.
    ++m_scanGeneration;
    ++m_cacheGeneration;
    m_scanRunning = false;
    m_cacheLoading = false;
    m_model->setIndex(nullptr);
    m_index.reset();
    m_model->refresh();
    m_currentPath.clear();
    m_preview->clear();
    m_counts = {};
    m_generated = 0;
    m_stale = false;
    m_truncated = false;
    m_lastError.clear();
    m_loadedCacheKey.clear();
    m_pack.reset();

    m_thumbnails->reset();
    m_thumbnails->setGame(nullptr, 0);
    updateModelContext();

    updateChips();
    updateViewStack();
    updateStatus();
    updateRefreshState();

    if (!key.isEmpty()) {
        loadCacheAsync();
    }
}

void AssetBrowserDialog::beginPick(int kind) {
    if (kind < 0 || kind > static_cast<int>(ui::AssetKind::Model)) {
        return;
    }
    m_pickKind = kind;
    m_pickButton->setVisible(true);
    m_pickButton->setEnabled(false);
    m_chipAll->setEnabled(false);
    m_chipMaterial->setEnabled(kind == static_cast<int>(ui::AssetKind::Material));
    m_chipTexture->setEnabled(kind == static_cast<int>(ui::AssetKind::Texture));
    m_chipModel->setEnabled(kind == static_cast<int>(ui::AssetKind::Model));
    m_preview->setPickMode(true);
    setKindFilter(kind);
    updateStatus();
}

void AssetBrowserDialog::endPick() {
    if (m_pickKind < 0) {
        return;
    }
    m_pickKind = -1;
    m_pickButton->setVisible(false);
    m_chipAll->setEnabled(true);
    m_chipMaterial->setEnabled(true);
    m_chipTexture->setEnabled(true);
    m_chipModel->setEnabled(true);
    m_preview->setPickMode(false);
    updateStatus();
}

void AssetBrowserDialog::showAll() {
    endPick();
    m_source->setCurrentIndex(0);
    setKindFilter(-1);
}

void AssetBrowserDialog::showKind(int kind) {
    if (kind < 0 || kind > static_cast<int>(ui::AssetKind::Model)) {
        return;
    }
    endPick();
    setKindFilter(kind);
}

void AssetBrowserDialog::setKindFilter(int kind) {
    if (kind < -1 || kind > static_cast<int>(ui::AssetKind::Model)) {
        return;
    }
    m_kind = kind;
    QToolButton* chip = m_chipAll;
    if (kind == static_cast<int>(ui::AssetKind::Material)) {
        chip = m_chipMaterial;
    } else if (kind == static_cast<int>(ui::AssetKind::Texture)) {
        chip = m_chipTexture;
    } else if (kind == static_cast<int>(ui::AssetKind::Model)) {
        chip = m_chipModel;
    }
    chip->setChecked(true);
    applyFilter();
}

void AssetBrowserDialog::setGridMode(bool grid) {
    m_listButton->setChecked(!grid);
    m_gridButton->setChecked(grid);
    if (m_gridMode == grid) {
        return;
    }
    m_gridMode = grid;
    updateViewStack();
    if (!m_currentPath.isEmpty()) {
        selectPath(m_currentPath);
    }
    scheduleThumbnailPump();
}

void AssetBrowserDialog::applyFilter() {
    if (m_model == nullptr || m_source == nullptr) {
        return;
    }
    ui::AssetFilter filter;
    if (m_kind >= 0) {
        filter.kinds = 1u << static_cast<unsigned>(m_kind);
    }
    filter.sources = static_cast<std::uint8_t>(m_source->currentData().toInt());

    const QString text = m_search->text().simplified();
    if (!text.isEmpty()) {
        const QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        filter.tokens.reserve(static_cast<std::size_t>(words.size()));
        for (const QString& word : words) {
            // AssetIndex matches tokens against path_lower, so normalize here;
            // otherwise "Brick" would never match "materials/brick/...".
            filter.tokens.push_back(word.toLower().toStdString());
        }
    }

    m_model->setFilter(filter);
    m_model->refresh();
    if (!m_currentPath.isEmpty()) {
        // Keep the row selected across a filter change when it is still
        // visible; if the filter excludes it, selectPath clears the preview.
        selectPath(m_currentPath);
    }
    updateViewStack();
    updateStatus();
    scheduleThumbnailPump();
}

void AssetBrowserDialog::updateChips() {
    const QLocale locale;
    const auto text_for = [&locale](const QString& label, int count) {
        if (count <= 0) {
            return label;
        }
        return QStringLiteral("%1 %2").arg(
            label, locale.toString(static_cast<qlonglong>(count)));
    };
    m_chipAll->setText(text_for(tr("All"), m_counts[0]));
    m_chipMaterial->setText(text_for(tr("Materials"), m_counts[1]));
    m_chipTexture->setText(text_for(tr("Textures"), m_counts[2]));
    m_chipModel->setText(text_for(tr("Models"), m_counts[3]));
}

void AssetBrowserDialog::updateViewStack() {
    const bool has_index = m_index != nullptr && !m_index->empty();
    const bool has_rows = m_model->visibleRowCount() > 0;
    if (has_index && has_rows) {
        m_views->setCurrentWidget(m_gridMode ? static_cast<QWidget*>(m_grid)
                                             : static_cast<QWidget*>(m_table));
        return;
    }

    m_views->setCurrentWidget(m_emptyPage);
    if (!has_index) {
        m_emptyTitle->setText(tr("No hay assets"));
        m_emptyHint->setText(tr("Escanea el juego para listar materiales, texturas y modelos, "
                                "o añade una carpeta con tus propios assets."));
        m_emptyScan->setVisible(true);
        m_emptyAdd->setVisible(true);
        m_emptyClear->setVisible(false);
    } else {
        m_emptyTitle->setText(tr("Ningún asset coincide"));
        m_emptyHint->setText(tr("Cambia el texto de búsqueda o los filtros para ver más "
                                "resultados."));
        m_emptyScan->setVisible(false);
        m_emptyAdd->setVisible(false);
        m_emptyClear->setVisible(true);
    }
}

void AssetBrowserDialog::updateStatus() {
    QStringList parts;
    if (m_scanRunning) {
        parts.append(tr("Escaneando…"));
    } else if (m_cacheLoading) {
        parts.append(tr("Cargando caché…"));
    }

    const QLocale locale;
    const int total = m_index != nullptr ? static_cast<int>(m_index->size()) : 0;
    const int visible = m_model != nullptr ? m_model->visibleRowCount() : 0;
    parts.append(tr("%1 assets").arg(locale.toString(static_cast<qlonglong>(total))));
    if (visible != total) {
        parts.append(tr("%1 visibles").arg(locale.toString(static_cast<qlonglong>(visible))));
    }
    parts.append(tr("Caché: %1").arg(format_age(m_generated)));
    const int pending = m_thumbnails->pendingCount();
    if (pending > 0) {
        parts.append(tr("%1 miniaturas en cola").arg(pending));
    }
    if (m_stale) {
        parts.append(tr("Caché antigua: pulsa Refresh"));
    }
    if (m_truncated) {
        parts.append(tr("Lista truncada por seguridad"));
    }
    if (!m_lastError.isEmpty()) {
        parts.append(tr("Error: %1").arg(m_lastError));
    }

    const QString text = parts.join(QStringLiteral(" · "));
    m_status->setText(text);
    m_status->setToolTip(text);
}

void AssetBrowserDialog::updateRefreshState() {
    const bool can_scan = !m_scanRunning && !m_cacheLoading &&
                          (m_engine != nullptr || !m_folders.isEmpty());
    if (m_refreshButton != nullptr) {
        m_refreshButton->setEnabled(can_scan);
    }
    if (m_emptyScan != nullptr) {
        m_emptyScan->setEnabled(can_scan);
    }
    if (m_addFolderButton != nullptr) {
        m_addFolderButton->setEnabled(!m_scanRunning);
    }
    if (m_emptyAdd != nullptr) {
        m_emptyAdd->setEnabled(!m_scanRunning);
    }
    if (m_listButton != nullptr) {
        m_listButton->setEnabled(!m_scanRunning || m_model->visibleRowCount() > 0);
    }
}

std::filesystem::path AssetBrowserDialog::cachePackFor(const QString& gameKey) const {
    if (m_appDataDir.empty() || gameKey.isEmpty()) {
        return {};
    }
    return m_appDataDir / "cache" / gameKey.toStdWString() / "cache.pack";
}

std::filesystem::path AssetBrowserDialog::legacyCacheFileFor(const QString& gameKey) const {
    if (m_appDataDir.empty() || gameKey.isEmpty()) {
        return {};
    }
    return m_appDataDir / "cache" / gameKey.toStdWString() / "assets-v1.tsv";
}

std::filesystem::path AssetBrowserDialog::legacyThumbnailDirFor(const QString& gameKey) const {
    if (m_appDataDir.empty() || gameKey.isEmpty()) {
        return {};
    }
    return m_appDataDir / "thumbnails" / gameKey.toStdWString();
}

void AssetBrowserDialog::updateModelContext() {}

void AssetBrowserDialog::updateSearchRoots() {
    const std::vector<std::filesystem::path> roots = search_roots(m_gameDir, m_folders);
    m_thumbnails->setSearchRoots(roots);
    m_preview->setSearchRoots(roots);
}

void AssetBrowserDialog::loadCacheAsync() {
    if (m_cacheLoading || m_gameKey.isEmpty()) {
        return;
    }
    const std::filesystem::path pack_file = cachePackFor(m_gameKey);
    const std::filesystem::path legacy_index = legacyCacheFileFor(m_gameKey);
    const std::filesystem::path legacy_thumbs = legacyThumbnailDirFor(m_gameKey);
    if (pack_file.empty()) {
        return;
    }

    const int generation = ++m_cacheGeneration;
    m_cacheLoading = true;
    m_loadedCacheKey = m_gameKey;
    updateStatus();
    updateRefreshState();

    const std::shared_ptr<ScanDelivery> delivery = m_scanDelivery;
    const std::string game_dir_string =
        absolute_game_dir_string(m_gameDir).toStdString();
    const std::filesystem::path game_dir = m_gameDir;

    std::thread([delivery, generation, pack_file, legacy_index, legacy_thumbs, game_dir_string, game_dir]() {
        AssetScanOutcome outcome = load_cache_worker(pack_file, legacy_index, legacy_thumbs, game_dir_string, game_dir);
        outcome.generation = generation;
        QMetaObject::invokeMethod(
            delivery.get(),
            [delivery, outcome]() {
                if (delivery->handler) {
                    delivery->handler(outcome);
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void AssetBrowserDialog::refresh() {
    if (m_scanRunning) {
        return;
    }
    startScan(nullptr);
}

void AssetBrowserDialog::startScan(const std::shared_ptr<ui::AssetIndex>& base) {
    if (m_scanRunning) {
        return;
    }
    const bool engine_ready = m_engine != nullptr && m_engine->initialized();
    if (!engine_ready && base == nullptr && m_folders.isEmpty()) {
        m_status->setText(tr("Conecta un juego o añade una carpeta para poder escanear."));
        return;
    }

    // A scan supersedes a cache load that is still in flight.
    ++m_cacheGeneration;
    m_cacheLoading = false;

    const int generation = ++m_scanGeneration;
    m_scanRunning = true;
    updateRefreshState();
    updateStatus();

    const std::shared_ptr<ScanDelivery> delivery = m_scanDelivery;
    const std::shared_ptr<EngineAccess> access = m_access;
    bridge::EngineView* engine = m_engine;
    const std::string game_dir_string =
        m_gameDir.empty() ? std::string() : absolute_game_dir_string(m_gameDir).toStdString();
    const QStringList folders = m_folders;
    const std::filesystem::path pack_file =
        m_gameKey.isEmpty() ? std::filesystem::path() : cachePackFor(m_gameKey);
    const std::filesystem::path legacy_index =
        m_gameKey.isEmpty() ? std::filesystem::path() : legacyCacheFileFor(m_gameKey);
    const std::filesystem::path legacy_thumbs =
        m_gameKey.isEmpty() ? std::filesystem::path() : legacyThumbnailDirFor(m_gameKey);
    const std::shared_ptr<ui::AssetPack> pack = m_pack;
    const std::shared_ptr<ui::AssetIndex> base_index = base;

    std::thread([delivery, generation, access, engine, engine_ready, game_dir_string,
                 folders, pack_file, legacy_index, legacy_thumbs, pack, base_index]() {
        AssetScanOutcome outcome = scan_worker(engine, engine_ready, access,
                                               game_dir_string, folders, pack_file,
                                               legacy_index, legacy_thumbs, pack, base_index);
        outcome.generation = generation;
        QMetaObject::invokeMethod(
            delivery.get(),
            [delivery, outcome]() {
                if (delivery->handler) {
                    delivery->handler(outcome);
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void AssetBrowserDialog::applyScanOutcome(const AssetScanOutcome& outcome) {
    const bool from_cache = outcome.loadedFromCache;
    if (from_cache) {
        if (outcome.generation != m_cacheGeneration) {
            return;
        }
        m_cacheLoading = false;
    } else {
        if (outcome.generation != m_scanGeneration) {
            return;
        }
        m_scanRunning = false;
    }
    updateRefreshState();

    if (!outcome.index) {
        // Nothing usable came back (no cache yet, or the scan failed before
        // producing anything). Keep the current view; only scans report errors.
        if (!from_cache && !outcome.error.isEmpty()) {
            m_lastError = outcome.error;
        }
        updateViewStack();
        updateStatus();
        return;
    }

    // A fresh scan may have picked up edited files, so the preview is reloaded
    // for the restored selection. A cache load leaves it untouched.
    applyIndex(outcome.index, outcome.generated, outcome.stale, outcome.truncated,
               outcome.counts, from_cache ? QString() : outcome.error, !from_cache);
}

void AssetBrowserDialog::applyIndex(const std::shared_ptr<ui::AssetIndex>& index,
                                    std::int64_t generated, bool stale, bool truncated,
                                    const std::array<int, 4>& counts, const QString& error,
                                    bool reloadPreview) {
    const QString keep = m_currentPath;
    m_index = index;
    m_generated = generated;
    if (reloadPreview) {
        // A scan may have fixed or changed files: drop queued work and let a
        // previously failed thumbnail be attempted again.
        m_thumbnails->invalidate();
    }
    // Packed assets key their BMP cache by the index's generation stamp: a
    // rescan (or a cache load) selects the matching set of thumbnails.
    m_thumbnails->setGame(m_pack, generated);
    m_stale = stale;
    m_truncated = truncated;
    m_counts = counts;
    m_lastError = error;

    m_model->setIndex(m_index.get());
    m_model->refresh();
    updateChips();
    updateViewStack();
    updateStatus();
    updateRefreshState();
    scheduleThumbnailPump();

    if (!keep.isEmpty()) {
        if (reloadPreview) {
            m_preview->clear();
        }
        selectPath(keep);
        if (reloadPreview) {
            onCurrentChanged(currentIndex());
        }
    } else {
        clearPreview();
    }
}

QModelIndex AssetBrowserDialog::currentIndex() const {
    if (m_gridMode) {
        return m_grid->currentIndex();
    }
    return m_table->currentIndex();
}

void AssetBrowserDialog::selectPath(const QString& virtualPath) {
    const QModelIndex index = m_model->indexForPath(virtualPath);
    if (!index.isValid()) {
        clearPreview();
        return;
    }
    QAbstractItemView* view = m_gridMode ? static_cast<QAbstractItemView*>(m_grid)
                                         : static_cast<QAbstractItemView*>(m_table);
    view->setCurrentIndex(index);
    if (view == m_table) {
        m_table->selectRow(index.row());
    }
    view->scrollTo(index);
}

void AssetBrowserDialog::onCurrentChanged(const QModelIndex& current) {
    const ui::AssetEntry* entry = m_model->entryAt(current);
    if (entry == nullptr) {
        return;
    }
    m_currentPath = QString::fromStdString(entry->virtual_path);
    const int kind = static_cast<int>(entry->kind);
    m_preview->showAsset(m_currentPath, kind, resolved_disk_path(*entry, m_gameDir),
                         static_cast<int>(entry->sources), entry->size);
    m_pickButton->setEnabled(m_pickKind < 0 || kind == m_pickKind);
}

void AssetBrowserDialog::clearPreview() {
    m_currentPath.clear();
    m_preview->clear();
    m_pickButton->setEnabled(false);
}

void AssetBrowserDialog::activateCurrent() {
    const QModelIndex index = currentIndex();
    const ui::AssetEntry* entry = m_model->entryAt(index);
    if (entry == nullptr) {
        return;
    }
    const QString path = QString::fromStdString(entry->virtual_path);
    const int kind = static_cast<int>(entry->kind);
    if (m_pickKind >= 0) {
        if (kind != m_pickKind) {
            m_status->setText(tr("Selecciona un asset de tipo %1.").arg(kind_label(m_pickKind)));
            return;
        }
        emit assetPicked(path, kind);
        return;
    }
    emit assetActivated(path, kind);
}

void AssetBrowserDialog::activatePath(const QString& virtualPath, int kind) {
    if (m_pickKind >= 0) {
        if (kind == m_pickKind) {
            m_pickButton->setEnabled(true);
        }
        return;
    }
    emit assetActivated(virtualPath, kind);
}

void AssetBrowserDialog::pickCurrent() {
    const QModelIndex index = currentIndex();
    const ui::AssetEntry* entry = m_model->entryAt(index);
    if (entry == nullptr || m_pickKind < 0) {
        return;
    }
    const int kind = static_cast<int>(entry->kind);
    if (kind != m_pickKind) {
        return;
    }
    emit assetPicked(QString::fromStdString(entry->virtual_path), kind);
}

void AssetBrowserDialog::chooseFolder() {
    if (m_scanRunning) {
        return; // the button is disabled; the context menu can still get here
    }
    const QString start = m_folders.isEmpty() ? QString() : m_folders.last();
    const QString dir =
        QFileDialog::getExistingDirectory(this, tr("Añadir carpeta de assets"), start);
    if (dir.isEmpty()) {
        return;
    }
    const QString clean = QDir::cleanPath(dir);
    for (const QString& folder : m_folders) {
        if (folder.compare(clean, Qt::CaseInsensitive) == 0) {
            m_status->setText(tr("La carpeta ya está en la lista."));
            return;
        }
    }

    m_folders.append(clean);
    updateSearchRoots();
    emit foldersChanged(m_folders);

    if (m_engine != nullptr) {
        startScan(nullptr); // a full rebuild keeps the cached VPK rows in sync
    } else {
        // No engine: add the folder on top of whatever is already indexed
        // (usually the game cache). The cache is not rewritten in this mode.
        startScan(m_index);
    }
}

void AssetBrowserDialog::showContextMenu(QAbstractItemView* view, const QPoint& pos) {
    const QModelIndex index = view->indexAt(pos);

    // Copy what the actions need before exec(): the menu runs a nested event
    // loop, and a scan finishing in the meantime resets the model.
    QString path;
    QString disk;
    int kind = -1;
    if (const ui::AssetEntry* entry = m_model->entryAt(index)) {
        path = QString::fromStdString(entry->virtual_path);
        disk = resolved_disk_path(*entry, m_gameDir);
        kind = static_cast<int>(entry->kind);
        view->setCurrentIndex(index);
        if (view == m_table) {
            m_table->selectRow(index.row());
        }
    }

    QMenu menu(this);
    QAction* open_action = nullptr;
    QAction* copy_game = nullptr;
    QAction* copy_disk = nullptr;
    QAction* reveal = nullptr;
    if (!path.isEmpty()) {
        open_action = menu.addAction(tr("Abrir"));
        copy_game = menu.addAction(tr("Copiar ruta de juego"));
        if (!disk.isEmpty()) {
            copy_disk = menu.addAction(tr("Copiar ruta de disco"));
            reveal = menu.addAction(tr("Revelar en Explorer"));
        }
        menu.addSeparator();
    }
    QAction* add_folder = menu.addAction(tr("Añadir carpeta…"));
    QAction* refresh_action = menu.addAction(tr("Refresh"));

    QAction* chosen = menu.exec(view->viewport()->mapToGlobal(pos));
    if (chosen == nullptr) {
        return;
    }
    if (chosen == open_action) {
        if (!path.isEmpty()) {
            if (m_pickKind >= 0) {
                if (kind == m_pickKind) {
                    emit assetPicked(path, kind);
                }
            } else {
                emit assetActivated(path, kind);
            }
        }
    } else if (chosen == copy_game) {
        copyToClipboard(path);
    } else if (chosen == copy_disk) {
        copyToClipboard(disk);
    } else if (chosen == reveal) {
        revealInExplorer(disk);
    } else if (chosen == add_folder) {
        chooseFolder();
    } else if (chosen == refresh_action) {
        refresh();
    }
}

void AssetBrowserDialog::revealInExplorer(const QString& diskPath) {
    if (diskPath.isEmpty()) {
        return;
    }
#ifdef Q_OS_WIN
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            {QStringLiteral("/select,"), QDir::toNativeSeparators(diskPath)});
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(diskPath).absolutePath()));
#endif
}

void AssetBrowserDialog::copyToClipboard(const QString& text) {
    if (text.isEmpty()) {
        return;
    }
    QApplication::clipboard()->setText(text);
    m_status->setText(tr("Copiado: %1").arg(text));
    m_status->setToolTip(text);
}

void AssetBrowserDialog::focusSearch() {
    m_search->setFocus(Qt::ShortcutFocusReason);
    m_search->selectAll();
}

void AssetBrowserDialog::scheduleThumbnailPump() {
    if (m_pumpingThumbnails || m_thumbnailTimer == nullptr) {
        return;
    }
    m_thumbnailTimer->start();
}

void AssetBrowserDialog::requestVisibleThumbnails() {
    if (m_index == nullptr || m_index->empty() || m_views->currentWidget() == m_emptyPage) {
        return;
    }

    m_pumpingThumbnails = true;
    int requested = 0;
    const auto request_index = [this, &requested](const QModelIndex& index) {
        if (!index.isValid() || requested >= kVisibleThumbnailCap) {
            return;
        }
        const ui::AssetEntry* entry = m_model->entryAt(index);
        if (entry == nullptr) {
            return;
        }
        const QString path = QString::fromStdString(entry->virtual_path);
        if (m_thumbnails->contains(path)) {
            return;
        }
        m_thumbnails->request(path, disk_path_of(*entry));
        ++requested;
    };

    if (m_gridMode) {
        const QSize cell = AssetGridDelegate::cellSize();
        const QRect viewport = m_grid->viewport()->rect();
        for (int y = viewport.top(); y <= viewport.bottom() && requested < kVisibleThumbnailCap;
             y += cell.height()) {
            for (int x = viewport.left(); x <= viewport.right() && requested < kVisibleThumbnailCap;
                 x += cell.width()) {
                request_index(m_grid->indexAt(QPoint(x + 2, y + 2)));
            }
        }
    } else {
        const int first = m_table->rowAt(2);
        const int last = m_table->rowAt(std::max(2, m_table->viewport()->height() - 3));
        if (first >= 0) {
            const int end = last >= first ? last : first;
            for (int row = first; row <= end && requested < kVisibleThumbnailCap; ++row) {
                request_index(m_model->index(row, AssetListModel::NameColumn));
            }
        }
    }
    m_pumpingThumbnails = false;
}

void AssetBrowserDialog::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::Copy)) {
        if (!m_currentPath.isEmpty()) {
            copyToClipboard(m_currentPath);
            event->accept();
            return;
        }
    }
    if (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_L) {
        focusSearch();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && !m_search->text().isEmpty()) {
        m_search->clear();
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void AssetBrowserDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    scheduleThumbnailPump();
}

void AssetBrowserDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (m_firstShow) {
        m_firstShow = false;
        m_search->setFocus(Qt::ActiveWindowFocusReason);
    }
    scheduleThumbnailPump();
}

void AssetBrowserDialog::setFolders(const QStringList& folders) {
    if (m_folders == folders) {
        return;
    }
    m_folders = folders;
    updateSearchRoots();
    updateRefreshState();
}

AssetBrowserState AssetBrowserDialog::state() const {
    AssetBrowserState result;
    result.kind = m_kind;
    result.source = m_source->currentIndex();
    result.grid = m_gridMode;
    result.sortColumn = m_model->sortColumn();
    result.sortAsc = m_model->sortOrder() == Qt::AscendingOrder;
    result.folders = m_folders;
    result.geometryBase64 = QString::fromLatin1(saveGeometry().toBase64());
    return result;
}

void AssetBrowserDialog::setState(const AssetBrowserState& state) {
    endPick();
    setFolders(state.folders);

    int column = state.sortColumn;
    if (column < 0 || column >= AssetListModel::ColumnCount) {
        column = AssetListModel::NameColumn;
    }
    const Qt::SortOrder order = state.sortAsc ? Qt::AscendingOrder : Qt::DescendingOrder;
    m_model->setSort(column, order);
    {
        // Updating the indicator would otherwise call QTableView::sortByColumn
        // and sort the same 150k rows twice while opening.
        const QSignalBlocker blocker(m_table->horizontalHeader());
        m_table->horizontalHeader()->setSortIndicator(column, order);
    }

    m_kind = (state.kind >= 0 && state.kind <= static_cast<int>(ui::AssetKind::Model))
                 ? state.kind
                 : -1;
    QToolButton* chip = m_chipAll;
    if (m_kind == static_cast<int>(ui::AssetKind::Material)) {
        chip = m_chipMaterial;
    } else if (m_kind == static_cast<int>(ui::AssetKind::Texture)) {
        chip = m_chipTexture;
    } else if (m_kind == static_cast<int>(ui::AssetKind::Model)) {
        chip = m_chipModel;
    }
    chip->setChecked(true);

    const int source = std::clamp(state.source, 0, m_source->count() - 1);
    if (m_source->currentIndex() != source) {
        m_source->setCurrentIndex(source); // applies the filter through the signal
    }

    m_listButton->setChecked(!state.grid);
    m_gridButton->setChecked(state.grid);
    m_gridMode = state.grid;
    if (!state.geometryBase64.isEmpty()) {
        restoreGeometry(QByteArray::fromBase64(state.geometryBase64.toLatin1()));
    }

    applyFilter();
}

} // namespace qt
} // namespace ui
