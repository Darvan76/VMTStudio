#include "model_browser_dialog.h"
#include "bridge/engine_view.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLocale>
#include <QMenu>
#include <QShowEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <string>
#include <thread>
#include <utility>

namespace ui {
namespace qt {

namespace {

// The tree never creates more than this many rows; live filtering narrows the
// rest down. The full (deduplicated) result stays in ModelBrowserDialog's
// m_entries.
constexpr int kMaxDisplayRows = 5000;

std::string to_lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// Sorts by path and drops duplicates, preferring the loose copy when the same
// path is reachable both on disk and inside a VPK: the tree lists every model
// once.
void sort_and_dedup(std::vector<bridge::ModelEntry>& entries) {
    std::stable_sort(entries.begin(), entries.end(),
                     [](const bridge::ModelEntry& a, const bridge::ModelEntry& b) {
                         if (a.name != b.name) {
                             return a.name < b.name;
                         }
                         return a.loose && !b.loose; // the loose copy wins the dedup
                     });
    entries.erase(std::unique(entries.begin(), entries.end(),
                              [](const bridge::ModelEntry& a, const bridge::ModelEntry& b) {
                                  return to_lower(a.name) == to_lower(b.name);
                              }),
                  entries.end());
}

// Parent folder of a game-relative path ("models/props_c17/oildrum001.mdl" ->
// "models/props_c17"); empty when there is none.
QString folder_of(const QString& game_path) {
    const qsizetype slash = game_path.lastIndexOf(QLatin1Char('/'));
    return slash > 0 ? game_path.left(slash) : QString();
}

} // namespace

ModelBrowserDialog::ModelBrowserDialog(bridge::EngineView* engine, QWidget* parent)
    : QDialog(parent), m_engine(engine) {
    setWindowTitle(tr("Model Browser"));
    resize(900, 600);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    // Top bar: search, scan, source filter and the scan status.
    auto* topBar = new QHBoxLayout();
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("Filter models..."));
    m_searchEdit->setClearButtonEnabled(true);
    topBar->addWidget(m_searchEdit, 1);

    m_scanBtn = new QPushButton(tr("Scan"), this);
    topBar->addWidget(m_scanBtn);

    m_sourceCombo = new QComboBox(this);
    m_sourceCombo->addItem(tr("All"));
    m_sourceCombo->addItem(tr("Loose only"));
    m_sourceCombo->addItem(tr("VPK only"));
    topBar->addWidget(m_sourceCombo);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet("color: #888888; font-size: 8.5pt;");
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // A long error must not stretch the dialog: let the label be clipped (the
    // full text stays available through the tooltip).
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    topBar->addWidget(m_statusLabel, 2);
    mainLayout->addLayout(topBar);

    // Model list. Sorting is switched off while rows are inserted (see
    // populateTree) and re-enabled afterwards so the header stays clickable.
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderLabels({tr("Model"), tr("Source"), tr("Folder")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(0, Qt::AscendingOrder);
    m_tree->header()->resizeSection(0, 460);
    m_tree->header()->resizeSection(1, 70);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    mainLayout->addWidget(m_tree, 1);

    // Bottom bar: count and actions.
    auto* bottomBar = new QHBoxLayout();
    m_countLabel = new QLabel(tr("No scan yet."), this);
    bottomBar->addWidget(m_countLabel);
    bottomBar->addStretch(1);

    m_loadBtn = new QPushButton(tr("Load Model"), this);
    m_loadBtn->setDefault(true);
    m_loadBtn->setEnabled(false);
    bottomBar->addWidget(m_loadBtn);

    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    bottomBar->addWidget(m_cancelBtn);
    mainLayout->addLayout(bottomBar);

    connect(m_searchEdit, &QLineEdit::textChanged, this, &ModelBrowserDialog::onSearchTextChanged);
    connect(m_scanBtn, &QPushButton::clicked, this, &ModelBrowserDialog::onScanClicked);
    connect(m_sourceCombo, &QComboBox::currentIndexChanged, this,
            &ModelBrowserDialog::onSourceFilterChanged);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            &ModelBrowserDialog::onItemSelectionChanged);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this,
            &ModelBrowserDialog::onItemDoubleClicked);
    connect(m_tree, &QWidget::customContextMenuRequested, this,
            &ModelBrowserDialog::onTreeContextMenu);
    connect(m_loadBtn, &QPushButton::clicked, this, &ModelBrowserDialog::onLoadClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);

