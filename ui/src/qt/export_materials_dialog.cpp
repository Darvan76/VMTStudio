#include "export_materials_dialog.h"

#include "bridge/engine_view.h"
#include "ui/export_plan.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QUrl>
#include <QVBoxLayout>

#include <cstddef>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace ui {
namespace qt {

namespace {

// Rows shown in the file preview before the "and N more" marker.
constexpr std::size_t kMaxPreviewRows = 200;

// Keeps the default folder name valid: anything outside [A-Za-z0-9._-] would
// either break the path or need escaping on Windows. Falls back to "model".
QString safe_folder_name(const QString& text) {
    QString result;
    result.reserve(text.size());
    bool has_readable = false;
    for (const QChar ch : text) {
        const ushort code = ch.unicode();
        const bool safe_code = (code >= 'a' && code <= 'z') ||
                               (code >= 'A' && code <= 'Z') ||
                               (code >= '0' && code <= '9') ||
                               code == '.' || code == '_' || code == '-';
        result += safe_code ? ch : QChar('_');
        if (safe_code && code != '_' && code != '.') {
            has_readable = true;
        }
    }
    return has_readable ? result : QStringLiteral("model");
}

// Default destination: <home>/VMTStudio-export/<model stem>.
QString default_destination(const QString& modelName) {
    const QString stem = safe_folder_name(QFileInfo(modelName).completeBaseName());
    return QDir::homePath() + QStringLiteral("/VMTStudio-export/") + stem;
}

// Path for display in lists and log lines ("materials/models/...vmt").
QString display_path(const std::filesystem::path& path) {
    return QString::fromStdWString(path.generic_wstring());
}

// Path for the file system and the destination line, using native separators.
QString native_path(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

// Writes `text` to `path` through a sibling ".tmp" file followed by a rename,
// mirroring EditorSession::save_as(): a crash or a full disk never leaves a
// truncated .vmt behind, and an existing file is only replaced once the new
// file is complete. Returns false and fills `error` on failure.
bool write_file_atomically(const std::filesystem::path& path, const std::string& text,
                           std::string& error) {
    std::filesystem::path temp = path;
    temp += L".tmp";
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "cannot open the temporary file for writing";
            return false;
        }
        if (!text.empty()) {
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        }
        if (!stream) {
            error = "failed while writing the file";
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            return false;
        }
        stream.close();
        if (!stream) {
            error = "failed to close the file";
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        // Windows rename does not replace an existing file; remove and retry.
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        ec.clear();
        std::filesystem::rename(temp, path, ec);
    }
    if (ec) {
        error = "cannot replace the existing file: " + ec.message();
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return false;
    }
    return true;
}

} // namespace

ExportMaterialsDialog::ExportMaterialsDialog(bridge::EngineView* engine,
                                             QStringList materials,
                                             QString modelName,
                                             QWidget* parent)
    : QDialog(parent),
      m_engine(engine),
      m_materials(std::move(materials)),
      m_modelName(std::move(modelName)) {
    setWindowTitle(tr("Export Model Materials"));
    setModal(true);
    resize(640, 560);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);

    // Header: how many materials of which model this export covers.
    const QString headerText = m_modelName.isEmpty()
        ? tr("%1 materials").arg(static_cast<qlonglong>(m_materials.size()))
        : tr("%1 materials from %2")
              .arg(static_cast<qlonglong>(m_materials.size()))
              .arg(m_modelName);
    auto* headerLabel = new QLabel(headerText, this);
    headerLabel->setWordWrap(true);
    QFont headerFont = headerLabel->font();
    headerFont.setBold(true);
    headerLabel->setFont(headerFont);
    mainLayout->addWidget(headerLabel);

    // Destination row.
    auto* destinationRow = new QHBoxLayout();
    destinationRow->addWidget(new QLabel(tr("Destination folder:"), this));
    m_destinationEdit = new QLineEdit(default_destination(m_modelName), this);
    m_browseButton = new QPushButton(tr("Browse..."), this);
    destinationRow->addWidget(m_destinationEdit, 1);
    destinationRow->addWidget(m_browseButton);
    mainLayout->addLayout(destinationRow);

    // Layout choice.
    auto* layoutGroup = new QGroupBox(tr("Layout"), this);
    auto* layoutBox = new QVBoxLayout(layoutGroup);
    m_flatRadio = new QRadioButton(tr("All in one folder"), layoutGroup);
    m_treeRadio = new QRadioButton(tr("Materials tree (ready for the game)"), layoutGroup);
    m_flatRadio->setChecked(true);
    layoutBox->addWidget(m_flatRadio);
    layoutBox->addWidget(m_treeRadio);
    mainLayout->addWidget(layoutGroup);

