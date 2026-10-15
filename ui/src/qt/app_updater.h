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

    void setRepository(const QString& owner, const QString& repo);
    QString owner() const { return m_owner; }
    QString repo() const { return m_repo; }

    // Initiates an asynchronous check against GitHub Releases.
    // If manual is true, user-initiated (errors and up-to-date will notify).
    // If ignoredVersion is provided and matches the latest release tag,
    // the updateAvailable signal is suppressed during automatic checks.
    void checkForUpdates(bool manual = false, const QString& ignoredVersion = QString());

    // Starts downloading the given release's best zip asset into a temporary location.
    void startDownload(const ReleaseInfo& release);

    // Cancels an ongoing download if one is active.
    void cancelDownload();

    // Extracts the downloaded zip and launches the detached updater script,
    // then schedules the application to terminate cleanly.
    bool applyUpdate(const QString& zipFilePath, QString& errorMessage);

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

private slots:
    void onCheckFinished();
    void onDownloadDataReady();
    void onDownloadFinished();

private:
    void cleanupCheckReply();
    void cleanupDownloadReply();

    QString m_owner = "Darvan76";
    QString m_repo = "VMTStudio";
    bool m_manualCheck = false;
    QString m_ignoredVersion;

    QNetworkAccessManager* m_net = nullptr;
    QNetworkReply* m_checkReply = nullptr;
    QNetworkReply* m_downloadReply = nullptr;

    std::unique_ptr<QFile> m_downloadFile;
    QString m_downloadPath;
    int m_redirectCount = 0;
    ReleaseInfo m_currentRelease;
};

} // namespace qt
} // namespace ui
