#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QListWidget>
#include <QComboBox>

#include "ui/settings.h"
#include "ui/connection_model.h"
#include "profiles/game_manager.h"

namespace ui {
namespace qt {

class ConnectionDialog : public QDialog {
    Q_OBJECT

public:
    explicit ConnectionDialog(ui::UISettings& settings, QWidget* parent = nullptr);
    ~ConnectionDialog() override = default;

private slots:
    void onBrowseBin();
    void onBrowseGame();
    void onAutoDetectGame();
    void onDiscoverGames();
    void onGameSelected(int index);
    void populateGameCombo();
    void onTestConnection();
    void onSaveAndConnect();

private:
    ui::UISettings& m_settings;
    profiles::GameManager m_gameManager;

    QComboBox* m_gameCombo = nullptr;
    QPushButton* m_discoverBtn = nullptr;

    QLineEdit* m_binEdit = nullptr;
    QPushButton* m_binBrowseBtn = nullptr;

    QLineEdit* m_gameEdit = nullptr;
    QPushButton* m_gameBrowseBtn = nullptr;
    QPushButton* m_autoDetectBtn = nullptr;

    QPushButton* m_testBtn = nullptr;
    QLabel* m_archLabel = nullptr;
    QLabel* m_summaryLabel = nullptr;
    QListWidget* m_resultsList = nullptr;

    QPushButton* m_saveBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;
};

} // namespace qt
} // namespace ui
