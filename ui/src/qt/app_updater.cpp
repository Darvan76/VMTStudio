#include "app_updater.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
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
#include <QTemporaryDir>
#include <chrono>

namespace ui {
namespace qt {

AppUpdater::AppUpdater(QObject* parent)
    : QObject(parent),
      m_net(new QNetworkAccessManager(this)) {
    m_applyTimer = new QTimer(this);
    m_applyTimer->setInterval(25);
    connect(m_applyTimer, &QTimer::timeout, this, &AppUpdater::finishPreparation);
}

AppUpdater::~AppUpdater() {
    cleanupCheckReply();
    cleanupDownloadReply();
    if (m_cancelPreparation) m_cancelPreparation->store(true);
    if (m_preparation.valid()) m_preparation.wait();
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
    if (isPreparing()) {
        emit downloadFailed(tr("Espera a que termine la preparación anterior."));
        return;
    }
    cleanupDownloadReply();

    m_redirectCount = 0;
    m_currentRelease = release;
    const ReleaseAsset asset = release.bestAsset();
    if (asset.downloadUrl.isEmpty()) {
        emit downloadFailed(tr("No se encontró ningún archivo descargable en la release."));
        return;
    }

    // Every application instance owns its download and extraction files.
    m_downloadDirectory = std::make_unique<QTemporaryDir>(
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath("VMTStudio_Update-XXXXXX"));
    if (!m_downloadDirectory->isValid()) {
        emit downloadFailed(tr("No se pudo crear el directorio temporal de descarga."));
        return;
    }
    m_downloadPath = m_downloadDirectory->filePath("update.zip");

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
    if (m_cancelPreparation) m_cancelPreparation->store(true);
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
    const qint64 expectedSize = m_currentRelease.bestAsset().size;
    if (expectedSize > 0 && QFileInfo(savedPath).size() != expectedSize) {
        emit downloadFailed(tr("El tamaño del archivo descargado no coincide con la release."));
        return;
    }

    emit downloadFinished(savedPath);
}

void AppUpdater::applyUpdate(const QString& zipFilePath) {
    if (isPreparing()) return;
    m_updateZip = zipFilePath;
    m_cancelPreparation = std::make_shared<std::atomic<bool>>(false);
    const auto cancelled = m_cancelPreparation;
    try {
        m_preparation = std::async(std::launch::async, [this, zipFilePath, cancelled]() {
            PreparedUpdate result;
            prepareUpdate(zipFilePath, cancelled, result.payloadDir, result.stagingDir, result.error);
            return result;
        });
        m_applyTimer->start();
    } catch (const std::exception& error) {
        emit updateFailed(tr("No se pudo iniciar la preparación: %1").arg(QString::fromUtf8(error.what())));
    }
}

void AppUpdater::finishPreparation() {
    if (!m_preparation.valid() ||
        m_preparation.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    m_applyTimer->stop();
    PreparedUpdate result;
    try {
        result = m_preparation.get();
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    if (m_cancelPreparation->load()) {
        if (!result.stagingDir.isEmpty()) QDir(result.stagingDir).removeRecursively();
        emit updateCancelled();
        return;
    }
    if (!result.error.isEmpty()) {
        emit updateFailed(result.error);
        return;
    }
    QString error;
    if (!launchUpdate(result, error)) {
        QDir(result.stagingDir).removeRecursively();
        emit updateFailed(error);
    }
}

bool AppUpdater::prepareUpdate(const QString& zipFilePath,
                              const std::shared_ptr<std::atomic<bool>>& cancelled,
                              QString& payloadDir, QString& stagingDir, QString& errorMessage) {
    if (cancelled->load()) return false;
    emit extractionStatus(tr("Extrayendo paquete de actualización..."));
    if (cancelled->load()) return false;
    const QFileInfo archive(zipFilePath);
    if (!archive.isFile() || archive.size() < 22) {
        errorMessage = tr("El archivo de actualización no existe o está incompleto.");
        return false;
    }

    const QString tempDir = QFileInfo(zipFilePath).dir().absolutePath();
    QTemporaryDir extraction(tempDir + "/extracted-XXXXXX");
    if (!extraction.isValid()) {
        errorMessage = tr("No se pudo crear el directorio temporal de extracción.");
        return false;
    }
    // Failed/cancelled attempts clean their own unique directory in this
    // worker; another attempt's files and the installed app stay untouched.
    const QString extractedDir = extraction.path();

    // Try tar.exe first (standard on Windows 10 build 17063+ and Windows 11)
    bool extracted = false;
    const QString nativeZip = QDir::toNativeSeparators(zipFilePath);
    const QString nativeExtracted = QDir::toNativeSeparators(extractedDir);

    const auto extract = [&cancelled](const QString& program, const QStringList& arguments,
                                      int timeoutMs) {
        if (cancelled->load()) return false;
        QProcess process;
        process.start(program, arguments);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (!process.waitForFinished(25)) {
            if (cancelled->load() || std::chrono::steady_clock::now() >= deadline ||
                process.state() == QProcess::NotRunning) {
                process.kill();
                process.waitForFinished(1000);
                return false;
            }
        }
        return !cancelled->load() && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    };
    extracted = extract("tar.exe", {"-xf", nativeZip, "-C", nativeExtracted}, 60000);
    if (cancelled->load()) return false;

    // Fallback to powershell Expand-Archive
    if (!extracted) {
        QString escapedZip = nativeZip;
        escapedZip.replace("'", "''");
        QString escapedExtracted = nativeExtracted;
        escapedExtracted.replace("'", "''");
        const QString psCommand = QString("Expand-Archive -Force -LiteralPath '%1' -DestinationPath '%2'")
                                      .arg(escapedZip, escapedExtracted);
        extracted = extract("powershell.exe", {"-NoProfile", "-NonInteractive", "-Command", psCommand}, 120000);
    }
    if (cancelled->load()) return false;

    if (!extracted) {
        errorMessage = tr("No se pudo descomprimir el archivo de actualización.");
        return false;
    }

    // Zip Slip defense: ensure all extracted files stay strictly inside extractedDir
    const QString canonicalExtracted = QFileInfo(extractedDir).canonicalFilePath();
    if (!canonicalExtracted.isEmpty()) {
        QDirIterator it(extractedDir, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            if (cancelled->load()) return false;
            it.next();
            const QString canonicalItem = it.fileInfo().canonicalFilePath();
            if (!canonicalItem.isEmpty() &&
                !canonicalItem.startsWith(canonicalExtracted + "/", Qt::CaseInsensitive)) {
                errorMessage = tr("Error de seguridad: el archivo de actualización contiene rutas no seguras (Zip Slip).");
                return false;
            }
        }
    }

    // Locate directory containing vmtstudio.exe
    if (QFile::exists(extractedDir + "/vmtstudio.exe")) {
        payloadDir = extractedDir;
    } else {
        // Look into subdirectories (e.g. vmt-studio-v0.2.0-windows-x64)
        QDir extDir(extractedDir);
        const QStringList subDirs = extDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& sub : subDirs) {
            if (cancelled->load()) return false;
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
    if (cancelled->load()) return false;

    emit extractionStatus(tr("Preparando script de actualización..."));

    // PowerShell -File passes paths as arguments without cmd.exe CALL's
    // second expansion of percent signs or delayed expansion of exclamation marks.
    const QString scriptPath = extraction.filePath("apply_update.ps1");
    QFile scriptFile(scriptPath);
    if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        errorMessage = tr("No se pudo crear el script de actualización.");
        return false;
    }
    const QByteArray script = R"ps(param(
    [int]$AppProcessId,
    [string]$SourceDir,
    [string]$DestinationDir,
    [string]$ExecutableName,
    [string]$CleanupDir
)
$ErrorActionPreference = 'Stop'
try {
    Wait-Process -Id $AppProcessId -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    & robocopy.exe $SourceDir $DestinationDir /E /IS /IT /R:5 /W:1 /XF apply_update.ps1
    if ($LASTEXITCODE -ge 8) {
        throw "Robocopy failed with exit code $LASTEXITCODE. The update files remain in $SourceDir."
    }
    Start-Process -FilePath (Join-Path $DestinationDir $ExecutableName) -WorkingDirectory $DestinationDir
    Start-Sleep -Seconds 2
    Remove-Item -LiteralPath $CleanupDir -Recurse -Force
    exit 0
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
)ps";
    if (scriptFile.write(script) != script.size() || !scriptFile.flush()) {
        errorMessage = tr("No se pudo escribir el script de actualización.");
        return false;
    }
    scriptFile.close();

    if (cancelled->load()) return false;
    stagingDir = extraction.path();
    extraction.setAutoRemove(false); // the detached script still needs the payload
    return true;
}

bool AppUpdater::launchUpdate(const PreparedUpdate& update, QString& errorMessage) {
    const QString scriptPath = QDir(update.stagingDir).filePath("apply_update.ps1");
    const bool ownsArchive = m_downloadDirectory &&
        QFileInfo(m_updateZip).absolutePath() == m_downloadDirectory->path();
    const QString cleanupDir = ownsArchive ? m_downloadDirectory->path() : update.stagingDir;
    emit extractionStatus(tr("Reiniciando aplicación..."));

    const bool launched = QProcess::startDetached("powershell.exe", {
        "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
        QDir::toNativeSeparators(scriptPath),
        "-AppProcessId", QString::number(QCoreApplication::applicationPid()),
        "-SourceDir", QDir::toNativeSeparators(update.payloadDir),
        "-DestinationDir", QDir::toNativeSeparators(QCoreApplication::applicationDirPath()),
        "-ExecutableName", QFileInfo(QCoreApplication::applicationFilePath()).fileName(),
        "-CleanupDir", QDir::toNativeSeparators(cleanupDir)
    });

    if (!launched) {
        errorMessage = tr("No se pudo iniciar el proceso detached de actualización.");
        return false;
    }

    if (ownsArchive) m_downloadDirectory->setAutoRemove(false);

    // Schedule application shutdown
    QTimer::singleShot(250, qApp, &QCoreApplication::quit);
    return true;
}

} // namespace qt
} // namespace ui
