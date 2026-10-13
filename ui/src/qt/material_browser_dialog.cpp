#include "material_browser_dialog.h"
#include "bridge/engine_view.h"
#include "ui/texture_thumbnail.h"
#include "ui/material_info.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QHeaderView>
#include <QPixmap>
#include <QImage>
#include <QApplication>
#include <fstream>
#include <map>

namespace ui {
namespace qt {

namespace {

class DiskTextureSource final : public TextureSource {
public:
    explicit DiskTextureSource(const std::vector<std::filesystem::path>& roots, bridge::EngineView* engine)
        : m_roots(roots), m_engine(engine) {}

    vmt::Result<std::vector<unsigned char>> read(const std::string& game_path) override {
        std::error_code ec;
        for (const auto& root : m_roots) {
            auto candidate = root / std::filesystem::path(game_path);
            if (std::filesystem::is_regular_file(candidate, ec)) {
                std::ifstream stream(candidate, std::ios::binary);
                if (stream) {
                    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(stream)),
                                                     std::istreambuf_iterator<char>());
                    return vmt::Ok(std::move(bytes));
                }
            }
        }
        if (m_engine && m_engine->initialized()) {
            return m_engine->read_binary_file(game_path);
        }
        return vmt::Err<std::vector<unsigned char>>(vmt::make_error(
            vmt::ErrorCode::NotFound, "Texture not found: " + game_path));
    }

private:
    std::vector<std::filesystem::path> m_roots;
    bridge::EngineView* m_engine = nullptr;
};

} // namespace

MaterialBrowserDialog::MaterialBrowserDialog(bridge::EngineView* engine,
                                           const std::vector<std::filesystem::path>& materialRoots,
                                           QWidget* parent)
    : QDialog(parent), m_engine(engine), m_materialRoots(materialRoots) {
    setWindowTitle(tr("Material Browser"));
    resize(860, 560);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    // Top control bar
    auto* topBar = new QHBoxLayout();
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("Search materials (e.g. brick, concrete, metal)..."));
    m_searchEdit->setClearButtonEnabled(true);
    topBar->addWidget(m_searchEdit);

    m_scanGameBtn = new QPushButton(tr("Scan Game (VPK)"), this);
    m_scanFolderBtn = new QPushButton(tr("Scan Folder..."), this);
    topBar->addWidget(m_scanGameBtn);
    topBar->addWidget(m_scanFolderBtn);
    mainLayout->addLayout(topBar);

    // Splitter: Left (Tree of materials) / Right (Thumbnail & Info)
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    m_treeWidget = new QTreeWidget(splitter);
    m_treeWidget->setHeaderLabels({tr("Material / Folder"), tr("Source")});
    m_treeWidget->header()->resizeSection(0, 320);
    splitter->addWidget(m_treeWidget);

    auto* rightPanel = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(4, 0, 0, 0);
    rightLayout->setSpacing(8);

    m_thumbLabel = new QLabel(rightPanel);
    m_thumbLabel->setFixedSize(220, 220);
    m_thumbLabel->setAlignment(Qt::AlignCenter);
    m_thumbLabel->setStyleSheet("background-color: #1a1a1c; border: 1px solid #3e3e42; border-radius: 4px;");
    m_thumbLabel->setText(tr("No preview"));
    rightLayout->addWidget(m_thumbLabel, 0, Qt::AlignCenter);

    m_flipCheck = new QCheckBox(tr("Flip preview vertically"), rightPanel);
    rightLayout->addWidget(m_flipCheck);

    m_infoEdit = new QPlainTextEdit(rightPanel);
    m_infoEdit->setReadOnly(true);
    m_infoEdit->setPlaceholderText(tr("Material information and properties..."));
    rightLayout->addWidget(m_infoEdit);

    m_openBtn = new QPushButton(tr("Open in Editor"), rightPanel);
    m_openBtn->setEnabled(false);
    m_openBtn->setDefault(true);
    rightLayout->addWidget(m_openBtn);

    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    mainLayout->addWidget(splitter);

    // Status bar
    m_statusLabel = new QLabel(tr("Ready. Click 'Scan Game' or 'Scan Folder' to populate."), this);
    m_statusLabel->setStyleSheet("color: #888888; font-size: 8.5pt;");
    mainLayout->addWidget(m_statusLabel);

    // Connect signals
    connect(m_searchEdit, &QLineEdit::textChanged, this, &MaterialBrowserDialog::onSearchTextChanged);
    connect(m_scanGameBtn, &QPushButton::clicked, this, &MaterialBrowserDialog::onScanGameClicked);
    connect(m_scanFolderBtn, &QPushButton::clicked, this, &MaterialBrowserDialog::onScanFolderClicked);
    connect(m_treeWidget, &QTreeWidget::itemSelectionChanged, this, &MaterialBrowserDialog::onItemSelectionChanged);
    connect(m_treeWidget, &QTreeWidget::itemDoubleClicked, this, &MaterialBrowserDialog::onItemDoubleClicked);
    connect(m_openBtn, &QPushButton::clicked, this, &MaterialBrowserDialog::onOpenClicked);
    connect(m_flipCheck, &QCheckBox::toggled, this, [this]() {
        if (m_selectedEntryIndex >= 0 && m_selectedEntryIndex < static_cast<int>(m_index.size())) {
            showMaterialDetails(m_index.entries()[m_selectedEntryIndex]);
        }
    });
}

