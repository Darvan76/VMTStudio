#include "app_updater.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

namespace ui {
namespace qt {

AppUpdater::AppUpdater(QObject* parent)
    : QObject(parent),
      m_net(new QNetworkAccessManager(this)) {
}

AppUpdater::~AppUpdater() {
    cleanupCheckReply();
    cleanupDownloadReply();
}

void AppUpdater::setRepository(const QString& owner, const QString& repo) {
    m_owner = owner;
    m_repo = repo;
}

void AppUpdater::cleanupCheckReply() {
    if (m_checkReply) {
        m_checkReply->disconnect(this);
        if (m_checkReply->isRunning()) {
            m_checkReply->abort();
        }
        m_checkReply->deleteLater();
        m_checkReply = nullptr;
    }
}

void AppUpdater::cleanupDownloadReply() {
    if (m_downloadReply) {
        m_downloadReply->disconnect(this);
        if (m_downloadReply->isRunning()) {
            m_downloadReply->abort();
        }
        m_downloadReply->deleteLater();
        m_downloadReply = nullptr;
    }
    if (m_downloadFile && m_downloadFile->isOpen()) {
        m_downloadFile->close();
    }
    m_downloadFile.reset();
}

void AppUpdater::checkForUpdates(bool manual, const QString& ignoredVersion) {
    cleanupCheckReply();

    m_manualCheck = manual;
    m_ignoredVersion = ignoredVersion.trimmed();

    const QString urlString = QString("https://api.github.com/repos/%1/%2/releases")
                                  .arg(m_owner, m_repo);
    QUrl url(urlString);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "VMTStudio-Updater");
    request.setRawHeader("Accept", "application/vnd.github.v3+json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    m_checkReply = m_net->get(request);
    connect(m_checkReply, &QNetworkReply::finished, this, &AppUpdater::onCheckFinished);
}

void AppUpdater::onCheckFinished() {
    if (!m_checkReply) {
        return;
    }

    if (m_checkReply->error() != QNetworkReply::NoError) {
        const QString err = m_checkReply->errorString();
        cleanupCheckReply();
        emit checkFailed(err, m_manualCheck);
        return;
    }

    const QByteArray data = m_checkReply->readAll();
    cleanupCheckReply();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (doc.isNull()) {
        emit checkFailed(tr("Error al analizar la respuesta del servidor (%1).").arg(parseError.errorString()), m_manualCheck);
        return;
    }

    if (doc.isObject()) {
        const QJsonObject obj = doc.object();
        if (obj.contains("message")) {
            emit checkFailed(tr("GitHub API: %1").arg(obj["message"].toString()), m_manualCheck);
            return;
        }
    }

    if (!doc.isArray()) {
        emit checkFailed(tr("Respuesta de GitHub inválida."), m_manualCheck);
        return;
    }

    const QJsonArray releasesArray = doc.array();
    ReleaseInfo latestRelease;
    AppVersion highestVersion;
    bool foundRelease = false;

    // Filter only official published releases (no drafts, no prereleases)
    for (const auto& item : releasesArray) {
        if (!item.isObject()) continue;
        const QJsonObject obj = item.toObject();

        const bool draft = obj["draft"].toBool(false);
        const bool prerelease = obj["prerelease"].toBool(false);
        if (draft || prerelease) {
            continue; // Only check official published releases
        }

        const QString tag = obj["tag_name"].toString().trimmed();
        const AppVersion parsedVer = AppVersion::parse(tag.toStdString());
        if (!parsedVer.is_valid()) {
            continue;
        }

        if (!foundRelease || parsedVer > highestVersion) {
            highestVersion = parsedVer;
            foundRelease = true;

            latestRelease.tagName = tag;
            latestRelease.name = obj["name"].toString();
            latestRelease.body = obj["body"].toString();
            latestRelease.htmlUrl = obj["html_url"].toString();
            latestRelease.publishedAt = QDateTime::fromString(obj["published_at"].toString(), Qt::ISODate);
            latestRelease.isDraft = draft;
            latestRelease.isPrerelease = prerelease;
            latestRelease.assets.clear();

            const QJsonArray assetsArray = obj["assets"].toArray();
            for (const auto& a : assetsArray) {
                if (!a.isObject()) continue;
                const QJsonObject aObj = a.toObject();
                ReleaseAsset asset;
                asset.name = aObj["name"].toString();
                asset.downloadUrl = aObj["browser_download_url"].toString();
                asset.size = aObj["size"].toVariant().toLongLong();
                if (!asset.downloadUrl.isEmpty()) {
                    latestRelease.assets.append(asset);
                }
            }
        }
    }

    if (!foundRelease) {
        // No official releases found in the repository
        emit upToDate(m_manualCheck);
        return;
    }

    const AppVersion currentVer = AppVersion::current();
    if (highestVersion > currentVer) {
        // A newer version is available.
        // If this is an automatic startup check and this version was ignored by the user, suppress notification.
        if (!m_manualCheck && !m_ignoredVersion.isEmpty()) {
            const AppVersion ignoredVer = AppVersion::parse(m_ignoredVersion.toStdString());
            if (latestRelease.tagName.compare(m_ignoredVersion, Qt::CaseInsensitive) == 0 ||
                (ignoredVer.is_valid() && latestRelease.version() == ignoredVer)) {
                // Ignored version: do not prompt on startup
                return;
            }
        }

        emit updateAvailable(latestRelease, m_manualCheck);
    } else {
        emit upToDate(m_manualCheck);
    }
}

void AppUpdater::startDownload(const ReleaseInfo& release) {
    cleanupDownloadReply();

    m_redirectCount = 0;
    m_currentRelease = release;
    const ReleaseAsset asset = release.bestAsset();
    if (asset.downloadUrl.isEmpty()) {
        emit downloadFailed(tr("No se encontró ningún archivo descargable en la release."));
        return;
    }

    const QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/VMTStudio_Update";
    QDir().mkpath(tempDir);

    m_downloadPath = tempDir + "/update.zip";
    if (QFile::exists(m_downloadPath)) {
        QFile::remove(m_downloadPath);
    }

    m_downloadFile = std::make_unique<QFile>(m_downloadPath);
    if (!m_downloadFile->open(QIODevice::WriteOnly)) {
        emit downloadFailed(tr("No se pudo crear el archivo temporal de descarga."));
        m_downloadFile.reset();
        return;
    }

    QUrl url(asset.downloadUrl);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "VMTStudio-Updater");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    m_downloadReply = m_net->get(request);
    connect(m_downloadReply, &QNetworkReply::readyRead, this, &AppUpdater::onDownloadDataReady);
    connect(m_downloadReply, &QNetworkReply::downloadProgress, this, &AppUpdater::downloadProgress);
    connect(m_downloadReply, &QNetworkReply::finished, this, &AppUpdater::onDownloadFinished);
}

void AppUpdater::cancelDownload() {
    cleanupDownloadReply();
}

void AppUpdater::onDownloadDataReady() {
    if (m_downloadReply && m_downloadFile && m_downloadFile->isOpen()) {
        m_downloadFile->write(m_downloadReply->readAll());
    }
}

void AppUpdater::onDownloadFinished() {
    if (!m_downloadReply) {
        return;
    }

    // Check for redirection manually if needed
    const QVariant redirectTarget = m_downloadReply->attribute(QNetworkRequest::RedirectionTargetAttribute);
    if (!redirectTarget.isNull()) {
        if (++m_redirectCount > 5) {
            cleanupDownloadReply();
            emit downloadFailed(tr("Demasiadas redirecciones durante la descarga."));
            return;
        }

        const QUrl redirectUrl = m_downloadReply->url().resolved(redirectTarget.toUrl());
        cleanupDownloadReply();

        m_downloadFile = std::make_unique<QFile>(m_downloadPath);
        if (!m_downloadFile->open(QIODevice::WriteOnly)) {
            emit downloadFailed(tr("No se pudo abrir el archivo para la redirección."));
            return;
        }

        QNetworkRequest req(redirectUrl);
        req.setHeader(QNetworkRequest::UserAgentHeader, "VMTStudio-Updater");
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

        m_downloadReply = m_net->get(req);
        connect(m_downloadReply, &QNetworkReply::readyRead, this, &AppUpdater::onDownloadDataReady);
        connect(m_downloadReply, &QNetworkReply::downloadProgress, this, &AppUpdater::downloadProgress);
        connect(m_downloadReply, &QNetworkReply::finished, this, &AppUpdater::onDownloadFinished);
        return;
    }

    if (m_downloadReply->error() != QNetworkReply::NoError) {
        const QString err = m_downloadReply->errorString();
        cleanupDownloadReply();
        emit downloadFailed(err);
        return;
    }

    if (m_downloadFile && m_downloadFile->isOpen()) {
        m_downloadFile->write(m_downloadReply->readAll());
        m_downloadFile->flush();
        if (m_downloadFile->error() != QFile::NoError) {
            cleanupDownloadReply();
            emit downloadFailed(tr("Error al escribir el archivo de actualización en disco."));
            return;
        }
        m_downloadFile->close();
    }
    m_downloadFile.reset();

    const QString savedPath = m_downloadPath;
    cleanupDownloadReply();

    // Verify minimum zip file size (empty zip EOCD record is 22 bytes)
    if (QFileInfo(savedPath).size() < 22) {
        emit downloadFailed(tr("El archivo descargado está incompleto o dañado."));
        return;
    }

    emit downloadFinished(savedPath);
}

bool AppUpdater::applyUpdate(const QString& zipFilePath, QString& errorMessage) {
    emit extractionStatus(tr("Extrayendo paquete de actualización..."));

    const QString tempDir = QFileInfo(zipFilePath).dir().absolutePath();
    const QString extractedDir = tempDir + "/extracted";

    // Clean up existing extracted directory
    if (QDir(extractedDir).exists()) {
        QDir(extractedDir).removeRecursively();
    }
    if (!QDir().mkpath(extractedDir)) {
        errorMessage = tr("No se pudo crear el directorio temporal de extracción.");
        return false;
    }

    // Try tar.exe first (standard on Windows 10 build 17063+ and Windows 11)
    bool extracted = false;
    QProcess tarProcess;
    const QString nativeZip = QDir::toNativeSeparators(zipFilePath);
    const QString nativeExtracted = QDir::toNativeSeparators(extractedDir);

    tarProcess.start("tar.exe", {"-xf", nativeZip, "-C", nativeExtracted});
    if (tarProcess.waitForFinished(60000) && tarProcess.exitCode() == 0) {
        extracted = true;
    }

    // Fallback to powershell Expand-Archive
    if (!extracted) {
        QProcess psProcess;
        QString escapedZip = nativeZip;
        escapedZip.replace("'", "''");
        QString escapedExtracted = nativeExtracted;
        escapedExtracted.replace("'", "''");
        const QString psCommand = QString("Expand-Archive -Force -LiteralPath '%1' -DestinationPath '%2'")
                                      .arg(escapedZip, escapedExtracted);
        psProcess.start("powershell.exe", {"-NoProfile", "-NonInteractive", "-Command", psCommand});
        if (psProcess.waitForFinished(120000) && psProcess.exitCode() == 0) {
            extracted = true;
        }
    }

    if (!extracted) {
        errorMessage = tr("No se pudo descomprimir el archivo de actualización.");
        return false;
    }

    // Locate directory containing vmtstudio.exe
    QString payloadDir;
    if (QFile::exists(extractedDir + "/vmtstudio.exe")) {
        payloadDir = extractedDir;
    } else {
        // Look into subdirectories (e.g. vmt-studio-v0.2.0-windows-x64)
        QDir extDir(extractedDir);
        const QStringList subDirs = extDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& sub : subDirs) {
            const QString candidate = extractedDir + "/" + sub;
            if (QFile::exists(candidate + "/vmtstudio.exe")) {
                payloadDir = candidate;
                break;
            }
        }
    }

