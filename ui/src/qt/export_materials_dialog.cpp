#include "export_materials_dialog.h"

#include "bridge/engine_view.h"
#include "ui/export_plan.h"
#include "ui/atomic_file.h"

#include <QAbstractItemView>
#include <QCheckBox>
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
#include <QTimer>

#include <cstddef>
#include <chrono>
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

// Use the same checked write/replace operation as the editor and preview.
bool write_file_atomically(const std::filesystem::path& path, const std::string& text,
                           std::string& error) {
    const vmt::Result<void> written = ui::atomic_write_file(path, text);
    if (!written) {
        error = written.error().message;
        if (!written.error().detail.empty()) {
            error += ": " + written.error().detail;
        }
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
    m_destinationEdit->setObjectName(QStringLiteral("exportDestination"));
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
    m_statusLabel->setObjectName(QStringLiteral("exportStatus"));
    m_statusLabel->setWordWrap(true);
    mainLayout->addWidget(m_statusLabel);

    // Buttons.
    auto* buttonRow = new QHBoxLayout();
    m_openFolderButton = new QPushButton(tr("Open Folder"), this);
    m_openFolderButton->setEnabled(false);
    m_exportButton = new QPushButton(tr("Export"), this);
    m_exportButton->setObjectName(QStringLiteral("exportStart"));
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
    m_exportTimer = new QTimer(this);
    m_exportTimer->setInterval(25);
    connect(m_exportTimer, &QTimer::timeout, this, &ExportMaterialsDialog::finishExport);
}

ExportMaterialsDialog::~ExportMaterialsDialog() {
    cancelExport();
    if (m_exportFuture.valid()) m_exportFuture.wait();
}

void ExportMaterialsDialog::cancelExport() {
    if (!m_exportFuture.valid()) return;
    m_cancelled->store(true);
}

void ExportMaterialsDialog::reject() {
    if (m_exportFuture.valid()) {
        m_closeAfterExport = true;
        cancelExport();
        m_statusLabel->setText(tr("Cancelling..."));
        return;
    }
    QDialog::reject();
}

void ExportMaterialsDialog::setExporting(bool exporting) {
    m_destinationEdit->setEnabled(!exporting);
    m_browseButton->setEnabled(!exporting);
    m_flatRadio->setEnabled(!exporting);
    m_treeRadio->setEnabled(!exporting);
    m_overwriteCheck->setEnabled(!exporting);
    m_exportButton->setEnabled(!exporting);
    m_closeButton->setText(exporting ? tr("Cancel and close") : tr("Close"));
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
    if (m_engine == nullptr || m_exportFuture.valid()) {
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
    m_logEdit->clear();
    appendLog(tr("Output folder: %1").arg(native_path(destination)));
    for (const std::string& duplicate : plan.duplicates) {
        appendLog(tr("Skipped (duplicate file name): %1")
                      .arg(QString::fromStdString(duplicate)));
    }

    const int total = static_cast<int>(plan.items.size());
    m_lastDestination = native_path(destination);
    m_openFolderButton->setEnabled(false);
    m_cancelled = std::make_shared<std::atomic<bool>>(false);
    m_completed = std::make_shared<std::atomic<int>>(0);
    m_closeAfterExport = false;
    const auto cancelled = m_cancelled;
    const auto completed = m_completed;
    auto* engine = m_engine;
    const bool overwrite = m_overwriteCheck->isChecked();
    try {
        m_exportFuture = std::async(std::launch::async, [plan, engine, overwrite, cancelled, completed]() {
            ExportResult result;
            for (const ui::ExportItem& item : plan.items) {
                if (cancelled->load()) break;
                completed->fetch_add(1);
                const QString displayName = display_path(item.relative);
                if (!overwrite) {
                    std::error_code existsEc;
                    const bool exists = std::filesystem::exists(item.absolute, existsEc);
                    if (existsEc) {
                        ++result.failed;
                        result.log.append(tr("Failed to check %1: %2")
                            .arg(displayName, QString::fromLocal8Bit(existsEc.message().c_str())));
                        continue;
                    }
                    if (exists) {
                        ++result.skipped;
                        continue;
                    }
                }

                const std::string gamePath = "materials/" + item.material + ".vmt";
                const vmt::Result<std::string> text = engine->read_text_file(gamePath, cancelled.get());
                if (cancelled->load()) break;
                if (!text.ok()) {
                    ++result.failed;
                    result.log.append(tr("Failed to read %1: %2")
                        .arg(QString::fromStdString(gamePath), QString::fromStdString(text.error().message)));
                    continue;
                }

                std::error_code dirEc;
                std::filesystem::create_directories(item.absolute.parent_path(), dirEc);
                if (dirEc) {
                    ++result.failed;
                    result.log.append(tr("Failed to create %1: %2")
                        .arg(native_path(item.absolute.parent_path()), QString::fromLocal8Bit(dirEc.message().c_str())));
                    continue;
                }

                std::string writeError;
                if (cancelled->load()) break;
                if (!write_file_atomically(item.absolute, text.value(), writeError)) {
                    ++result.failed;
                    result.log.append(tr("Failed to write %1: %2")
                        .arg(displayName, QString::fromStdString(writeError)));
                    continue;
                }
                ++result.written;
            }
            result.cancelled = cancelled->load();
            return result;
        });
    } catch (const std::exception& error) {
        m_statusLabel->setText(tr("Cannot start export: %1").arg(QString::fromUtf8(error.what())));
        return;
    }
    setExporting(true);
    m_progress = new QProgressDialog(tr("Exporting materials..."), tr("Cancel"), 0, total, this);
    // The parent export dialog is already modal. A modal progress setValue()
    // pumps nested events; a non-modal child keeps worker completion sequential.
    m_progress->setWindowModality(Qt::NonModal);
    m_progress->setMinimumDuration(0);
    m_progress->setAutoClose(false);
    m_progress->setAutoReset(false);
    connect(m_progress, &QProgressDialog::canceled, this, &ExportMaterialsDialog::cancelExport);
    m_progress->show();
    m_exportTimer->start();
}

void ExportMaterialsDialog::finishExport() {
    if (!m_exportFuture.valid()) return;
    if (m_progress) m_progress->setValue(m_completed->load());
    if (m_exportFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    m_exportTimer->stop();
    ExportResult result;
    try {
        result = m_exportFuture.get();
    } catch (const std::exception& error) {
        result.failed = 1;
        result.log.append(tr("Export failed: %1").arg(QString::fromUtf8(error.what())));
    }
    if (m_progress) {
        m_progress->close();
        m_progress->deleteLater();
        m_progress = nullptr;
    }
    setExporting(false);
    m_openFolderButton->setEnabled(true);
    for (const auto& line : result.log) appendLog(line);
    QString summary = tr("Exported %1 file(s), skipped %2, %3 failed")
                          .arg(result.written).arg(result.skipped).arg(result.failed);
    if (result.cancelled) {
        summary = tr("Cancelled. ") + summary;
    }
    const bool problem = result.cancelled || result.failed > 0;
    m_statusLabel->setStyleSheet(problem ? QStringLiteral("color: #f14c4c; font-weight: bold;")
                                         : QStringLiteral("color: #4ec9b0; font-weight: bold;"));
    m_statusLabel->setText(summary);
    if (m_closeAfterExport) QDialog::reject();
}

} // namespace qt
} // namespace ui