    // Preview of the files the plan will write.
    mainLayout->addWidget(new QLabel(tr("Files to export:"), this));
    m_previewList = new QListWidget(this);
    m_previewList->setSelectionMode(QAbstractItemView::NoSelection);
    m_previewList->setAlternatingRowColors(true);
    mainLayout->addWidget(m_previewList, 1);

    m_overwriteCheck = new QCheckBox(tr("Overwrite existing files"), this);
    m_overwriteCheck->setChecked(true);
    mainLayout->addWidget(m_overwriteCheck);

    // Result log (kept after the export so failures can be inspected).
    mainLayout->addWidget(new QLabel(tr("Export log:"), this));
    m_logEdit = new QPlainTextEdit(this);
    m_logEdit->setReadOnly(true);
    m_logEdit->setMaximumHeight(120);
    m_logEdit->setPlaceholderText(tr("The output folder and any failures are listed here."));
    mainLayout->addWidget(m_logEdit);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    mainLayout->addWidget(m_statusLabel);

    // Buttons.
    auto* buttonRow = new QHBoxLayout();
    m_openFolderButton = new QPushButton(tr("Open Folder"), this);
    m_openFolderButton->setEnabled(false);
    m_exportButton = new QPushButton(tr("Export"), this);
    m_exportButton->setDefault(true);
    m_closeButton = new QPushButton(tr("Close"), this);
    buttonRow->addWidget(m_openFolderButton);
    buttonRow->addStretch();
    buttonRow->addWidget(m_exportButton);
    buttonRow->addWidget(m_closeButton);
    mainLayout->addLayout(buttonRow);

    connect(m_browseButton, &QPushButton::clicked,
            this, &ExportMaterialsDialog::browseForDestination);
    connect(m_destinationEdit, &QLineEdit::textChanged,
            this, &ExportMaterialsDialog::refreshPreview);
    connect(m_flatRadio, &QRadioButton::toggled,
            this, &ExportMaterialsDialog::refreshPreview);
    connect(m_exportButton, &QPushButton::clicked,
            this, &ExportMaterialsDialog::exportAll);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::close);
    connect(m_openFolderButton, &QPushButton::clicked, this, [this]() {
        if (!m_lastDestination.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_lastDestination));
        }
    });

    refreshPreview();
}

void ExportMaterialsDialog::browseForDestination() {
    const QString current = m_destinationEdit->text().trimmed();
    const QString chosen = QFileDialog::getExistingDirectory(
        this, tr("Select Destination Folder"),
        current.isEmpty() ? QDir::homePath() : current);
    if (!chosen.isEmpty()) {
        m_destinationEdit->setText(chosen);
    }
}

void ExportMaterialsDialog::refreshPreview() {
    m_previewList->clear();

    std::vector<std::string> materials;
    materials.reserve(static_cast<std::size_t>(m_materials.size()));
    for (const QString& name : m_materials) {
        materials.push_back(name.toStdString());
    }

    ui::ExportOptions options;
    options.layout = m_treeRadio->isChecked() ? ui::ExportLayout::MaterialsTree
                                              : ui::ExportLayout::Flat;
    options.destination = std::filesystem::path(m_destinationEdit->text().trimmed().toStdWString());

    const vmt::Result<ui::ExportPlan> plan = ui::plan_material_export(materials, options);
    if (!plan.ok()) {
        m_exportButton->setEnabled(false);
        m_statusLabel->setStyleSheet(QStringLiteral("color: #f14c4c; font-weight: bold;"));
        m_statusLabel->setText(QString::fromStdString(plan.error().message));
        return;
    }

    const ui::ExportPlan& value = plan.value();
    for (std::size_t index = 0; index < value.items.size() && index < kMaxPreviewRows; ++index) {
        m_previewList->addItem(display_path(value.items[index].relative));
    }
    if (value.items.size() > kMaxPreviewRows) {
        auto* moreItem = new QListWidgetItem(
            tr("and %1 more").arg(static_cast<qlonglong>(value.items.size() - kMaxPreviewRows)),
            m_previewList);
        moreItem->setFlags(Qt::NoItemFlags);
    }

    if (m_engine == nullptr) {
        m_exportButton->setEnabled(false);
        m_statusLabel->setStyleSheet(QStringLiteral("color: #f14c4c; font-weight: bold;"));
        m_statusLabel->setText(tr("No engine connection: the materials of the model cannot be read."));
        return;
    }

    QString status = tr("%1 file(s) ready to export").arg(static_cast<qlonglong>(value.items.size()));
    if (!value.duplicates.empty()) {
        status += tr(" - %1 duplicate name(s) skipped")
                      .arg(static_cast<qlonglong>(value.duplicates.size()));
    }
    m_exportButton->setEnabled(!value.items.empty());
    m_statusLabel->setStyleSheet(QStringLiteral("color: #888888;"));
    m_statusLabel->setText(status);
}

