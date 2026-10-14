#include "connection_dialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QMessageBox>

namespace ui {
namespace qt {

ConnectionDialog::ConnectionDialog(ui::UISettings& settings, QWidget* parent)
    : QDialog(parent), m_settings(settings) {
    setWindowTitle(tr("Game Connection Settings"));
    setMinimumWidth(500);
    setMinimumHeight(380);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);

    // Group: Paths
    auto* pathsGroup = new QGroupBox(tr("Source Engine Paths"), this);
    auto* pathsLayout = new QFormLayout(pathsGroup);

    // Bin folder
    auto* binRow = new QWidget(pathsGroup);
    auto* binLayout = new QHBoxLayout(binRow);
    binLayout->setContentsMargins(0, 0, 0, 0);
    m_binEdit = new QLineEdit(QString::fromStdString(m_settings.last_bin_dir), binRow);
    m_binBrowseBtn = new QPushButton(tr("Browse..."), binRow);
    binLayout->addWidget(m_binEdit);
    binLayout->addWidget(m_binBrowseBtn);
    pathsLayout->addRow(tr("Game /bin folder:"), binRow);

    // Game folder
    auto* gameRow = new QWidget(pathsGroup);
    auto* gameLayout = new QHBoxLayout(gameRow);
    gameLayout->setContentsMargins(0, 0, 0, 0);
    m_gameEdit = new QLineEdit(QString::fromStdString(m_settings.last_game_dir), gameRow);
    m_gameBrowseBtn = new QPushButton(tr("Browse..."), gameRow);
    m_autoDetectBtn = new QPushButton(tr("Auto-detect"), gameRow);
    gameLayout->addWidget(m_gameEdit);
    gameLayout->addWidget(m_gameBrowseBtn);
    gameLayout->addWidget(m_autoDetectBtn);
    pathsLayout->addRow(tr("Game mod folder:"), gameRow);

    mainLayout->addWidget(pathsGroup);

    // Diagnostics / Test Group
    auto* testGroup = new QGroupBox(tr("Connection Diagnostics"), this);
    auto* testLayout = new QVBoxLayout(testGroup);

    auto* btnRow = new QHBoxLayout();
    m_testBtn = new QPushButton(tr("Test Connection"), testGroup);
    m_testBtn->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    btnRow->addWidget(m_testBtn);

    m_archLabel = new QLabel(tr("Architecture: --"), testGroup);
    btnRow->addWidget(m_archLabel);
    btnRow->addStretch();
    testLayout->addLayout(btnRow);

    m_summaryLabel = new QLabel(tr("Status: Not tested yet"), testGroup);
    m_summaryLabel->setStyleSheet("font-weight: bold;");
    testLayout->addWidget(m_summaryLabel);

    m_resultsList = new QListWidget(testGroup);
    testLayout->addWidget(m_resultsList);

    mainLayout->addWidget(testGroup);

    // Bottom Action Buttons
    auto* actionRow = new QHBoxLayout();
    actionRow->addStretch();

    m_saveBtn = new QPushButton(tr("Save & Connect"), this);
    m_saveBtn->setDefault(true);
    m_cancelBtn = new QPushButton(tr("Cancel"), this);

    actionRow->addWidget(m_saveBtn);
    actionRow->addWidget(m_cancelBtn);
    mainLayout->addLayout(actionRow);

    // Connect signals
    connect(m_binBrowseBtn, &QPushButton::clicked, this, &ConnectionDialog::onBrowseBin);
    connect(m_gameBrowseBtn, &QPushButton::clicked, this, &ConnectionDialog::onBrowseGame);
    connect(m_autoDetectBtn, &QPushButton::clicked, this, &ConnectionDialog::onAutoDetectGame);
    connect(m_testBtn, &QPushButton::clicked, this, &ConnectionDialog::onTestConnection);
    connect(m_saveBtn, &QPushButton::clicked, this, &ConnectionDialog::onSaveAndConnect);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);

    // If paths are populated, test right away
    if (!m_settings.last_bin_dir.empty()) {
        onTestConnection();
    }
}

void ConnectionDialog::onBrowseBin() {
    QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Source Game /bin Folder"),
        m_binEdit->text().isEmpty() ? QString() : m_binEdit->text());
    if (!dir.isEmpty()) {
        m_binEdit->setText(dir);
        onAutoDetectGame();
    }
}

void ConnectionDialog::onBrowseGame() {
    QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Game Folder (Root or Mod Folder)"),
        m_gameEdit->text().isEmpty() ? QString() : m_gameEdit->text());
    if (!dir.isEmpty()) {
        m_gameEdit->setText(dir);

        // Crowbar-style convenience: after choosing a game root, try to locate
        // its /bin folder and the gameinfo.txt folder automatically.
        std::filesystem::path selected(dir.toStdWString());
        std::filesystem::path bin = ui::detect_bin_dir(selected);
        if (!bin.empty()) {
            m_binEdit->setText(QString::fromStdWString(bin.wstring()));
        }
        std::filesystem::path game = ui::detect_game_dir(bin.empty() ? selected : bin);
        if (!game.empty()) {
            m_gameEdit->setText(QString::fromStdWString(game.wstring()));
        }
    }
}

void ConnectionDialog::onAutoDetectGame() {
    std::filesystem::path binPath(m_binEdit->text().toStdWString());
    std::filesystem::path detected = ui::detect_game_dir(binPath);
    if (!detected.empty()) {
        m_gameEdit->setText(QString::fromStdWString(detected.wstring()));
    }
}

void ConnectionDialog::onTestConnection() {
    std::filesystem::path binPath(m_binEdit->text().toStdWString());
    ui::ConnectionCheck check = ui::test_connection(binPath);

    m_archLabel->setText(tr("Architecture: %1").arg(QString::fromStdString(check.architecture)));
    m_summaryLabel->setText(QString::fromStdString(check.summary));
    if (check.connected) {
        m_summaryLabel->setStyleSheet("color: #4ec9b0; font-weight: bold;");
    } else {
        m_summaryLabel->setStyleSheet("color: #f14c4c; font-weight: bold;");
    }

    m_resultsList->clear();
    for (const auto& line : check.lines) {
        auto* item = new QListWidgetItem(QString::fromStdString(line), m_resultsList);
        if (line.rfind("[OK]", 0) == 0) {
            item->setForeground(QColor("#4ec9b0"));
        } else if (line.rfind("[FAIL]", 0) == 0) {
            item->setForeground(QColor("#f14c4c"));
        }
    }
}

void ConnectionDialog::onSaveAndConnect() {
    m_settings.last_bin_dir = m_binEdit->text().toStdString();
    m_settings.last_game_dir = m_gameEdit->text().toStdString();
    accept();
}

} // namespace qt
} // namespace ui
