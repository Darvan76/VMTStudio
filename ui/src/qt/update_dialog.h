#pragma once

#include <QDialog>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "app_updater.h"

namespace ui {
namespace qt {

class UpdateDialog : public QDialog {
    Q_OBJECT

public:
    enum class UserChoice {
        UpdateNow,
        Later,
        SkipThisVersion
    };

    explicit UpdateDialog(const ReleaseInfo& release, AppUpdater* updater, QWidget* parent = nullptr);
    ~UpdateDialog() override = default;

    UserChoice userChoice() const { return m_choice; }
    void reject() override;

signals:
    void userChoseUpdate();
    void userChoseLater();
    void userChoseSkip(const QString& versionTag);

private slots:
    void onUpdateClicked();
    void onLaterClicked();
    void onSkipClicked();
    void onCancelDownloadClicked();
    void onOpenInBrowserClicked();

    void onDownloadProgress(qint64 received, qint64 total);
    void onDownloadFinished(const QString& zipFilePath);
    void onDownloadFailed(const QString& error);
    void onExtractionStatus(const QString& status);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void setupUi();

    ReleaseInfo m_release;
    QPointer<AppUpdater> m_updater;
    UserChoice m_choice = UserChoice::Later;

    QLabel* m_titleLabel = nullptr;
    QLabel* m_versionLabel = nullptr;
    QLabel* m_dateLabel = nullptr;
    QTextBrowser* m_notesBrowser = nullptr;

    // Progress widgets
    QWidget* m_progressContainer = nullptr;
    QLabel* m_progressLabel = nullptr;
    QProgressBar* m_progressBar = nullptr;
    QPushButton* m_cancelDownloadBtn = nullptr;

    // Button bar
    QWidget* m_buttonContainer = nullptr;
    QPushButton* m_updateBtn = nullptr;
    QPushButton* m_laterBtn = nullptr;
    QPushButton* m_skipBtn = nullptr;
    QPushButton* m_browserBtn = nullptr;
};

} // namespace qt
} // namespace ui