    if (!m_engine) {
        m_scanBtn->setEnabled(false);
        setStatus(tr("Connect to a game first (Game > Connection Settings...)"));
    }
}

ModelBrowserDialog::~ModelBrowserDialog() {
    // A scan still in flight becomes a no-op when it completes.
    *m_alive = false;
}

void ModelBrowserDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (m_firstShow) {
        m_firstShow = false;
        startScan();
    }
}

void ModelBrowserDialog::startScan() {
    if (!m_engine || m_scanRunning) {
        return;
    }
    m_scanRunning = true;
    m_scanBtn->setEnabled(false);
    setStatus(tr("Scanning the game file system (VPK included)..."));

    auto alive = m_alive;
    bridge::EngineView* engine = m_engine;

    std::thread([this, alive, engine]() {
        auto listRes = engine->list_models("models/");

        // The dialog may be gone by now; after this check the worker no longer
        // touches anything it does not own.
        if (!*alive) {
            return;
        }

        if (!listRes) {
            const std::string message = listRes.error().message;
            QMetaObject::invokeMethod(this, [this, alive, message]() {
                if (!*alive) return;
                m_scanRunning = false;
                m_scanBtn->setText(tr("Rescan"));
                m_scanBtn->setEnabled(m_engine != nullptr);
                m_summary = tr("Scan failed: %1").arg(QString::fromStdString(message));
                setStatus(m_summary);
            }, Qt::QueuedConnection);
            return;
        }

        // Sorting and deduplication happen off the UI thread: the tree only
        // ever sees the final list.
        auto entries = std::make_shared<std::vector<bridge::ModelEntry>>(
            std::move(listRes.value().entries));
        const bool truncated = listRes.value().truncated;
        sort_and_dedup(*entries);

        QMetaObject::invokeMethod(this, [this, alive, entries, truncated]() {
            if (!*alive) return;
            m_scanRunning = false;
            m_scanned = true;
            m_entries = std::move(*entries);
            m_scanBtn->setText(tr("Rescan"));
            m_scanBtn->setEnabled(m_engine != nullptr);
            m_summary = truncated
                ? tr("Found %1 models (the scan stopped at its safety cap).")
                      .arg(QLocale().toString(static_cast<qlonglong>(m_entries.size())))
                : tr("Found %1 models.")
                      .arg(QLocale().toString(static_cast<qlonglong>(m_entries.size())));
            applyFilter();
        }, Qt::QueuedConnection);
    }).detach();
}

void ModelBrowserDialog::onScanClicked() {
    startScan();
}

void ModelBrowserDialog::onSearchTextChanged() {
    applyFilter();
}

void ModelBrowserDialog::onSourceFilterChanged() {
    applyFilter();
}

void ModelBrowserDialog::applyFilter() {
    const QString needle = m_searchEdit->text().trimmed();
    const int sourceFilter = m_sourceCombo->currentIndex(); // 0 all, 1 loose, 2 VPK
    const bool filtered = !needle.isEmpty() || sourceFilter != 0;

    std::vector<const bridge::ModelEntry*> matches;
    matches.reserve(m_entries.size());
    for (const bridge::ModelEntry& entry : m_entries) {
        if (sourceFilter == 1 && !entry.loose) {
            continue; // VPK copy, hidden by "Loose only"
        }
        if (sourceFilter == 2 && entry.loose) {
            continue; // loose copy, hidden by "VPK only"
        }
        if (!needle.isEmpty() &&
            !QString::fromStdString(entry.name).contains(needle, Qt::CaseInsensitive)) {
            continue;
        }
        matches.push_back(&entry);
    }

    populateTree(matches);
    updateSummary(matches.size(), filtered);
}

