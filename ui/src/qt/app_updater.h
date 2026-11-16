#pragma once

#include <QDateTime>
#include <QFile>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QUrl>
#include <memory>
#include <atomic>
#include <future>

class QTimer;
class QTemporaryDir;

#include "ui/version.h"

namespace ui {
namespace qt {

struct ReleaseAsset {
    QString name;
    QString downloadUrl;
    qint64 size = 0;
};

struct ReleaseInfo {
    QString tagName;   // e.g. "v0.1.1"
    QString name;      // Release title
    QString body;      // Markdown changelog
    QString htmlUrl;   // GitHub web page URL
    QDateTime publishedAt;
    bool isDraft = false;
    bool isPrerelease = false;
    QList<ReleaseAsset> assets;

    AppVersion version() const {
        return AppVersion::parse(tagName.toStdString());
    }

    ReleaseAsset bestAsset() const {
        ReleaseAsset best;
        for (const auto& a : assets) {
            if (a.downloadUrl.isEmpty()) {
                continue;
            }
            if (a.name.endsWith(".zip", Qt::CaseInsensitive)) {
                if (a.name.contains("debug", Qt::CaseInsensitive)) {
                    continue;
                }
                if (a.name.contains("windows", Qt::CaseInsensitive) ||
                    a.name.contains("win64", Qt::CaseInsensitive) ||
                    a.name.contains("x64", Qt::CaseInsensitive)) {
                    return a;
                }
                if (best.downloadUrl.isEmpty()) {
                    best = a;
                }
            }
        }
        return best;
    }
};

class AppUpdater : public QObject {
    Q_OBJECT

public:
    explicit AppUpdater(QObject* parent = nullptr);
    ~AppUpdater() override;

    // Initiates an asynchronous check against GitHub Releases.
    // If manual is true, user-initiated (errors and up-to-date will notify).
    // If ignoredVersion is provided and matches the latest release tag,
    // the updateAvailable signal is suppressed during automatic checks.
    void checkForUpdates(bool manual = false, const QString& ignoredVersion = QString());

    // Starts downloading the given release's best zip asset into a temporary location.
    void startDownload(const ReleaseInfo& release);

    // Cancels an ongoing download if one is active.
    void cancelDownload();

    // Prepares the package in a cancellable worker. Only a completed, current
    // preparation may launch the updater and schedule application shutdown.
    void applyUpdate(const QString& zipFilePath);
    bool isPreparing() const { return m_preparation.valid(); }

signals:
    // Fired when a valid, newer release is found.
    void updateAvailable(const ReleaseInfo& release, bool manual);

    // Fired when the app is already at or beyond the latest version.
    void upToDate(bool manual);

    // Fired when GitHub API network or parsing fails.
    void checkFailed(const QString& error, bool manual);

    // Download progress signals
    void downloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void downloadFinished(const QString& zipFilePath);
    void downloadFailed(const QString& error);

    // Update extraction / preparation status
    void extractionStatus(const QString& statusText);
    void updateFailed(const QString& error);
    void updateCancelled();

private slots:
    void onCheckFinished();
    void onDownloadDataReady();
    void onDownloadFinished();

private:
    void cleanupCheckReply();
    void cleanupDownloadReply();
    bool prepareUpdate(const QString& zipFilePath,
                       const std::shared_ptr<std::atomic<bool>>& cancelled,
                       QString& payloadDir, QString& stagingDir, QString& errorMessage);
    void finishPreparation();

    struct PreparedUpdate {
        QString payloadDir;
        QString stagingDir;
        QString error;
    };
    bool launchUpdate(const PreparedUpdate& update, QString& errorMessage);
    std::future<PreparedUpdate> m_preparation;
    std::shared_ptr<std::atomic<bool>> m_cancelPreparation;
    QTimer* m_applyTimer = nullptr;
    QString m_updateZip;

    QString m_owner = "Darvan76";
    QString m_repo = "VMTStudio";
    bool m_manualCheck = false;
    QString m_ignoredVersion;

    QNetworkAccessManager* m_net = nullptr;
    QNetworkReply* m_checkReply = nullptr;
    QNetworkReply* m_downloadReply = nullptr;

    std::unique_ptr<QFile> m_downloadFile;
    std::unique_ptr<QTemporaryDir> m_downloadDirectory;
    QString m_downloadPath;
    int m_redirectCount = 0;
    ReleaseInfo m_currentRelease;
};

} // namespace qt
} // namespace ui
