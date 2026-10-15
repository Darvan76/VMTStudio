#pragma once

#include <QAction>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QStringList>
#include <QTableWidget>
#include <QComboBox>
#include <QLabel>
#include <QTimer>
#include <QDockWidget>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

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
class ModelBrowserDialog;
class ModelMaterialsDock;
class AnimationDock;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    bool openFile(const QString& filePath);
    bool openGameMaterial(const QString& materialName);

    // Loads a model by game-relative name ("models/props_c17/oildrum001.mdl")
    // or by absolute disk path. Used by drag & drop, the recent-models menu and
    // the --model command line option. Defers the load while the engine is
    // still connecting.
    void openModel(const QString& modelPath);

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
    void onAnimationSpeedDown();
    void onAnimationSpeedUp();
    void onOpenPreviewOptions();
    void onLoadModel();

    // Model menu
    void onOpenModel();
    void onCloseModel();
    void onRecentModelTriggered();
    void onModelSelected(const QString& gameRelative, bool loose);

    // Model materials dock
    void onModelMaterialSelected(const QString& gameName);
    void onModelMaterialActivated(const QString& gameName);
    void onModelMaterialsExported(const QStringList& materials);
    void onIsolateToggled(const QString& gameName, bool enabled);
    void onExportModelMaterials();

    // Viewport model interaction
    void onModelDropped(const QString& filePath);
    void onPlayheadTick(int frame, qintptr cycle_x10000);

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
    void ensureEngineAsync();
    void syncPreview();
    void refreshMaterialRoots();

    // --- Model viewer -------------------------------------------------------
    void loadModelNow(const QString& modelPath);
    void queryModelSceneAsync();
    void onModelSceneLoaded(const std::shared_ptr<bridge::ModelScene>& scene);
    void onModelSceneFailed(const QString& error);
    void clearModelView();
    void updateModelActions();
    void updateRecentModelsMenu();
    void addRecentModel(const QString& path);
    // Game-relative path of a disk file below the configured game folder; empty
    // when the game folder is unknown or the file lives outside of it.
    QString gameRelativePath(const QString& diskPath) const;
    bool containsModelMaterial(const QString& gameName) const;

    // In-place editing of a material that belongs to the loaded model: opens it
    // in the VMT editor and previews it through the loose override at its own
    // game-relative path (no global forced preview).
    bool openModelMaterial(const QString& gameName, bool focusEditor);
    void saveModelMaterialOverride();
    void setAnimationSpeedScale(float speed);

    // Experimental isolate mode: transparent substitutes for every material of
    // the model except the kept one; every written file is tracked here.
    struct IsolateSubstitute {
        std::filesystem::path file;
        std::string material;
    };
    void applyIsolation(const QString& keepName);
    void cleanupIsolation(bool reload);

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
    bool m_engineConnecting = false;
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
    ui::PreviewOptions m_previewOptions;
    ui::PreviewSync m_previewSync;
    std::filesystem::path m_previewRoot;
    std::vector<std::filesystem::path> m_materialRoots;

    std::string m_currentRenderMode = "shaded";
    bool m_animPlaying = true;
    bool m_showAdvanced = false;

    // Model viewer state.
    std::shared_ptr<bridge::ModelScene> m_modelScene;
    QString m_currentModelName;   // game-relative name of the loaded model
    QString m_requestedModelPath; // path the model was requested with (game-relative preferred)
    QString m_pendingModelPath;   // load deferred until the engine is ready
    std::string m_currentModelMaterial; // model material open in the editor, empty when none
    quint64 m_modelSceneGeneration = 0; // drops stale scene queries after reload/close
    float m_animSpeedScale = 1.0f;
    std::vector<IsolateSubstitute> m_isolateSubstitutes;

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
    ModelMaterialsDock* m_materialsDock = nullptr;
    AnimationDock* m_animationDock = nullptr;

    QComboBox* m_shapeCombo = nullptr;
    QComboBox* m_renderModeCombo = nullptr;
    QAction* m_animAction = nullptr;
    QAction* m_materialsDockAction = nullptr;
    QAction* m_animationDockAction = nullptr;
    QAction* m_closeModelAction = nullptr;
    QAction* m_exportMaterialsAction = nullptr;

    QLabel* m_statusDoc = nullptr;
    QLabel* m_statusEngine = nullptr;
    QLabel* m_statusFindings = nullptr;

    QMenu* m_recentMenu = nullptr;
    QMenu* m_recentModelsMenu = nullptr;

    QTimer* m_previewSyncTimer = nullptr;
    bool m_suppressSync = false;
};

} // namespace qt
} // namespace ui
