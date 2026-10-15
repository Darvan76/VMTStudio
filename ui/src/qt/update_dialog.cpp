#include "update_dialog.h"

#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QPixmap>
#include <QUrl>
#include <QVBoxLayout>

#include "ui/version.h"

namespace ui {
namespace qt {

UpdateDialog::UpdateDialog(const ReleaseInfo& release, AppUpdater* updater, QWidget* parent)
    : QDialog(parent),
      m_release(release),
      m_updater(updater) {
    setWindowTitle(tr("Actualización disponible - VMT-Studio"));
    setWindowIcon(QIcon(":/icons/app.png"));
    resize(560, 460);
    setMinimumSize(480, 400);

    setupUi();

    if (m_updater) {
        connect(m_updater, &AppUpdater::downloadProgress, this, &UpdateDialog::onDownloadProgress);
        connect(m_updater, &AppUpdater::downloadFinished, this, &UpdateDialog::onDownloadFinished);
        connect(m_updater, &AppUpdater::downloadFailed, this, &UpdateDialog::onDownloadFailed);
        connect(m_updater, &AppUpdater::extractionStatus, this, &UpdateDialog::onExtractionStatus);
    }
}

void UpdateDialog::setupUi() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 20, 20, 20);
    mainLayout->setSpacing(14);

    // --- Header Section ---
    auto* headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(16);