void MaterialBrowserDialog::onSearchTextChanged(const QString& text) {
    auto results = m_index.search(text.toStdString());
    populateTree(results);
}

void MaterialBrowserDialog::onScanGameClicked() {
    if (!m_engine || !m_engine->initialized()) {
        QMessageBox::information(this, tr("Engine Not Ready"),
            tr("Please configure and connect a Source game via Game > Connection first."));
        return;
    }
    m_statusLabel->setText(tr("Scanning game materials from VPK..."));
    QApplication::processEvents();

    auto listRes = m_engine->list_materials();
    if (!listRes) {
        QMessageBox::warning(this, tr("Scan Failed"),
            QString::fromStdString(listRes.error().message));
        m_statusLabel->setText(tr("Scan failed."));
        return;
    }

    std::size_t added = m_index.add_engine_names(listRes.value());
    m_statusLabel->setText(tr("Loaded %1 materials from game.").arg(added));
    onSearchTextChanged(m_searchEdit->text());
}

void MaterialBrowserDialog::onScanFolderClicked() {
    QString folder = QFileDialog::getExistingDirectory(this, tr("Select Materials Folder"));
    if (folder.isEmpty()) return;

    m_statusLabel->setText(tr("Scanning folder: %1...").arg(folder));
    QApplication::processEvents();

    std::size_t added = m_index.add_directory(std::filesystem::path(folder.toStdWString()));
    m_statusLabel->setText(tr("Added %1 materials from %2.").arg(added).arg(folder));
    onSearchTextChanged(m_searchEdit->text());
}

void MaterialBrowserDialog::populateTree(const std::vector<std::size_t>& indices) {
    m_treeWidget->clear();
    m_selectedEntryIndex = -1;
    m_openBtn->setEnabled(false);

    std::map<std::string, QTreeWidgetItem*> folderNodes;

    const std::size_t kMaxDisplay = 2000;
    std::size_t count = std::min(indices.size(), kMaxDisplay);

    for (std::size_t i = 0; i < count; ++i) {
        std::size_t entryIdx = indices[i];
        const auto& entry = m_index.entries()[entryIdx];

        std::string name = entry.name;
        std::string folder = "root";
        std::string filename = name;

        auto slashPos = name.rfind('/');
        if (slashPos != std::string::npos) {
            folder = name.substr(0, slashPos);
            filename = name.substr(slashPos + 1);
        }

        QTreeWidgetItem* folderItem = nullptr;
        auto fIt = folderNodes.find(folder);
        if (fIt != folderNodes.end()) {
            folderItem = fIt->second;
        } else {
            folderItem = new QTreeWidgetItem(m_treeWidget, {QString::fromStdString(folder)});
            folderItem->setExpanded(indices.size() < 50); // Expand if small result set
            folderNodes[folder] = folderItem;
        }

        auto* item = new QTreeWidgetItem(folderItem, {
            QString::fromStdString(filename),
            entry.from_engine ? tr("Game (VPK)") : tr("Disk")
        });
        item->setData(0, Qt::UserRole, static_cast<int>(entryIdx));
    }

    if (indices.size() > kMaxDisplay) {
        m_statusLabel->setText(tr("Showing %1 of %2 matching materials (refine search for more).")
            .arg(count).arg(indices.size()));
    } else {
        m_statusLabel->setText(tr("%1 materials matching filter.").arg(indices.size()));
    }
}

