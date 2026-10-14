#pragma once

#include <QMainWindow>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QComboBox>
#include <QLabel>
#include <QTimer>
#include <QDockWidget>

#include "ui/session.h"
#include "ui/settings.h"
#include "ui/preview_options.h"
#include "ui/preview_sync.h"
#include "ui/validate.h"
#include "vmt/schema.h"
#include "bridge/engine_view.h"

namespace ui {
namespace qt {

class ViewportWidget;
class InspectorWidget;
class VmtHighlighter;
class NotificationPopup;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    bool openFile(const QString& filePath);
    bool openGameMaterial(const QString& materialName);

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void changeEvent(QEvent* event) override;

private slots:
    // File menu
    void onNewFile();
    void onOpenFile();
    void onSaveFile();
    void onSaveFileAs();
    void onSaveToGame();
    void onRevertFile();
    void onRecentFileTriggered();

    // Edit menu
    void onUndo();
    void onRedo();

    // View menu
    void onToggleShowAdvanced(bool show);
    void onResetLayout();

    // Preview menu / toolbar
    void onShapeSelected(int index);
    void onRenderModeSelected(int index);
    void onToggleAnimation();
    void onSetAnimationSpeed(float speed);
    void onOpenPreviewOptions();
    void onLoadModel();

    // Tools & Game
    void onOpenMaterialBrowser();
    void onValidateMaterial();
    void onToggleDebugConsole();
    void onOpenGameConnection();
    void onOpenAbout();

    // Widget interactions
    void onCodeEditorTextChanged();
    void onInspectorParameterChanged(const QString& name, const QString& value);
    void onInspectorShaderChanged(const QString& newShader);
    void onBrowseTextureRequested(const QString& paramName, const QString& currentValue);
    void onViewportResized(int width, int height);
    void onMaterialDropped(const QString& filePath);

    // Sync timers
    void onPreviewSyncTimeout();

private:
    void setupUi();
    void setupMenus();
    void setupToolBar();
    void setupStatusBar();
    void setupDock();

    void loadSettings();
    void saveSettings();
    void restoreLayout();
    void saveLayoutState();
    void updateRecentFilesMenu();
    void addRecentFile(const QString& path);

    void updateTitle();
    void updateStatus();
    bool maybeSave();

    bool ensureEngine(QString& error);
    void syncPreview();
    void refreshMaterialRoots();

    bool isCurrentMaterialBrushOnly() const;
    QString getCurrentShaderName() const;
    QString getMaterialDisplayName() const;
    void checkBrushMaterial(bool forcePopup);

    // Core Domain Objects
    ui::EditorSession m_session;
    vmt::SchemaLibrary m_schemas;
    ui::UISettings m_settings;
    std::filesystem::path m_settingsPath;

    std::unique_ptr<bridge::EngineView> m_engine;
    ui::PreviewOptions m_previewOptions;
    ui::PreviewSync m_previewSync;
    std::filesystem::path m_previewRoot;
    std::vector<std::filesystem::path> m_materialRoots;

    std::string m_currentRenderMode = "shaded";
    bool m_animPlaying = true;
    bool m_showAdvanced = false;

    // Brush shader tracking
    std::string m_lastShader;
    bool m_wasBrushMaterial = false;

    // UI Widgets
    InspectorWidget* m_inspector = nullptr;
    ViewportWidget* m_viewport = nullptr;
    QPlainTextEdit* m_codeEditor = nullptr;
    VmtHighlighter* m_highlighter = nullptr;
    QDockWidget* m_validationDock = nullptr;
    QTableWidget* m_validationTable = nullptr;
    NotificationPopup* m_notificationPopup = nullptr;

    QComboBox* m_shapeCombo = nullptr;
    QComboBox* m_renderModeCombo = nullptr;
    QAction* m_animAction = nullptr;

    QLabel* m_statusDoc = nullptr;
    QLabel* m_statusEngine = nullptr;
    QLabel* m_statusFindings = nullptr;

    QMenu* m_recentMenu = nullptr;

    QTimer* m_previewSyncTimer = nullptr;
    bool m_suppressSync = false;
};

} // namespace qt
} // namespace ui