    auto* iconLabel = new QLabel(this);
    QPixmap iconPix(":/icons/app.png");
    if (!iconPix.isNull()) {
        iconLabel->setPixmap(iconPix.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    headerLayout->addWidget(iconLabel, 0, Qt::AlignTop);

    auto* headerTextLayout = new QVBoxLayout();
    headerTextLayout->setSpacing(4);

    m_titleLabel = new QLabel(tr("<span style='font-size: 15px; font-weight: bold; color: #ffffff;'>¡Nueva versión de VMT-Studio disponible!</span>"), this);
    headerTextLayout->addWidget(m_titleLabel);

    const QString currentVerStr = QString::fromStdString(AppVersion::current().to_string());
    const QString newVerStr = m_release.tagName;
    m_versionLabel = new QLabel(
        tr("Versión actual: <span style='color: #888888;'>%1</span> &nbsp;&nbsp;➜&nbsp;&nbsp; Nueva versión: <b style='color: #e08214;'>%2</b>")
            .arg(currentVerStr, newVerStr), this);
    m_versionLabel->setTextFormat(Qt::RichText);
    headerTextLayout->addWidget(m_versionLabel);

    QString dateStr = m_release.publishedAt.isValid()
                          ? m_release.publishedAt.toString("dd/MM/yyyy HH:mm")
                          : QString();
    QString releaseNameStr = m_release.name.isEmpty() ? m_release.tagName : m_release.name;
    if (!dateStr.isEmpty()) {
        m_dateLabel = new QLabel(tr("<span style='color: #aaaaaa; font-size: 11px;'>%1 • Publicado el %2</span>").arg(releaseNameStr, dateStr), this);
    } else {
        m_dateLabel = new QLabel(tr("<span style='color: #aaaaaa; font-size: 11px;'>%1</span>").arg(releaseNameStr), this);
    }
    m_dateLabel->setTextFormat(Qt::RichText);
    headerTextLayout->addWidget(m_dateLabel);

    headerLayout->addLayout(headerTextLayout, 1);
    mainLayout->addLayout(headerLayout);

    // Separator line
    auto* sepLine = new QFrame(this);
    sepLine->setFrameShape(QFrame::HLine);
    sepLine->setFrameShadow(QFrame::Sunken);
    sepLine->setStyleSheet("background-color: #333333; max-height: 1px;");
    mainLayout->addWidget(sepLine);

    // --- Changelog / Release Notes ---
    auto* notesTitle = new QLabel(tr("<b>Novedades y cambios:</b>"), this);
    mainLayout->addWidget(notesTitle);

    m_notesBrowser = new QTextBrowser(this);
    m_notesBrowser->setOpenExternalLinks(true);
    m_notesBrowser->setStyleSheet(
        "QTextBrowser {"
        "  background-color: #252526;"
        "  border: 1px solid #3f3f46;"
        "  border-radius: 4px;"
        "  color: #d4d4d4;"
        "  padding: 8px;"
        "  font-size: 12px;"
        "}"
    );

    if (m_release.body.trimmed().isEmpty()) {
        m_notesBrowser->setMarkdown(tr("*No se especificaron notas para esta release en GitHub.*"));
    } else {
        m_notesBrowser->setMarkdown(m_release.body);
    }
    mainLayout->addWidget(m_notesBrowser, 1);

    // --- Progress Section (hidden initially) ---
    m_progressContainer = new QWidget(this);
    auto* progressLayout = new QVBoxLayout(m_progressContainer);
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->setSpacing(8);

    m_progressLabel = new QLabel(tr("Preparando descarga..."), m_progressContainer);
    m_progressLabel->setStyleSheet("color: #e08214; font-weight: 500;");
    progressLayout->addWidget(m_progressLabel);

    m_progressBar = new QProgressBar(m_progressContainer);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setFixedHeight(18);
    m_progressBar->setStyleSheet(
        "QProgressBar {"
        "  border: 1px solid #3f3f46;"
        "  border-radius: 3px;"
        "  text-align: center;"
        "  background-color: #252526;"
        "  color: #ffffff;"
        "}"
        "QProgressBar::chunk {"
        "  background-color: #e08214;"
        "  border-radius: 2px;"
        "}"
    );
    progressLayout->addWidget(m_progressBar);

    auto* cancelDownloadLayout = new QHBoxLayout();
    cancelDownloadLayout->addStretch();
    m_cancelDownloadBtn = new QPushButton(tr("Cancelar descarga"), m_progressContainer);
    m_cancelDownloadBtn->setStyleSheet(
        "QPushButton { background-color: #3e3e42; color: #ffffff; border: 1px solid #555555; padding: 4px 14px; border-radius: 3px; }"
        "QPushButton:hover { background-color: #4e4e52; }"
    );
    connect(m_cancelDownloadBtn, &QPushButton::clicked, this, &UpdateDialog::onCancelDownloadClicked);
    cancelDownloadLayout->addWidget(m_cancelDownloadBtn);
    progressLayout->addLayout(cancelDownloadLayout);

    m_progressContainer->hide();
    mainLayout->addWidget(m_progressContainer);

    // --- Button Bar ---
    m_buttonContainer = new QWidget(this);
    auto* buttonLayout = new QHBoxLayout(m_buttonContainer);
    buttonLayout->setContentsMargins(0, 4, 0, 0);
    buttonLayout->setSpacing(10);

    // Skip this version button (left side)
    m_skipBtn = new QPushButton(tr("No recordar esta actualización"), m_buttonContainer);
    m_skipBtn->setCursor(Qt::PointingHandCursor);
    m_skipBtn->setToolTip(tr("Ignora esta versión para que no vuelva a aparecer el aviso"));
    m_skipBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: transparent;"
        "  color: #888888;"
        "  border: 1px solid #444444;"
        "  padding: 6px 14px;"
        "  border-radius: 3px;"
        "}"
        "QPushButton:hover {"
        "  color: #ffffff;"
        "  border-color: #666666;"
        "  background-color: #2d2d30;"
        "}"
    );
    connect(m_skipBtn, &QPushButton::clicked, this, &UpdateDialog::onSkipClicked);
    buttonLayout->addWidget(m_skipBtn);

    buttonLayout->addStretch();

    // Open on GitHub button
    m_browserBtn = new QPushButton(tr("Ver en GitHub"), m_buttonContainer);
    m_browserBtn->setCursor(Qt::PointingHandCursor);
    m_browserBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #2d2d30;"
        "  color: #cccccc;"
        "  border: 1px solid #3f3f46;"
        "  padding: 6px 14px;"
        "  border-radius: 3px;"
        "}"
        "QPushButton:hover {"
        "  color: #ffffff;"
        "  background-color: #3e3e42;"
        "}"
    );
    connect(m_browserBtn, &QPushButton::clicked, this, &UpdateDialog::onOpenInBrowserClicked);
    buttonLayout->addWidget(m_browserBtn);

    // Later button (remind again on next startup)
    m_laterBtn = new QPushButton(tr("Más tarde"), m_buttonContainer);
    m_laterBtn->setCursor(Qt::PointingHandCursor);
    m_laterBtn->setToolTip(tr("Cerrar por ahora. Volverá a recordarse en el próximo inicio."));
    m_laterBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #3e3e42;"
        "  color: #ffffff;"
        "  border: 1px solid #555555;"
        "  padding: 6px 18px;"
        "  border-radius: 3px;"
        "}"
        "QPushButton:hover {"
        "  background-color: #4e4e52;"
        "}"
    );
    connect(m_laterBtn, &QPushButton::clicked, this, &UpdateDialog::onLaterClicked);
    buttonLayout->addWidget(m_laterBtn);

    // Update Now button (Primary action)
    m_updateBtn = new QPushButton(tr("Actualizar ahora"), m_buttonContainer);
    m_updateBtn->setCursor(Qt::PointingHandCursor);
    m_updateBtn->setDefault(true);
    m_updateBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #e08214;"
        "  color: #ffffff;"
        "  font-weight: bold;"
        "  border: none;"
        "  padding: 6px 20px;"
        "  border-radius: 3px;"
        "}"
        "QPushButton:hover {"
        "  background-color: #f09224;"
        "}"
        "QPushButton:pressed {"
        "  background-color: #c07204;"
        "}"
    );
    connect(m_updateBtn, &QPushButton::clicked, this, &UpdateDialog::onUpdateClicked);
    buttonLayout->addWidget(m_updateBtn);

    mainLayout->addWidget(m_buttonContainer);
}