    if (payloadDir.isEmpty()) {
        errorMessage = tr("El archivo descargado no contiene el ejecutable vmtstudio.exe.");
        return false;
    }

    emit extractionStatus(tr("Preparando script de actualización..."));

    // Write apply_update.cmd
    const QString scriptPath = tempDir + "/apply_update.cmd";
    QFile scriptFile(scriptPath);
    if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        errorMessage = tr("No se pudo crear el script de actualización.");
        return false;
    }

    {
        QTextStream out(&scriptFile);
        out << "@echo off\r\n";
        out << "chcp 65001 >nul\r\n";
        out << "setlocal enabledelayedexpansion\r\n";
        out << "title Actualizando VMT-Studio...\r\n";
        out << "\r\n";
        out << "set \"PID=%~1\"\r\n";
        out << "set \"SRC=%~2\"\r\n";
        out << "set \"DST=%~3\"\r\n";
        out << "set \"EXE=%~4\"\r\n";
        out << "\r\n";
        out << ":: Quitar barras invertidas finales para evitar escape accidental en robocopy\r\n";
        out << "if \"%SRC:~-1%\"==\"\\\" set \"SRC=%SRC:~0,-1%\"\r\n";
        out << "if \"%DST:~-1%\"==\"\\\" set \"DST=%DST:~0,-1%\"\r\n";
        out << "\r\n";
        out << "echo ========================================\r\n";
        out << "echo        Actualizando VMT-Studio...        \r\n";
        out << "echo ========================================\r\n";
        out << "echo Esperando a que el proceso %PID% finalice...\r\n";
        out << "\r\n";
        out << ":wait_loop\r\n";
        out << "tasklist /FI \"PID eq %PID%\" 2>nul | find /I \"%PID%\" >nul\r\n";
        out << "if not errorlevel 1 (\r\n";
        out << "    timeout /t 1 /nobreak >nul\r\n";
        out << "    goto wait_loop\r\n";
        out << ")\r\n";
        out << "\r\n";
        out << ":: Breve espera para liberar bloqueos residuales de archivos y DLLs\r\n";
        out << "timeout /t 1 /nobreak >nul\r\n";
        out << "\r\n";
        out << "echo Aplicando archivos actualizados...\r\n";
        out << "robocopy \"%SRC%\" \"%DST%\" /E /IS /IT /R:5 /W:1 > \"%TEMP%\\vmtstudio_update_robocopy.log\"\r\n";
        out << "set ROBO_ERR=%ERRORLEVEL%\r\n";
        out << "if %ROBO_ERR% GEQ 8 (\r\n";
        out << "    echo Error critico al copiar archivos (codigo %ROBO_ERR%).\r\n";
        out << "    echo Consulte %TEMP%\\vmtstudio_update_robocopy.log\r\n";
        out << "    pause\r\n";
        out << "    exit /b %ROBO_ERR%\r\n";
        out << ")\r\n";
        out << "\r\n";
        out << "echo Iniciando VMT-Studio actualizado...\r\n";
        out << "start \"\" \"%DST%\\%EXE%\"\r\n";
        out << "\r\n";
        out << ":: Limpiar archivos temporales\r\n";
        out << "timeout /t 2 /nobreak >nul\r\n";
        out << "rd /s /q \"%~dp0\" 2>nul\r\n";
        out << "exit /b 0\r\n";
    }
    scriptFile.close();

    const qint64 currentPid = QCoreApplication::applicationPid();
    const QString nativeSrc = QDir::toNativeSeparators(payloadDir);
    const QString nativeDst = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    const QString exeName = QFileInfo(QCoreApplication::applicationFilePath()).fileName();

    emit extractionStatus(tr("Reiniciando aplicación..."));

    // Using "call" prevents cmd.exe /c from stripping surrounding quotes on whitespace paths
    const bool launched = QProcess::startDetached("cmd.exe", {
        "/c",
        "call",
        QDir::toNativeSeparators(scriptPath),
        QString::number(currentPid),
        nativeSrc,
        nativeDst,
        exeName
    });

    if (!launched) {
        errorMessage = tr("No se pudo iniciar el proceso detached de actualización.");
        return false;
    }

    // Schedule application shutdown
    QTimer::singleShot(250, qApp, &QCoreApplication::quit);
    return true;
}

} // namespace qt
} // namespace ui