void MaterialBrowserDialog::onItemSelectionChanged() {
    auto selectedItems = m_treeWidget->selectedItems();
    if (selectedItems.isEmpty()) {
        m_selectedEntryIndex = -1;
        m_openBtn->setEnabled(false);
        m_thumbLabel->setText(tr("No preview"));
        m_infoEdit->clear();
        return;
    }

    auto* item = selectedItems.first();
    QVariant itemData = item->data(0, Qt::UserRole);
    if (!itemData.isValid()) {
        m_selectedEntryIndex = -1;
        m_openBtn->setEnabled(false);
        return;
    }

    int entryIdx = itemData.toInt();
    if (entryIdx >= 0 && entryIdx < static_cast<int>(m_index.size())) {
        m_selectedEntryIndex = entryIdx;
        m_openBtn->setEnabled(true);
        showMaterialDetails(m_index.entries()[entryIdx]);
    }
}

void MaterialBrowserDialog::showMaterialDetails(const ui::MaterialEntry& entry) {
    m_infoEdit->setPlainText(tr("Loading material: %1...").arg(QString::fromStdString(entry.name)));

    std::string text;
    if (!entry.disk_path.empty()) {
        std::ifstream stream(entry.disk_path, std::ios::binary);
        if (stream) {
            text.assign((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        }
    } else if (m_engine && m_engine->initialized()) {
        auto res = m_engine->read_text_file("materials/" + entry.name + ".vmt");
        if (res) text = res.value();
    }

    if (!text.empty()) {
        m_infoEdit->setPlainText(QString::fromStdString(text));
    } else {
        m_infoEdit->setPlainText(tr("Material file could not be read."));
    }

    // Attempt thumbnail decode
    DiskTextureSource source(m_materialRoots, m_engine);
    auto thumbRes = ui::make_texture_thumbnail(text, source, 220);

    if (thumbRes && thumbRes.value().width > 0 && thumbRes.value().height > 0) {
        const auto& thumb = thumbRes.value();
        QImage img(thumb.width, thumb.height, QImage::Format_ARGB32);
        const unsigned char* src = thumb.bgra.data();
        bool flip = m_flipCheck->isChecked();

        for (int y = 0; y < thumb.height; ++y) {
            int srcY = flip ? (thumb.height - 1 - y) : y;
            const unsigned char* row = src + (srcY * thumb.width * 4);
            QRgb* dest = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < thumb.width; ++x) {
                unsigned char b = row[x * 4 + 0];
                unsigned char g = row[x * 4 + 1];
                unsigned char r = row[x * 4 + 2];
                unsigned char a = row[x * 4 + 3];
                dest[x] = qRgba(r, g, b, a);
            }
        }
        m_thumbLabel->setPixmap(QPixmap::fromImage(img).scaled(
            m_thumbLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        QString errStr = thumbRes ? tr("Failed to decode") : QString::fromStdString(thumbRes.error().message);
        m_thumbLabel->setText(tr("No preview\n(%1)").arg(errStr));
    }
}

void MaterialBrowserDialog::onItemDoubleClicked(QTreeWidgetItem* item, int column) {
    Q_UNUSED(column);
    if (!item) return;
    QVariant itemData = item->data(0, Qt::UserRole);
    if (itemData.isValid()) {
        onOpenClicked();
    }
}

void MaterialBrowserDialog::onOpenClicked() {
    if (m_selectedEntryIndex >= 0 && m_selectedEntryIndex < static_cast<int>(m_index.size())) {
        const auto& entry = m_index.entries()[m_selectedEntryIndex];
        emit materialSelected(
            QString::fromStdString(entry.name),
            QString::fromStdWString(entry.disk_path.wstring()),
            entry.from_engine);
        accept();
    }
}

} // namespace qt
} // namespace ui