void UpdateDialog::onUpdateClicked() {
    const ReleaseAsset asset = m_release.bestAsset();
    if (asset.downloadUrl.isEmpty()) {
        // Fallback: Open GitHub release page in browser
        QDesktopServices::openUrl(QUrl(m_release.htmlUrl));
        return;
    }

    m_choice = UserChoice::UpdateNow;
    emit userChoseUpdate();

    m_buttonContainer->hide();
    m_progressContainer->show();
    m_progressLabel->setText(tr("Iniciando descarga de %1...").arg(asset.name));
    m_progressBar->setValue(0);
    m_cancelDownloadBtn->setText(tr("Cancelar descarga"));

    if (m_updater) {
        m_updater->startDownload(m_release);
    }
}

void UpdateDialog::onLaterClicked() {
    m_choice = UserChoice::Later;
    emit userChoseLater();
    reject();
}

void UpdateDialog::onSkipClicked() {
    m_choice = UserChoice::SkipThisVersion;
    emit userChoseSkip(m_release.tagName);
    accept();
    close();
}

void UpdateDialog::reject() {
    if (m_choice == UserChoice::UpdateNow && m_updater) {
        m_updater->cancelDownload();
    }
    QDialog::reject();
    close();
}

void UpdateDialog::onCancelDownloadClicked() {
    if (m_updater) {
        m_updater->cancelDownload();
    }
    if (m_cancelDownloadBtn->text() == tr("Cerrar")) {
        reject();
        return;
    }
    m_progressContainer->hide();
    m_buttonContainer->show();
}

void UpdateDialog::onOpenInBrowserClicked() {
    if (!m_release.htmlUrl.isEmpty()) {
        QDesktopServices::openUrl(QUrl(m_release.htmlUrl));
    }
}

void UpdateDialog::onDownloadProgress(qint64 received, qint64 total) {
    if (total > 0) {
        const int percent = static_cast<int>((received * 100) / total);
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(percent);

        const double recvMB = static_cast<double>(received) / (1024.0 * 1024.0);
        const double totalMB = static_cast<double>(total) / (1024.0 * 1024.0);
        m_progressLabel->setText(
            tr("Descargando actualización: %1 MB / %2 MB (%3%)")
                .arg(QString::number(recvMB, 'f', 1),
                     QString::number(totalMB, 'f', 1),
                     QString::number(percent)));
    } else {
        m_progressBar->setRange(0, 0); // indeterminate
        const double recvMB = static_cast<double>(received) / (1024.0 * 1024.0);
        m_progressLabel->setText(tr("Descargando actualización: %1 MB descargados...")
                                     .arg(QString::number(recvMB, 'f', 1)));
    }
}

void UpdateDialog::onDownloadFinished(const QString& zipFilePath) {
    m_progressLabel->setText(tr("Descarga completada. Extrayendo archivos..."));
    m_progressBar->setRange(0, 0);
    m_cancelDownloadBtn->setEnabled(false);

    QString error;
    if (m_updater) {
        const bool success = m_updater->applyUpdate(zipFilePath, error);
        if (!success) {
            m_progressBar->setRange(0, 100);
            m_progressBar->setValue(0);
            m_progressLabel->setText(tr("<b style='color:#ff5555;'>Error al aplicar actualización:</b> %1").arg(error));
            m_cancelDownloadBtn->setEnabled(true);
            m_cancelDownloadBtn->setText(tr("Cerrar"));
        }
    }
}

void UpdateDialog::onDownloadFailed(const QString& error) {
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressLabel->setText(tr("<b style='color:#ff5555;'>Error en la descarga:</b> %1").arg(error));
    m_cancelDownloadBtn->setText(tr("Volver"));
}

void UpdateDialog::onExtractionStatus(const QString& status) {
    m_progressLabel->setText(status);
}

void UpdateDialog::closeEvent(QCloseEvent* event) {
    if (m_choice == UserChoice::UpdateNow && m_updater) {
        m_updater->cancelDownload();
    }
    QDialog::closeEvent(event);
}

} // namespace qt
} // namespace ui