void ModelBrowserDialog::populateTree(const std::vector<const bridge::ModelEntry*>& matches) {
    m_tree->setSortingEnabled(false);
    m_tree->clear();

    const std::size_t shown =
        std::min(matches.size(), static_cast<std::size_t>(kMaxDisplayRows));
    for (std::size_t i = 0; i < shown; ++i) {
        const bridge::ModelEntry& entry = *matches[i];
        const QString path = QString::fromStdString(entry.name);
        auto* item = new QTreeWidgetItem(m_tree, {
            path,
            entry.loose ? tr("Disk") : tr("VPK"),
            folder_of(path)
        });
        // The sort order changes while the tree is sorted, so the row carries
        // its own data instead of relying on a positional index.
        item->setData(0, Qt::UserRole, path);
        item->setData(0, Qt::UserRole + 1, entry.loose);
        item->setToolTip(0, path);
    }

    m_tree->setSortingEnabled(true); // re-sorts by the current header indicator
    m_loadBtn->setEnabled(false);    // clear() dropped the previous selection
}

void ModelBrowserDialog::updateSummary(std::size_t matched, bool filtered) {
    if (!m_scanned) {
        return;
    }

    const qlonglong total = static_cast<qlonglong>(m_entries.size());
    const qlonglong matchedCount = static_cast<qlonglong>(matched);

    if (filtered) {
        m_countLabel->setText(tr("%1 of %2 shown")
                                  .arg(QLocale().toString(matchedCount),
                                       QLocale().toString(total)));
    } else {
        m_countLabel->setText(tr("%1 models").arg(QLocale().toString(total)));
    }

    if (matched > static_cast<std::size_t>(kMaxDisplayRows)) {
        setStatus(tr("Showing the first %1 - refine the search.").arg(kMaxDisplayRows));
    } else if (!m_summary.isEmpty()) {
        setStatus(m_summary);
    }
}

void ModelBrowserDialog::onItemSelectionChanged() {
    m_loadBtn->setEnabled(!m_tree->selectedItems().isEmpty());
}

void ModelBrowserDialog::onItemDoubleClicked(QTreeWidgetItem* item, int column) {
    Q_UNUSED(column);
    if (!item) return;
    loadItem(item);
}

void ModelBrowserDialog::onLoadClicked() {
    const QList<QTreeWidgetItem*> selected = m_tree->selectedItems();
    if (selected.isEmpty()) return;
    loadItem(selected.first());
}

void ModelBrowserDialog::onTreeContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_tree->itemAt(pos);
    if (!item || !item->data(0, Qt::UserRole).isValid()) {
        return;
    }

    m_tree->setCurrentItem(item); // keep the selection in sync with the menu

    QMenu menu(this);
    QAction* copyAction = menu.addAction(tr("Copy Path"));
    const QAction* chosen = menu.exec(m_tree->viewport()->mapToGlobal(pos));
    if (chosen == copyAction) {
        QApplication::clipboard()->setText(item->data(0, Qt::UserRole).toString());
    }
}

void ModelBrowserDialog::loadItem(QTreeWidgetItem* item) {
    if (!item) return;
    const QVariant path = item->data(0, Qt::UserRole);
    if (!path.isValid()) return; // not a model row
    const bool loose = item->data(0, Qt::UserRole + 1).toBool();
    emit modelSelected(path.toString(), loose);
    accept();
}

void ModelBrowserDialog::setStatus(const QString& text) {
    m_statusLabel->setText(text);
    m_statusLabel->setToolTip(text);
}

} // namespace qt
} // namespace ui