void ExportMaterialsDialog::appendLog(const QString& line) {
    m_logEdit->appendPlainText(line);
}

void ExportMaterialsDialog::exportAll() {
    if (m_engine == nullptr) {
        return; // the Export button is disabled in this case
    }

    const QString destinationText = m_destinationEdit->text().trimmed();
    std::vector<std::string> materials;
    materials.reserve(static_cast<std::size_t>(m_materials.size()));
    for (const QString& name : m_materials) {
        materials.push_back(name.toStdString());
    }

    ui::ExportOptions options;
    options.layout = m_treeRadio->isChecked() ? ui::ExportLayout::MaterialsTree
                                              : ui::ExportLayout::Flat;
    options.destination = std::filesystem::path(destinationText.toStdWString());

    const vmt::Result<ui::ExportPlan> planResult = ui::plan_material_export(materials, options);
    if (!planResult.ok()) {
        refreshPreview(); // puts the reason in the status label
        return;
    }
    const ui::ExportPlan& plan = planResult.value();
    if (plan.items.empty()) {
        return;
    }

    const std::filesystem::path destination = options.destination;
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    if (ec) {
        m_statusLabel->setStyleSheet(QStringLiteral("color: #f14c4c; font-weight: bold;"));
        m_statusLabel->setText(tr("Cannot create the output folder."));
        m_logEdit->setPlainText(tr("Cannot create the output folder %1: %2")
                                    .arg(native_path(destination),
                                         QString::fromLocal8Bit(ec.message().c_str())));
        return;
    }

    m_logEdit->clear();
    appendLog(tr("Output folder: %1").arg(native_path(destination)));
    for (const std::string& duplicate : plan.duplicates) {
        appendLog(tr("Skipped (duplicate file name): %1")
                      .arg(QString::fromStdString(duplicate)));
    }

    const int total = static_cast<int>(plan.items.size());
    QProgressDialog progress(tr("Exporting materials..."), tr("Cancel"), 0, total, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);

    int written = 0;
    int skipped = 0;
    int failed = 0;
    bool cancelled = false;

    for (std::size_t index = 0; index < plan.items.size(); ++index) {
        progress.setLabelText(
            tr("%1 / %2").arg(static_cast<qlonglong>(index) + 1).arg(total));
        progress.setValue(static_cast<int>(index));
        QCoreApplication::processEvents();
        if (progress.wasCanceled()) {
            cancelled = true;
            break;
        }

        const ui::ExportItem& item = plan.items[index];
        const QString displayName = display_path(item.relative);

        // "Overwrite" off: leave existing files untouched and count them.
        if (!m_overwriteCheck->isChecked()) {
            std::error_code existsEc;
            const bool exists = std::filesystem::exists(item.absolute, existsEc);
            if (existsEc) {
                ++failed;
                appendLog(tr("Failed to check %1: %2")
                              .arg(displayName,
                                   QString::fromLocal8Bit(existsEc.message().c_str())));
                continue;
            }
            if (exists) {
                ++skipped;
                continue;
            }
        }

        const std::string gamePath = "materials/" + item.material + ".vmt";
        const vmt::Result<std::string> text = m_engine->read_text_file(gamePath);
        if (!text.ok()) {
            ++failed;
            appendLog(tr("Failed to read %1: %2")
                          .arg(QString::fromStdString(gamePath),
                               QString::fromStdString(text.error().message)));
            continue;
        }

        std::error_code dirEc;
        std::filesystem::create_directories(item.absolute.parent_path(), dirEc);
        if (dirEc) {
            ++failed;
            appendLog(tr("Failed to create %1: %2")
                          .arg(native_path(item.absolute.parent_path()),
                               QString::fromLocal8Bit(dirEc.message().c_str())));
            continue;
        }

        std::string writeError;
        if (!write_file_atomically(item.absolute, text.value(), writeError)) {
            ++failed;
            appendLog(tr("Failed to write %1: %2")
                          .arg(displayName, QString::fromStdString(writeError)));
            continue;
        }
        ++written;
    }

    progress.close();

    m_lastDestination = native_path(destination);
    m_openFolderButton->setEnabled(true);

    QString summary = tr("Exported %1 file(s), skipped %2, %3 failed")
                          .arg(written).arg(skipped).arg(failed);
    if (cancelled) {
        summary = tr("Cancelled. ") + summary;
    }
    const bool problem = cancelled || failed > 0;
    m_statusLabel->setStyleSheet(problem ? QStringLiteral("color: #f14c4c; font-weight: bold;")
                                         : QStringLiteral("color: #4ec9b0; font-weight: bold;"));
    m_statusLabel->setText(summary);
}

} // namespace qt
} // namespace ui
