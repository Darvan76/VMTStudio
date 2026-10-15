#include "main_window.h"
#include "viewport_widget.h"
#include "inspector_widget.h"
#include "vmt_highlighter.h"
#include "preview_options_dialog.h"
#include "material_browser_dialog.h"
#include "model_browser_dialog.h"
#include "model_materials_dock.h"
#include "animation_dock.h"
#include "export_materials_dialog.h"
#include "connection_dialog.h"
#include "about_dialog.h"
#include "notification_popup.h"
#include "app_log.h"
#include "ui/animation_model.h"
#include "ui/shader_catalog.h"
#include "vmt/document.h"
#include <algorithm>
#include <cctype>
#include <iterator>
#include <fstream>
#include <thread>

#include <QMenuBar>
#include <QMenu>
#include <QToolBar>
#include <QStatusBar>
#include <QSplitter>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QCloseEvent>
#include <QHeaderView>
#include <QFontDatabase>
#include <QApplication>
#include <QIcon>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ui {
namespace qt {

namespace {

bridge::PreviewShape to_bridge_shape(ui::PreviewShape shape) {
    switch (shape) {
        case ui::PreviewShape::Sphere: return bridge::PreviewShape::Sphere;
        case ui::PreviewShape::Cube: return bridge::PreviewShape::Cube;
        case ui::PreviewShape::Cylinder: return bridge::PreviewShape::Cylinder;
        case ui::PreviewShape::Plane: return bridge::PreviewShape::Plane;
        case ui::PreviewShape::BrushWall: return bridge::PreviewShape::BrushWall;
        case ui::PreviewShape::BrushFloor: return bridge::PreviewShape::BrushFloor;
        case ui::PreviewShape::BrushCube: return bridge::PreviewShape::BrushCube;
        case ui::PreviewShape::BrushDisplacement: return bridge::PreviewShape::BrushDisplacement;
    }
    return bridge::PreviewShape::Sphere;
}

bridge::PreviewLighting to_bridge_lighting(const ui::PreviewLighting& lighting) {
    bridge::PreviewLighting out;
    for (int i = 0; i < 3; ++i) {
        out.ambient[i] = lighting.ambient[i];
        out.key_dir[i] = lighting.key_dir[i];
        out.key_color[i] = lighting.key_color[i];
        out.fill_dir[i] = lighting.fill_dir[i];
        out.fill_color[i] = lighting.fill_color[i];
        out.background[i] = lighting.background[i];
    }
    return out;
}

std::filesystem::path temp_preview_dir() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetTempPathW(MAX_PATH, buffer);
    if (length == 0 || length >= MAX_PATH) {
        return std::filesystem::temp_directory_path() / "VMTStudio" / "preview";
    }
    return std::filesystem::path(buffer) / L"VMTStudio" / L"preview";
}

std::filesystem::path app_data_dir() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return std::filesystem::temp_directory_path() / "VMTStudio";
    }
    return std::filesystem::path(buffer) / L"VMTStudio";
}

// <preview root>/materials/<game-relative material name>.vmt: the loose override
// the engine resolves first for a model material. Empty when the name cannot be
// used as a relative path (absolute segments, "..", drive letters).
std::filesystem::path preview_override_path(const std::filesystem::path& preview_root,
                                            const std::string& material_name) {
    if (preview_root.empty() || material_name.empty()) {
        return {};
    }
    std::filesystem::path target = preview_root / L"materials";
    std::string component;
    for (std::size_t i = 0; i <= material_name.size(); ++i) {
        const char c = i < material_name.size() ? material_name[i] : '/';
        if (c == '/' || c == '\\') {
            if (component.empty() || component == ".") {
                // skip empty / current-directory components
            } else if (component == "..") {
                return {};
            } else {
                target /= std::filesystem::path(component);
            }
            component.clear();
        } else if (c == ':') {
            return {};
        } else {
            component += c;
        }
    }
    target += L".vmt";
    return target;
}

// The transparent stand-in the experimental isolate mode writes for every
// material of the model except the kept one.
constexpr const char* kTransparentVmt =
    "\"UnlitGeneric\"\n"
    "{\n"
    "    \"$basetexture\" \"vgui/white\"\n"
    "    \"$translucent\" \"1\"\n"
    "    \"$alpha\" \"0\"\n"
    "}\n";

// Case-insensitive equality for game-relative material names (the engine file
// system is case-insensitive).
bool material_name_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    return std::equal(a.begin(), a.end(), b.begin(), [](char ca, char cb) {
        return std::tolower(static_cast<unsigned char>(ca)) ==
               std::tolower(static_cast<unsigned char>(cb));
    });
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(tr("VMT-Studio"));
    setWindowIcon(QIcon(":/icons/app.png"));
    resize(1360, 820);

    m_previewRoot = temp_preview_dir();
    m_settingsPath = app_data_dir() / "settings.json";

    loadSettings();

    // Load shader schemas
#ifdef VMT_SCHEMA_DIR
    auto library = vmt::SchemaLibrary::load_dir(std::filesystem::u8path(VMT_SCHEMA_DIR));
    if (library) {
        m_schemas = std::move(library.value());
        log_write("APP ", "Loaded " + std::to_string(m_schemas.shader_names().size()) + " shader schemas");
    } else {
        log_write("WARN", "Failed to load schemas: " + library.error().message);
    }
#endif

    // Setup preview sync timer
    m_previewSyncTimer = new QTimer(this);
    m_previewSyncTimer->setSingleShot(true);
    m_previewSyncTimer->setInterval(200);
    connect(m_previewSyncTimer, &QTimer::timeout, this, &MainWindow::onPreviewSyncTimeout);

    setupUi();

    // Populate the inspector's shader dropdown with the full catalog (known
    // shaders plus every shader the loaded schemas cover) so the user can pick
    // one instead of typing it by hand.
    {
        const auto names = ui::shader_picker_names(&m_schemas);
        QStringList shaderNames;
        shaderNames.reserve(static_cast<int>(names.size()));
        for (const std::string& name : names) {
            shaderNames.append(QString::fromStdString(name));
        }
        m_inspector->setAvailableShaders(shaderNames);
    }

    m_notificationPopup = new NotificationPopup(this);
    setupMenus();
    setupToolBar();
    setupStatusBar();
    setupDock();

    restoreLayout();

    // Default template document
    onNewFile();

    // Asynchronously connect to the engine preview in the background so the
    // window appears immediately without blocking startup.
    if (!m_settings.last_bin_dir.empty() && !m_settings.last_game_dir.empty()) {
        ensureEngineAsync();
    }

    refreshMaterialRoots();
}

MainWindow::~MainWindow() {
    *m_alive = false;
    saveSettings();
    // The animation dock must never outlive the engine it talks to.
    if (m_animationDock) {
        m_animationDock->setEngine(nullptr);
    }
    // Remove the experimental isolate substitutes from the preview root.
    cleanupIsolation(false);
    if (m_engine) {
        m_engine->shutdown();
        m_engine.reset();
    }
}

void MainWindow::setupUi() {
    auto* mainSplitter = new QSplitter(Qt::Horizontal, this);
    mainSplitter->setHandleWidth(4);

    // 1. Parameter Inspector (Left)
    m_inspector = new InspectorWidget(mainSplitter);
    m_inspector->setMinimumWidth(280);
    mainSplitter->addWidget(m_inspector);

    // 2. Viewport Widget (Center)
    m_viewport = new ViewportWidget(mainSplitter);
    m_viewport->setMinimumWidth(400);
    m_viewport->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    mainSplitter->addWidget(m_viewport);

    // 3. VMT Code Editor (Right)
    m_codeEditor = new QPlainTextEdit(mainSplitter);
    m_codeEditor->setMinimumWidth(320);

    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPointSize(10);
    m_codeEditor->setFont(monoFont);
    m_codeEditor->setTabStopDistance(24);

    m_highlighter = new VmtHighlighter(m_codeEditor->document());
    mainSplitter->addWidget(m_codeEditor);

    // Set proportional initial split
    mainSplitter->setSizes({340, 560, 460});
    setCentralWidget(mainSplitter);

    // Connect inspector signals
    connect(m_inspector, &InspectorWidget::parameterChanged,
            this, &MainWindow::onInspectorParameterChanged);
    connect(m_inspector, &InspectorWidget::shaderChanged,
            this, &MainWindow::onInspectorShaderChanged);
    connect(m_inspector, &InspectorWidget::showAdvancedToggled,
            this, &MainWindow::onToggleShowAdvanced);
    connect(m_inspector, &InspectorWidget::browseTextureRequested,
            this, &MainWindow::onBrowseTextureRequested);

    // Connect viewport signals
    connect(m_viewport, &ViewportWidget::viewportResized,
            this, &MainWindow::onViewportResized);
    connect(m_viewport, &ViewportWidget::fileDropped,
            this, &MainWindow::onMaterialDropped);
    connect(m_viewport, &ViewportWidget::modelDropped,
            this, &MainWindow::onModelDropped);
    connect(m_viewport, &ViewportWidget::playheadTick,
            this, &MainWindow::onPlayheadTick);

    // Connect code editor signals
    connect(m_codeEditor, &QPlainTextEdit::textChanged,
            this, &MainWindow::onCodeEditorTextChanged);

    // Make validation findings actionable: double click jumps to the source line.
    connect(m_validationTable, &QTableWidget::cellDoubleClicked,
            this, [this](int row, int /*column*/) {
        if (row < 0 || row >= m_validationTable->rowCount()) {
            return;
        }
        auto* locationItem = m_validationTable->item(row, 3);
        if (!locationItem) {
            return;
        }
        const QString text = locationItem->text();
        int line = 0;
        for (const QChar& ch : text) {
            if (ch.isDigit()) {
                line = line * 10 + ch.digitValue();
            } else if (line > 0) {
                break;
            }
        }
        if (line <= 0) {
            return;
        }

        QTextCursor cursor = m_codeEditor->textCursor();
        cursor.movePosition(QTextCursor::Start);
        for (int i = 1; i < line; ++i) {
            if (!cursor.movePosition(QTextCursor::Down)) {
                break;
            }
        }
        m_codeEditor->setTextCursor(cursor);
        m_codeEditor->setFocus();
    });
}

void MainWindow::setupMenus() {
    // --- File Menu ---
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&New Material"), QKeySequence::New, this, &MainWindow::onNewFile);
    fileMenu->addAction(tr("&Open Material..."), QKeySequence::Open, this, &MainWindow::onOpenFile);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::onSaveFile);
    fileMenu->addAction(tr("Save &As..."), QKeySequence::SaveAs, this, &MainWindow::onSaveFileAs);
    fileMenu->addAction(tr("Save to &Game Folder"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_S), this, &MainWindow::onSaveToGame);
    fileMenu->addAction(tr("&Revert to Saved"), QKeySequence(Qt::CTRL | Qt::Key_R), this, &MainWindow::onRevertFile);
    fileMenu->addSeparator();

    m_recentMenu = fileMenu->addMenu(tr("Recent &Files"));
    updateRecentFilesMenu();

    fileMenu->addSeparator();
    fileMenu->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    // --- Edit Menu ---
    auto* editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(tr("&Undo"), QKeySequence::Undo, this, &MainWindow::onUndo);
    editMenu->addAction(tr("&Redo"), QKeySequence::Redo, this, &MainWindow::onRedo);

    // --- View Menu ---
    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    auto* advAction = viewMenu->addAction(tr("Show &Advanced Parameters"), [this]() {
        onToggleShowAdvanced(!m_showAdvanced);
    });
    advAction->setCheckable(true);
    advAction->setChecked(m_showAdvanced);

    viewMenu->addAction(tr("&Reset Layout"), this, &MainWindow::onResetLayout);
    viewMenu->addSeparator();

    m_materialsDockAction = viewMenu->addAction(tr("&Materials Dock"));
    m_materialsDockAction->setCheckable(true);
    m_materialsDockAction->setChecked(true);

    m_animationDockAction = viewMenu->addAction(tr("&Animation Dock"));
    m_animationDockAction->setCheckable(true);
    m_animationDockAction->setChecked(false);

    // --- Preview Menu ---
    auto* previewMenu = menuBar()->addMenu(tr("&Preview"));
    auto* shapeMenu = previewMenu->addMenu(tr("&Shape"));
    const QStringList shapes = {
        tr("Sphere"), tr("Cube"), tr("Cylinder"), tr("Plane"),
        tr("Brush Wall (128x128)"), tr("Brush Floor (256x256)"),
        tr("Brush Cube"), tr("Brush Displacement")
    };
    for (int i = 0; i < shapes.size(); ++i) {
        shapeMenu->addAction(shapes[i], [this, i]() { onShapeSelected(i); });
    }

    auto* modeMenu = previewMenu->addMenu(tr("&Render Mode"));
    modeMenu->addAction(tr("Shaded (Default)"), [this]() { onRenderModeSelected(0); });
    modeMenu->addAction(tr("Wireframe"), [this]() { onRenderModeSelected(1); });
    modeMenu->addAction(tr("Albedo (No Lighting)"), [this]() { onRenderModeSelected(2); });
    modeMenu->addAction(tr("Normals"), [this]() { onRenderModeSelected(3); });

    auto* animMenu = previewMenu->addMenu(tr("&Animation"));
    m_animAction = animMenu->addAction(tr("Play / Pause"), QKeySequence(Qt::Key_Space), this, &MainWindow::onToggleAnimation);
    animMenu->addAction(tr("Halve Playback Speed"), QKeySequence(Qt::Key_BracketLeft), this, &MainWindow::onAnimationSpeedDown);
    animMenu->addAction(tr("Double Playback Speed"), QKeySequence(Qt::Key_BracketRight), this, &MainWindow::onAnimationSpeedUp);
    animMenu->addAction(tr("0.5x Speed"), [this]() { onSetAnimationSpeed(0.5f); });
    animMenu->addAction(tr("1.0x Normal Speed"), [this]() { onSetAnimationSpeed(1.0f); });
    animMenu->addAction(tr("2.0x Speed"), [this]() { onSetAnimationSpeed(2.0f); });

    previewMenu->addSeparator();
    previewMenu->addAction(tr("Preview &Options..."), QKeySequence(Qt::CTRL | Qt::Key_P), this, &MainWindow::onOpenPreviewOptions);
    previewMenu->addAction(tr("Load Custom &Model (.mdl)..."), QKeySequence(Qt::CTRL | Qt::Key_M), this, &MainWindow::onLoadModel);

    // --- Model Menu ---
    auto* modelMenu = menuBar()->addMenu(tr("&Model"));
    modelMenu->addAction(tr("&Open Model..."), QKeySequence(Qt::Key_F4), this, &MainWindow::onOpenModel);
    m_closeModelAction = modelMenu->addAction(tr("&Close Model"), this, &MainWindow::onCloseModel);
    m_closeModelAction->setEnabled(false);
    m_recentModelsMenu = modelMenu->addMenu(tr("Recent &Models"));
    updateRecentModelsMenu();

    // --- Tools Menu ---
    auto* toolsMenu = menuBar()->addMenu(tr("&Tools"));
    toolsMenu->addAction(tr("Material &Browser..."), QKeySequence(Qt::Key_F3), this, &MainWindow::onOpenMaterialBrowser);
    toolsMenu->addAction(tr("&Validate Material"), QKeySequence(Qt::Key_F7), this, &MainWindow::onValidateMaterial);
    toolsMenu->addSeparator();
    m_exportMaterialsAction = toolsMenu->addAction(tr("&Export Model Materials..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), this, &MainWindow::onExportModelMaterials);
    m_exportMaterialsAction->setEnabled(false);
    toolsMenu->addSeparator();
    toolsMenu->addAction(tr("Toggle &Debug Console"), QKeySequence(Qt::Key_F12), this, &MainWindow::onToggleDebugConsole);

    // --- Game Menu ---
    auto* gameMenu = menuBar()->addMenu(tr("&Game"));
    gameMenu->addAction(tr("&Connection Settings..."), QKeySequence(Qt::Key_F2), this, &MainWindow::onOpenGameConnection);

    // --- Help Menu ---
    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About VMT-Studio..."), QKeySequence(Qt::Key_F1), this, &MainWindow::onOpenAbout);
}

void MainWindow::setupToolBar() {
    auto* toolBar = addToolBar(tr("Main Toolbar"));
    toolBar->setMovable(false);

    toolBar->addAction(style()->standardIcon(QStyle::SP_DialogOpenButton), tr("Open"), this, &MainWindow::onOpenFile);
    toolBar->addAction(style()->standardIcon(QStyle::SP_DialogSaveButton), tr("Save"), this, &MainWindow::onSaveFile);
    toolBar->addSeparator();

    toolBar->addAction(tr("Validate"), this, &MainWindow::onValidateMaterial);
    toolBar->addAction(tr("Browser (F3)"), this, &MainWindow::onOpenMaterialBrowser);
    toolBar->addAction(tr("Options"), this, &MainWindow::onOpenPreviewOptions);
    toolBar->addSeparator();

    // Shape selector combo
    m_shapeCombo = new QComboBox(toolBar);
    m_shapeCombo->addItems({
        tr("Sphere"), tr("Cube"), tr("Cylinder"), tr("Plane"),
        tr("Brush Wall"), tr("Brush Floor"), tr("Brush Cube"), tr("Brush Displacement")
    });
    connect(m_shapeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onShapeSelected);
    toolBar->addWidget(new QLabel(tr(" Shape: "), toolBar));
    toolBar->addWidget(m_shapeCombo);

    // Render mode combo
    m_renderModeCombo = new QComboBox(toolBar);
    m_renderModeCombo->addItems({tr("Shaded"), tr("Wireframe"), tr("Albedo"), tr("Normals")});
    connect(m_renderModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onRenderModeSelected);
    toolBar->addWidget(new QLabel(tr(" Mode: "), toolBar));
    toolBar->addWidget(m_renderModeCombo);
}

void MainWindow::setupStatusBar() {
    m_statusDoc = new QLabel(this);
    m_statusEngine = new QLabel(this);
    m_statusFindings = new QLabel(this);

    statusBar()->addPermanentWidget(m_statusDoc, 2);
    statusBar()->addPermanentWidget(m_statusEngine, 1);
    statusBar()->addPermanentWidget(m_statusFindings, 1);

    updateStatus();
}

void MainWindow::setupDock() {
    m_validationDock = new QDockWidget(tr("Validation Findings"), this);
    m_validationDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);

    m_validationTable = new QTableWidget(m_validationDock);
    m_validationTable->setColumnCount(4);
    m_validationTable->setHorizontalHeaderLabels({tr("Severity"), tr("Parameter"), tr("Message"), tr("Suggestion")});
    m_validationTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_validationTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_validationTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_validationTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_validationTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_validationTable->setSelectionBehavior(QAbstractItemView::SelectRows);

    m_validationDock->setWidget(m_validationTable);
    addDockWidget(Qt::BottomDockWidgetArea, m_validationDock);
    m_validationDock->hide(); // Hidden by default until validation is run

    // --- Model viewer docks -------------------------------------------------
    m_materialsDock = new ModelMaterialsDock(this);
    addDockWidget(Qt::LeftDockWidgetArea, m_materialsDock);
    m_materialsDock->setEnabled(false); // enabled only while a model is loaded
    connect(m_materialsDock, &ModelMaterialsDock::materialSelected,
            this, &MainWindow::onModelMaterialSelected);
    connect(m_materialsDock, &ModelMaterialsDock::materialActivated,
            this, &MainWindow::onModelMaterialActivated);
    connect(m_materialsDock, &ModelMaterialsDock::materialExportRequested,
            this, &MainWindow::onModelMaterialsExported);
    connect(m_materialsDock, &ModelMaterialsDock::isolateToggled,
            this, &MainWindow::onIsolateToggled);

    m_animationDock = new AnimationDock(this);
    addDockWidget(Qt::RightDockWidgetArea, m_animationDock);
    m_animationDock->setEngine(m_engine.get());
    connect(m_animationDock, &AnimationDock::statusMessage, this, [this](const QString& text) {
        statusBar()->showMessage(text, 4000);
    });
    m_animationDock->hide(); // hidden until a model is loaded

    // Keep the View menu toggles and the dock visibility in sync both ways.
    connect(m_materialsDockAction, &QAction::toggled, m_materialsDock, &QWidget::setVisible);
    connect(m_materialsDock, &QDockWidget::visibilityChanged, m_materialsDockAction, &QAction::setChecked);
    connect(m_animationDockAction, &QAction::toggled, m_animationDock, &QWidget::setVisible);
    connect(m_animationDock, &QDockWidget::visibilityChanged, m_animationDockAction, &QAction::setChecked);

    updateModelActions();
}

void MainWindow::loadSettings() {
    m_settings = ui::UISettings::load(m_settingsPath);
    m_previewOptions = ui::options_from_settings(m_settings);
    m_showAdvanced = m_settings.show_advanced;
}

void MainWindow::saveSettings() {
    m_settings.show_advanced = m_showAdvanced;
    saveLayoutState();
    ui::options_to_settings(m_previewOptions, m_settings);
    (void)m_settings.save(m_settingsPath);
}

void MainWindow::restoreLayout() {
    if (m_settings.window_width > 0 && m_settings.window_height > 0) {
        resize(m_settings.window_width, m_settings.window_height);
    }

    if (auto* splitter = qobject_cast<QSplitter*>(centralWidget())) {
        if (!m_settings.splitter_sizes.empty()) {
            std::vector<int> sizes;
            std::string token;
            for (char ch : m_settings.splitter_sizes) {
                if (ch == ',') {
                    try { sizes.push_back(std::stoi(token)); } catch (...) { }
                    token.clear();
                } else {
                    token += ch;
                }
            }
            if (!token.empty()) {
                try { sizes.push_back(std::stoi(token)); } catch (...) { }
            }
            if (sizes.size() == static_cast<std::size_t>(splitter->count())) {
                splitter->setSizes(QList<int>(sizes.begin(), sizes.end()));
            }
        }
    }

    if (m_validationDock) {
        m_validationDock->setVisible(m_settings.validation_dock_visible);
    }
}

void MainWindow::saveLayoutState() {
    m_settings.window_width = width();
    m_settings.window_height = height();

    if (auto* splitter = qobject_cast<QSplitter*>(centralWidget())) {
        std::string joined;
        const QList<int> sizes = splitter->sizes();
        for (int i = 0; i < sizes.size(); ++i) {
            if (i != 0) {
                joined += ",";
            }
            joined += std::to_string(sizes[i]);
        }
        m_settings.splitter_sizes = joined;
    }

    if (m_validationDock) {
        m_settings.validation_dock_visible = m_validationDock->isVisible();
    }
}

void MainWindow::updateRecentFilesMenu() {
    m_recentMenu->clear();
    if (m_settings.recent_files.empty()) {
        auto* action = m_recentMenu->addAction(tr("(No recent files)"));
        action->setEnabled(false);
        return;
    }
    for (const auto& pathStr : m_settings.recent_files) {
        QString qPath = QString::fromStdString(pathStr);
        m_recentMenu->addAction(qPath, this, &MainWindow::onRecentFileTriggered);
    }
}

void MainWindow::addRecentFile(const QString& path) {
    std::string p = path.toStdString();
    auto& rec = m_settings.recent_files;
    rec.erase(std::remove(rec.begin(), rec.end(), p), rec.end());
    rec.insert(rec.begin(), p);
    if (rec.size() > 10) {
        rec.resize(10);
    }
    updateRecentFilesMenu();
    saveSettings();
}

void MainWindow::updateTitle() {
    if (!m_currentModelName.isEmpty()) {
        QString title = tr("VMT-Studio - %1").arg(m_currentModelName);
        if (m_session.dirty()) {
            title += " *";
        }
        setWindowTitle(title);
        return;
    }

    QString title = tr("VMT-Studio");
    if (!m_session.path().empty()) {
        title = QString::fromStdWString(m_session.path().filename().wstring()) + " - " + title;
    } else {
        title = tr("untitled.vmt* - ") + title;
    }
    if (m_session.dirty()) {
        title += " *";
    }
    setWindowTitle(title);
}

void MainWindow::updateStatus() {
    if (m_session.path().empty()) {
        m_statusDoc->setText(tr("Document: untitled.vmt (Unsaved)"));
    } else {
        m_statusDoc->setText(tr("Document: %1 %2")
            .arg(QString::fromStdWString(m_session.path().wstring()))
            .arg(m_session.dirty() ? tr("[Modified]") : tr("[Saved]")));
    }

    if (m_engine && m_engine->initialized()) {
        m_statusEngine->setText(tr("Engine: Connected (HLMV 32-bit)"));
        m_statusEngine->setStyleSheet("color: #4ec9b0;");
    } else if (m_engineConnecting) {
        m_statusEngine->setText(tr("Engine: Connecting..."));
        m_statusEngine->setStyleSheet("color: #dcdcaa;");
    } else {
        m_statusEngine->setText(tr("Engine: Idle"));
        m_statusEngine->setStyleSheet("color: #dcdcaa;");
    }
}

bool MainWindow::ensureEngine(QString& error) {
    if (m_engine && m_engine->initialized()) {
        return true;
    }

    if (m_engineConnecting) {
        error = tr("Engine is currently connecting in the background. Please wait a moment.");
        return false;
    }

    if (m_settings.last_bin_dir.empty() || m_settings.last_game_dir.empty()) {
        error = tr("Game /bin or mod folder not configured. Use Game > Connection Settings.");
        return false;
    }

    if (!m_engine) {
        auto createRes = bridge::EngineView::create();
        if (!createRes) {
            error = QString::fromStdString(createRes.error().message);
            return false;
        }
        m_engine = std::move(createRes.value());
    }

    bridge::EngineViewConfig config;
    config.bin_dir = m_settings.last_bin_dir;
    config.game_dir = m_settings.last_game_dir;
    config.child_window = reinterpret_cast<void*>(m_viewport->hostHwnd());
    config.width = std::max(64, m_viewport->width());
    config.height = std::max(64, m_viewport->height());

    auto initRes = m_engine->initialize(config);
    if (!initRes) {
        error = QString::fromStdString(initRes.error().message);
        m_viewport->setStatusText(tr("Engine Initialization Error:\n%1").arg(error));
        return false;
    }

    // Mount temporary preview root
    std::error_code ec;
    std::filesystem::create_directories(m_previewRoot / "materials", ec);
    (void)m_engine->mount_directory(m_previewRoot, true);

    // Mount current material root so the engine can resolve textures (.vtf)
    if (!m_session.material_root().empty()) {
        (void)m_engine->mount_directory(m_session.material_root(), true);
    }

    // Apply lighting and shapes
    (void)m_engine->set_preview_shape(to_bridge_shape(m_previewOptions.shape));
    (void)m_engine->set_lighting(to_bridge_lighting(m_previewOptions.lighting));
    (void)m_engine->set_render_mode(m_currentRenderMode);

    m_viewport->setEngineView(m_engine.get());
    if (m_animationDock) {
        m_animationDock->setEngine(m_engine.get());
    }
    (void)m_engine->resize(0, 0);
    updateStatus();
    return true;
}

void MainWindow::ensureEngineAsync() {
    if (m_engineConnecting) {
        return;
    }
    if (m_engine && m_engine->initialized()) {
        return;
    }
    if (m_settings.last_bin_dir.empty() || m_settings.last_game_dir.empty()) {
        return;
    }

    m_engineConnecting = true;
    updateStatus();
    m_viewport->setStatusText(tr("Source Engine Preview\nConnecting to HLMV backend in background..."));

    bridge::EngineViewConfig config;
    config.bin_dir = m_settings.last_bin_dir;
    config.game_dir = m_settings.last_game_dir;
    config.child_window = reinterpret_cast<void*>(m_viewport->hostHwnd());
    config.width = std::max(64, m_viewport->width());
    config.height = std::max(64, m_viewport->height());

    auto alive = m_alive;
    std::filesystem::path previewRoot = m_previewRoot;
    std::filesystem::path materialRoot = m_session.material_root();
    ui::PreviewOptions previewOptions = m_previewOptions;
    std::string renderMode = m_currentRenderMode;

    std::thread([this, config, alive, previewRoot, materialRoot, previewOptions, renderMode]() {
        auto createRes = bridge::EngineView::create();
        if (!createRes) {
            std::string errMsg = createRes.error().message;
            QMetaObject::invokeMethod(qApp, [this, alive, errMsg]() {
                if (!*alive) return;
                m_engineConnecting = false;
                m_viewport->setStatusText(tr("Engine Initialization Error:\n%1").arg(QString::fromStdString(errMsg)));
                updateStatus();
            }, Qt::QueuedConnection);
            return;
        }

        auto engine = std::move(createRes.value());
        auto initRes = engine->initialize(config);
        if (!initRes) {
            std::string errMsg = initRes.error().message;
            QMetaObject::invokeMethod(qApp, [this, alive, errMsg]() {
                if (!*alive) return;
                m_engineConnecting = false;
                m_viewport->setStatusText(tr("Engine Initialization Error:\n%1").arg(QString::fromStdString(errMsg)));
                updateStatus();
            }, Qt::QueuedConnection);
            return;
        }

        // Mount temporary preview root
        std::error_code ec;
        std::filesystem::create_directories(previewRoot / "materials", ec);
        (void)engine->mount_directory(previewRoot, true);

        // Mount material root if available
        if (!materialRoot.empty()) {
            (void)engine->mount_directory(materialRoot, true);
        }

        auto rawEngine = engine.release();
        QMetaObject::invokeMethod(qApp, [this, alive, rawEngine, previewOptions, renderMode]() {
            if (!*alive) {
                delete rawEngine;
                return;
            }

            m_engine.reset(rawEngine);
            m_engineConnecting = false;

            if (!m_session.material_root().empty()) {
                (void)m_engine->mount_directory(m_session.material_root(), true);
            }
            checkBrushMaterial(false);

            // Apply lighting and shapes
            (void)m_engine->set_preview_shape(to_bridge_shape(m_previewOptions.shape));
            (void)m_engine->set_lighting(to_bridge_lighting(previewOptions.lighting));
            (void)m_engine->set_render_mode(renderMode);

            m_viewport->setEngineView(m_engine.get());
            if (m_animationDock) {
                m_animationDock->setEngine(m_engine.get());
            }
            (void)m_engine->resize(0, 0);
            updateStatus();

            // A model requested while the engine was starting owns the
            // viewport; the flat material preview must not override it.
            if (!m_pendingModelPath.isEmpty()) {
                const QString pending = m_pendingModelPath;
                m_pendingModelPath.clear();
                loadModelNow(pending);
            } else {
                syncPreview();
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void MainWindow::refreshMaterialRoots() {
    m_materialRoots.clear();
    if (!m_settings.last_game_dir.empty()) {
        m_materialRoots.push_back(std::filesystem::path(m_settings.last_game_dir));
    }
    if (!m_previewRoot.empty()) {
        m_materialRoots.push_back(m_previewRoot);
    }
    if (!m_session.material_root().empty()) {
        m_materialRoots.push_back(m_session.material_root());
    }
}

bool MainWindow::isCurrentMaterialBrushOnly() const {
    if (!m_session.document()) {
        return false;
    }
    return ui::is_brush_only_shader(m_session.document()->shader());
}

QString MainWindow::getCurrentShaderName() const {
    if (!m_session.document()) {
        return QStringLiteral("LightmappedGeneric");
    }
    return QString::fromUtf8(m_session.document()->shader().data(),
                             static_cast<int>(m_session.document()->shader().size()));
}

QString MainWindow::getMaterialDisplayName() const {
    if (!m_session.path().empty()) {
        return QString::fromStdWString(m_session.path().stem().wstring());
    }
    if (!m_session.display_name().empty()) {
        std::filesystem::path p(m_session.display_name());
        return QString::fromStdWString(p.stem().wstring());
    }
    return QStringLiteral("untitled");
}

void MainWindow::checkBrushMaterial(bool forcePopup) {
    if (!m_session.document()) {
        m_wasBrushMaterial = false;
        m_lastShader.clear();
        return;
    }

    std::string currentShader = std::string(m_session.document()->shader());
    bool isBrush = ui::is_brush_only_shader(currentShader);

    if (isBrush) {
        bool shouldNotify = forcePopup || !m_wasBrushMaterial || !ui::shader_name_equal(currentShader, m_lastShader);

        // Auto-change viewport shape to BrushWall if not already a brush shape or freshly loaded
        if (!ui::is_brush_shape(m_previewOptions.shape) || shouldNotify) {
            m_previewOptions.shape = ui::PreviewShape::BrushWall;
            if (m_shapeCombo) {
                m_shapeCombo->blockSignals(true);
                m_shapeCombo->setCurrentIndex(static_cast<int>(ui::PreviewShape::BrushWall));
                m_shapeCombo->blockSignals(false);
            }
            if (m_engine && m_engine->initialized()) {
                (void)m_engine->set_preview_shape(to_bridge_shape(ui::PreviewShape::BrushWall));
            }
        }

        if (shouldNotify) {
            QString matName = getMaterialDisplayName();
            QString shaderName = getCurrentShaderName();
            if (m_notificationPopup) {
                m_notificationPopup->showMessage(
                    tr("%1 material contains %2 (Brush Material), the viewport changed the shape")
                        .arg(matName, shaderName),
                    NotificationSeverity::Info, 6000);
                m_notificationPopup->reposition(m_viewport);
            }
        }
    }

    m_wasBrushMaterial = isBrush;
    m_lastShader = currentShader;
}

void MainWindow::syncPreview() {
    m_previewSyncTimer->start();
}

void MainWindow::onPreviewSyncTimeout() {
    if (m_engineConnecting) {
        return;
    }
    QString err;
    if (!ensureEngine(err)) {
        return;
    }

    // A model load queued while the engine was starting owns the viewport as
    // soon as it begins; the flat material preview must not override it.
    if (!m_pendingModelPath.isEmpty()) {
        const QString pending = m_pendingModelPath;
        m_pendingModelPath.clear();
        loadModelNow(pending);
        return;
    }

    // A material of the loaded model is previewed through its loose override at
    // the material's own game-relative path (docs/IPC_PROTOCOL.md): drop any
    // forced material and re-resolve just that slot, so the engine keeps
    // painting the model with its own materials.
    if (!m_currentModelMaterial.empty()) {
        const std::string modelMaterial = m_currentModelMaterial;
        if (m_session.dirty()) {
            if (!m_session.text_is_valid()) {
                statusBar()->showMessage(tr("Preview paused: fix the VMT syntax error first."), 4000);
                return;
            }
            const vmt::Result<void> writeRes =
                ui::write_preview_material_at(m_previewRoot, modelMaterial, m_session.text());
            if (!writeRes) {
                statusBar()->showMessage(
                    tr("Preview update failed: %1")
                        .arg(QString::fromStdString(writeRes.error().message)), 5000);
                return;
            }
        }
        (void)m_engine->set_material("");
        (void)m_engine->reload_material_by_name(modelMaterial);
        return;
    }

    std::string materialName = "vmtviewer_preview";
    if (!m_session.path().empty()) {
        materialName = ui::vtf_texture_path(m_session.path());
        if (materialName.empty()) {
            materialName = m_session.path().stem().string();
        }
    }

    // Write preview file to mounted preview root preserving directory hierarchy
    std::filesystem::path outFile = m_previewRoot / "materials" / (materialName + ".vmt");
    std::error_code ec;
    std::filesystem::create_directories(outFile.parent_path(), ec);

    std::ofstream stream(outFile, std::ios::binary);
    if (stream) {
        stream << m_session.text();
        stream.close();
    }

    // If there is an adjacent loose .vtf in the source directory (i.e. not in a materials/ structure),
    // copy it into the preview materials directory so the engine can resolve $basetexture
    if (!m_session.path().empty()) {
        std::filesystem::path srcDir = m_session.path().parent_path();
        std::filesystem::path srcVtf = srcDir / (m_session.path().stem().string() + ".vtf");
        if (std::filesystem::is_regular_file(srcVtf, ec)) {
            std::filesystem::path destVtf = outFile.parent_path() / (m_session.path().stem().string() + ".vtf");
            std::filesystem::copy_file(srcVtf, destVtf, std::filesystem::copy_options::overwrite_existing, ec);
        }
    }

    (void)m_engine->set_material(materialName);
    (void)m_engine->reload_material();
}

bool MainWindow::maybeSave() {
    if (!m_session.dirty()) {
        return true;
    }
    auto reply = QMessageBox::question(
        this, tr("Unsaved Changes"),
        tr("The current material has unsaved changes.\nDo you want to save them before proceeding?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);

    if (reply == QMessageBox::Save) {
        onSaveFile();
        return !m_session.dirty();
    }
    return reply != QMessageBox::Cancel;
}

void MainWindow::onNewFile() {
    if (!maybeSave()) return;

    std::string defaultVmt =
        "\"LightmappedGeneric\"\n"
        "{\n"
        "\t\"$basetexture\"\t\"dev/dev_measuregeneric01\"\n"
        "\t\"$surfaceprop\"\t\"concrete\"\n"
        "}\n";

    (void)m_session.open_text(defaultVmt, "untitled.vmt");
    m_currentModelMaterial.clear();

    m_suppressSync = true;
    m_codeEditor->setPlainText(QString::fromStdString(defaultVmt));
    m_inspector->updateFromDocument(m_session.document(), m_schemas, m_showAdvanced);
    m_suppressSync = false;

    updateTitle();
    updateStatus();
    checkBrushMaterial(true);
    syncPreview();
}

bool MainWindow::openFile(const QString& filePath) {
    if (!maybeSave()) return false;

    std::filesystem::path path(filePath.toStdWString());
    auto res = m_session.open_file(path);
    if (!res) {
        QMessageBox::warning(this, tr("Open Failed"), QString::fromStdString(res.error().message));
        return false;
    }

    addRecentFile(filePath);
    refreshMaterialRoots();
    m_currentModelMaterial.clear();

    if (m_engine && m_engine->initialized()) {
        const auto& matRoot = m_session.material_root();
        if (!matRoot.empty()) {
            (void)m_engine->mount_directory(matRoot, true);
        }
    }

    m_suppressSync = true;
    m_codeEditor->setPlainText(QString::fromStdString(m_session.text()));
    m_inspector->updateFromDocument(m_session.document(), m_schemas, m_showAdvanced);
    m_suppressSync = false;

    updateTitle();
    updateStatus();
    checkBrushMaterial(true);
    syncPreview();
    return true;
}

bool MainWindow::openGameMaterial(const QString& materialName) {
    if (!maybeSave()) return false;

    QString err;
    if (!ensureEngine(err)) {
        QMessageBox::warning(this, tr("Engine Error"), err);
        return false;
    }

    std::string path = "materials/" + materialName.toStdString();
    if (path.rfind(".vmt") == std::string::npos) {
        path += ".vmt";
    }

    auto readRes = m_engine->read_text_file(path);
    if (!readRes) {
        QMessageBox::warning(this, tr("Read Failed"), QString::fromStdString(readRes.error().message));
        return false;
    }

    (void)m_session.open_text(readRes.value(), materialName.toStdString() + ".vmt");
    // Materials of the loaded model keep the in-place override path on save.
    m_currentModelMaterial = containsModelMaterial(materialName) ? materialName.toStdString() : std::string();

    m_suppressSync = true;
    m_codeEditor->setPlainText(QString::fromStdString(m_session.text()));
    m_inspector->updateFromDocument(m_session.document(), m_schemas, m_showAdvanced);
    m_suppressSync = false;

    updateTitle();
    updateStatus();
    checkBrushMaterial(true);
    syncPreview();
    return true;
}

void MainWindow::onOpenFile() {
    QString file = QFileDialog::getOpenFileName(
        this, tr("Open Source Material (.vmt)"),
        QString(), tr("Source Materials (*.vmt);;All Files (*.*)"));
    if (!file.isEmpty()) {
        openFile(file);
    }
}

void MainWindow::onSaveFile() {
    if (!m_currentModelMaterial.empty()) {
        saveModelMaterialOverride();
        return;
    }
    if (m_session.path().empty()) {
        onSaveFileAs();
        return;
    }
    auto res = m_session.save();
    if (!res) {
        QMessageBox::warning(this, tr("Save Failed"), QString::fromStdString(res.error().message));
    }
    updateTitle();
    updateStatus();
}

void MainWindow::onSaveFileAs() {
    QString file = QFileDialog::getSaveFileName(
        this, tr("Save Material As"),
        QString(), tr("Source Materials (*.vmt);;All Files (*.*)"));
    if (file.isEmpty()) return;

    auto res = m_session.save_as(std::filesystem::path(file.toStdWString()));
    if (!res) {
        QMessageBox::warning(this, tr("Save Failed"), QString::fromStdString(res.error().message));
    } else {
        addRecentFile(file);
        // The session now targets an explicit file, not a model override.
        m_currentModelMaterial.clear();
    }
    updateTitle();
    updateStatus();
}

void MainWindow::onSaveToGame() {
    if (m_settings.last_game_dir.empty()) {
        QMessageBox::information(this, tr("No Game Connected"),
            tr("Please configure the game mod folder via Game > Connection Settings."));
        return;
    }

    QString matName = QString::fromStdWString(m_session.path().stem().wstring());
    if (matName.isEmpty()) matName = "custom_material";

    std::filesystem::path gameMatPath =
        std::filesystem::path(m_settings.last_game_dir) / "materials" / (matName.toStdString() + ".vmt");

    std::error_code ec;
    std::filesystem::create_directories(gameMatPath.parent_path(), ec);

    auto res = m_session.save_as(gameMatPath);
    if (!res) {
        QMessageBox::warning(this, tr("Save Failed"), QString::fromStdString(res.error().message));
    } else {
        QMessageBox::information(this, tr("Saved to Game"),
            tr("Material saved to:\n%1").arg(QString::fromStdWString(gameMatPath.wstring())));
    }
    updateTitle();
    updateStatus();
}

void MainWindow::onRevertFile() {
    if (!m_session.dirty()) return;
    auto reply = QMessageBox::question(
        this, tr("Revert Changes"),
        tr("Discard all unsaved edits and reload from disk?"),
        QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::Yes) {
        m_session.revert();
        m_suppressSync = true;
        m_codeEditor->setPlainText(QString::fromStdString(m_session.text()));
        m_inspector->updateFromDocument(m_session.document(), m_schemas, m_showAdvanced);
        m_suppressSync = false;
        updateTitle();
        updateStatus();
        checkBrushMaterial(false);
        syncPreview();
    }
}

void MainWindow::onRecentFileTriggered() {
    auto* action = qobject_cast<QAction*>(sender());
    if (action) {
        openFile(action->text());
    }
}

void MainWindow::onUndo() {
    if (m_session.undo()) {
        m_suppressSync = true;
        m_codeEditor->setPlainText(QString::fromStdString(m_session.text()));
        m_inspector->updateFromDocument(m_session.document(), m_schemas, m_showAdvanced);
        m_suppressSync = false;
        updateTitle();
        updateStatus();
        checkBrushMaterial(false);
        syncPreview();
    }
}

void MainWindow::onRedo() {
    if (m_session.redo()) {
        m_suppressSync = true;
        m_codeEditor->setPlainText(QString::fromStdString(m_session.text()));
        m_inspector->updateFromDocument(m_session.document(), m_schemas, m_showAdvanced);
        m_suppressSync = false;
        updateTitle();
        updateStatus();
        checkBrushMaterial(false);
        syncPreview();
    }
}

void MainWindow::onToggleShowAdvanced(bool show) {
    m_showAdvanced = show;
    m_inspector->updateFromDocument(m_session.document(), m_schemas, m_showAdvanced);
}

void MainWindow::onResetLayout() {
    // Reset splitter proportions
    if (auto* splitter = qobject_cast<QSplitter*>(centralWidget())) {
        splitter->setSizes({340, 560, 460});
    }
}

void MainWindow::onShapeSelected(int index) {
    ui::PreviewShape newShape = static_cast<ui::PreviewShape>(index);
    if (isCurrentMaterialBrushOnly() && !ui::is_brush_shape(newShape)) {
        if (m_shapeCombo && m_shapeCombo->currentIndex() != static_cast<int>(m_previewOptions.shape)) {
            m_shapeCombo->blockSignals(true);
            m_shapeCombo->setCurrentIndex(static_cast<int>(m_previewOptions.shape));
            m_shapeCombo->blockSignals(false);
        }

        QString mat = getMaterialDisplayName();
        QString shader = getCurrentShaderName();

        if (m_notificationPopup) {
            m_notificationPopup->showMessage(
                tr("Cannot change shape: %1 material contains %2 (Brush Material). Viewport must remain a brush shape.")
                    .arg(mat, shader),
                NotificationSeverity::Warning, 5000);
            m_notificationPopup->reposition(m_viewport);
        }

        QMessageBox::warning(this, tr("Cannot Change Shape"),
            tr("Cannot change to model shape: '%1' material contains '%2' (Brush Material).\n\n"
               "World brush shaders are exclusive to Hammer / world geometry and cannot be previewed on 3D models.")
            .arg(mat, shader));
        return;
    }

    m_previewOptions.shape = newShape;
    if (m_shapeCombo && m_shapeCombo->currentIndex() != index) {
        m_shapeCombo->blockSignals(true);
        m_shapeCombo->setCurrentIndex(index);
        m_shapeCombo->blockSignals(false);
    }
    if (m_engine && m_engine->initialized()) {
        (void)m_engine->set_preview_shape(to_bridge_shape(m_previewOptions.shape));
    }
}

void MainWindow::onRenderModeSelected(int index) {
    const char* modes[] = {"shaded", "wireframe", "albedo", "normals"};
    if (index >= 0 && index < 4) {
        m_currentRenderMode = modes[index];
        if (m_renderModeCombo && m_renderModeCombo->currentIndex() != index) {
            m_renderModeCombo->blockSignals(true);
            m_renderModeCombo->setCurrentIndex(index);
            m_renderModeCombo->blockSignals(false);
        }
        if (m_engine && m_engine->initialized()) {
            (void)m_engine->set_render_mode(m_currentRenderMode);
        }
    }
}

void MainWindow::onToggleAnimation() {
    m_animPlaying = !m_animPlaying;
    if (m_engine && m_engine->initialized()) {
        (void)m_engine->set_animation(m_animPlaying ? "play" : "pause");
    }
}

void MainWindow::onSetAnimationSpeed(float speed) {
    setAnimationSpeedScale(speed);
}

void MainWindow::onOpenPreviewOptions() {
    PreviewOptionsDialog dlg(m_previewOptions, isCurrentMaterialBrushOnly(), this);
    connect(&dlg, &PreviewOptionsDialog::optionsChanged, this, [this](const ui::PreviewOptions& opts) {
        m_previewOptions = opts;
        if (m_shapeCombo) {
            m_shapeCombo->blockSignals(true);
            m_shapeCombo->setCurrentIndex(static_cast<int>(m_previewOptions.shape));
            m_shapeCombo->blockSignals(false);
        }
        if (m_engine && m_engine->initialized()) {
            (void)m_engine->set_preview_shape(to_bridge_shape(m_previewOptions.shape));
            (void)m_engine->set_lighting(to_bridge_lighting(m_previewOptions.lighting));
        }
    });
    connect(&dlg, &PreviewOptionsDialog::loadModelRequested, this, &MainWindow::onLoadModel);
    if (dlg.exec() == QDialog::Accepted) {
        m_previewOptions = dlg.options();
        if (m_shapeCombo) {
            m_shapeCombo->blockSignals(true);
            m_shapeCombo->setCurrentIndex(static_cast<int>(m_previewOptions.shape));
            m_shapeCombo->blockSignals(false);
        }
        saveSettings();
    }
}

void MainWindow::onLoadModel() {
    if (isCurrentMaterialBrushOnly()) {
        QString mat = getMaterialDisplayName();
        QString shader = getCurrentShaderName();

        if (m_notificationPopup) {
            m_notificationPopup->showMessage(
                tr("Cannot load model: %1 material contains %2 (Brush Material).")
                    .arg(mat, shader),
                NotificationSeverity::Warning, 5000);
            m_notificationPopup->reposition(m_viewport);
        }

        QMessageBox::warning(this, tr("Cannot Load Model"),
            tr("Cannot preview custom model: '%1' material contains '%2' (Brush Material).\n\n"
               "World brush shaders cannot be applied to 3D models.")
            .arg(mat, shader));
        return;
    }

    QString file = QFileDialog::getOpenFileName(
        this, tr("Select Source Engine Model (.mdl)"),
        QString(), tr("Source Models (*.mdl);;All Files (*.*)"));
    if (!file.isEmpty()) {
        // Route through openModel so the docks, recents and title stay in sync.
        openModel(file);
    }
}

// --- Model viewer -------------------------------------------------------------

void MainWindow::onOpenModel() {
    if (!m_engine || !m_engine->initialized()) {
        QString err;
        if (!ensureEngine(err)) {
            statusBar()->showMessage(
                tr("The model browser needs a connected engine: %1").arg(err), 6000);
            return;
        }
    }

    ModelBrowserDialog dlg(m_engine.get(), this);
    connect(&dlg, &ModelBrowserDialog::modelSelected, this, &MainWindow::onModelSelected);
    dlg.exec();
}

void MainWindow::onModelSelected(const QString& gameRelative, bool loose) {
    Q_UNUSED(loose); // vpk and loose models load through the same engine path
    openModel(gameRelative);
}

void MainWindow::onCloseModel() {
    if (m_currentModelName.isEmpty() && m_requestedModelPath.isEmpty()) {
        return;
    }
    cleanupIsolation(true);
    clearModelView();
    statusBar()->showMessage(tr("Model closed."), 3000);
}

void MainWindow::onRecentModelTriggered() {
    auto* action = qobject_cast<QAction*>(sender());
    if (!action) {
        return;
    }
    const QString path = action->data().toString();
    if (path.isEmpty()) {
        return;
    }

    // Absolute entries can disappear between sessions; drop those instead of
    // turning the menu into a list of broken entries.
    if (QFileInfo(path).isAbsolute() && !QFileInfo::exists(path)) {
        m_settings.recent_models.erase(
            std::remove(m_settings.recent_models.begin(), m_settings.recent_models.end(),
                        path.toStdString()),
            m_settings.recent_models.end());
        updateRecentModelsMenu();
        saveSettings();
        statusBar()->showMessage(tr("The model \"%1\" no longer exists.").arg(path), 5000);
        return;
    }

    openModel(path);
}

void MainWindow::openModel(const QString& modelPath) {
    const QString path = modelPath.trimmed();
    if (path.isEmpty()) {
        return;
    }

    if (m_engine && m_engine->initialized()) {
        loadModelNow(path);
        return;
    }

    if (m_engineConnecting) {
        m_pendingModelPath = path;
        statusBar()->showMessage(
            tr("Connecting to the engine: \"%1\" will load automatically.").arg(path), 5000);
        return;
    }

    QString err;
    if (ensureEngine(err)) {
        loadModelNow(path);
        return;
    }

    // Not configured yet or initialization failed: remember the request; the
    // asynchronous connect (started here or later after Game > Connection
    // Settings) picks it up.
    m_pendingModelPath = path;
    ensureEngineAsync();
    statusBar()->showMessage(
        tr("\"%1\" is queued and will load once the engine is ready.").arg(path), 6000);
}

void MainWindow::updateRecentModelsMenu() {
    if (!m_recentModelsMenu) {
        return;
    }
    m_recentModelsMenu->clear();
    if (m_settings.recent_models.empty()) {
        auto* action = m_recentModelsMenu->addAction(tr("(No recent models)"));
        action->setEnabled(false);
        return;
    }
    for (const std::string& pathStr : m_settings.recent_models) {
        const QString path = QString::fromStdString(pathStr);
        auto* action = m_recentModelsMenu->addAction(path, this, &MainWindow::onRecentModelTriggered);
        action->setData(path);
    }
}

void MainWindow::addRecentModel(const QString& path) {
    if (path.isEmpty()) {
        return;
    }
    m_settings.last_model = path.toStdString();
    m_settings.remember_model(m_settings.last_model);
    updateRecentModelsMenu();
    saveSettings();
}

void MainWindow::loadModelNow(const QString& modelPath) {
    if (!m_engine || !m_engine->initialized()) {
        return;
    }

    QString target = modelPath.trimmed();
    if (target.isEmpty()) {
        return;
    }

    // Absolute disk paths below the game folder are addressed by their
    // game-relative name: the backend resolves those through the engine file
    // system (VPK entries included), a bare disk path would miss mounted VPKs.
    if (QFileInfo(target).isAbsolute()) {
        const QString relative = gameRelativePath(target);
        if (!relative.isEmpty()) {
            target = relative;
        }
    }
    target.replace('\\', '/');

    // A new model replaces the current one: drop the previous isolation, stale
    // scene state and any queued preview sync that would force a material over
    // the fresh model.
    cleanupIsolation(true);
    clearModelView();
    m_previewSyncTimer->stop();

    m_requestedModelPath = target;
    const vmt::Result<void> loadRes =
        m_engine->set_model(std::filesystem::path(target.toStdWString()));
    if (!loadRes) {
        const QString message = QString::fromStdString(loadRes.error().message);
        m_requestedModelPath.clear();
        updateModelActions();
        statusBar()->showMessage(tr("Cannot load model \"%1\": %2").arg(target, message), 8000);
        return;
    }

    // Drop a forced material left over from the flat preview flow
    // (docs/IPC_PROTOCOL.md: SET_MATERIAL with an empty name clears it).
    (void)m_engine->set_material("");

    updateModelActions();
    statusBar()->showMessage(tr("Loading model \"%1\"...").arg(target), 4000);
    queryModelSceneAsync();
}

void MainWindow::queryModelSceneAsync() {
    if (!m_engine || !m_engine->initialized()) {
        return;
    }
    bridge::EngineView* engine = m_engine.get();
    const quint64 generation = m_modelSceneGeneration;
    auto alive = m_alive;

    std::thread([this, engine, generation, alive]() {
        auto result = engine->query_model_scene();
        if (!*alive) {
            return;
        }
        if (result) {
            auto scene = std::make_shared<bridge::ModelScene>(std::move(result.value()));
            QMetaObject::invokeMethod(qApp, [this, alive, generation, scene]() {
                if (!*alive || generation != m_modelSceneGeneration) {
                    return; // superseded by a newer load or a close
                }
                onModelSceneLoaded(scene);
            }, Qt::QueuedConnection);
        } else {
            const std::string message = result.error().message;
            QMetaObject::invokeMethod(qApp, [this, alive, generation, message]() {
                if (!*alive || generation != m_modelSceneGeneration) {
                    return;
                }
                onModelSceneFailed(QString::fromStdString(message));
            }, Qt::QueuedConnection);
        }
    }).detach();
}

void MainWindow::onModelSceneLoaded(const std::shared_ptr<bridge::ModelScene>& scene) {
    if (!scene || scene->model_name.empty()) {
        onModelSceneFailed(tr("The engine did not report a loaded model."));
        return;
    }

    m_modelScene = scene;
    m_currentModelName = QString::fromStdString(scene->model_name);
    m_requestedModelPath = m_currentModelName;

    if (m_materialsDock) {
        m_materialsDock->setScene(*scene);
        m_materialsDock->setEnabled(true);
    }
    if (m_animationDock) {
        m_animationDock->setScene(*scene);
        m_animationDock->show();
    }

    addRecentModel(m_currentModelName);
    updateModelActions();
    updateTitle();
    updateStatus();
    statusBar()->showMessage(
        tr("Model \"%1\" loaded: %2 material(s), %3 sequence(s).")
            .arg(m_currentModelName)
            .arg(static_cast<int>(scene->materials.size()))
            .arg(static_cast<int>(scene->sequences.size())),
        6000);
}

void MainWindow::onModelSceneFailed(const QString& error) {
    const QString attempted = m_requestedModelPath;
    clearModelView();
    statusBar()->showMessage(
        tr("Cannot load model \"%1\": %2").arg(attempted, error), 8000);
}

void MainWindow::clearModelView() {
    ++m_modelSceneGeneration; // invalidates any in-flight scene query

    m_modelScene.reset();
    m_currentModelName.clear();
    m_requestedModelPath.clear();
    m_currentModelMaterial.clear();

    if (m_materialsDock) {
        m_materialsDock->clear();
        m_materialsDock->setEnabled(false);
    }
    if (m_animationDock) {
        m_animationDock->clear();
        m_animationDock->hide();
    }

    updateModelActions();
    updateTitle();
    updateStatus();
}

void MainWindow::updateModelActions() {
    const bool hasModel = !m_currentModelName.isEmpty() || !m_requestedModelPath.isEmpty();
    if (m_closeModelAction) {
        m_closeModelAction->setEnabled(hasModel);
    }
    if (m_exportMaterialsAction) {
        m_exportMaterialsAction->setEnabled(
            m_modelScene != nullptr && m_engine != nullptr && m_engine->initialized());
    }
}

QString MainWindow::gameRelativePath(const QString& diskPath) const {
    if (diskPath.isEmpty() || m_settings.last_game_dir.empty()) {
        return QString();
    }

    std::error_code ec;
    const std::filesystem::path file(diskPath.toStdWString());
    const std::filesystem::path root(m_settings.last_game_dir);
    const std::filesystem::path relative = std::filesystem::relative(file, root, ec);
    if (ec || relative.empty() || relative.is_absolute() || *relative.begin() == L"..") {
        return QString(); // outside the game folder (or not resolvable)
    }
    return QString::fromStdWString(relative.generic_wstring());
}

bool MainWindow::containsModelMaterial(const QString& gameName) const {
    if (!m_modelScene || gameName.isEmpty()) {
        return false;
    }
    const std::string name = gameName.toStdString();
    for (const bridge::ModelMaterial& material : m_modelScene->materials) {
        if (material_name_equal(material.name, name)) {
            return true;
        }
    }
    return false;
}

void MainWindow::onModelMaterialSelected(const QString& gameName) {
    if (gameName.isEmpty() || m_currentModelName.isEmpty()) {
        return; // the dock cleared its own selection
    }
    openModelMaterial(gameName, false);
}

void MainWindow::onModelMaterialActivated(const QString& gameName) {
    if (gameName.isEmpty() || m_currentModelName.isEmpty()) {
        return;
    }
    openModelMaterial(gameName, true);
}

bool MainWindow::openModelMaterial(const QString& gameName, bool focusEditor) {
    if (gameName.isEmpty() || m_currentModelName.isEmpty()) {
        return false;
    }
    const std::string name = gameName.toStdString();

    if (m_currentModelMaterial == name) {
        // Already open: keep the editor buffer (a re-activation must not load
        // over unsynced keystrokes), only re-assert the model preview.
        if (focusEditor) {
            m_codeEditor->setFocus();
        }
        if (m_engine && m_engine->initialized()) {
            (void)m_engine->set_material("");
        }
        return true;
    }

    // Leaving a material: flush its latest valid text to the override before
    // the editor buffer is replaced. A failed write aborts the switch so
    // unsynced edits cannot slip away.
    if (!m_currentModelMaterial.empty() &&
        m_session.dirty() && m_session.text_is_valid()) {
        const std::string previous = m_currentModelMaterial;
        const vmt::Result<void> flushRes =
            ui::write_preview_material_at(m_previewRoot, previous, m_session.text());
        if (!flushRes) {
            statusBar()->showMessage(
                tr("Cannot switch material: the pending edits of \"%1\" could not be written (%2).")
                    .arg(QString::fromStdString(previous),
                         QString::fromStdString(flushRes.error().message)),
                8000);
            return false;
        }
        if (m_engine && m_engine->initialized()) {
            (void)m_engine->reload_material_by_name(previous);
        }
    }

    // Prefer the loose override (the preview root outranks the game files):
    // reopening a material shows what was written last, the engine copy only
    // seeds a material that has no override yet. On the x64 UI the engine copy
    // is read from the game folder on disk.
    std::string text;
    bool loaded = false;
    const std::filesystem::path overrideFile = preview_override_path(m_previewRoot, name);
    if (!overrideFile.empty()) {
        std::ifstream stream(overrideFile, std::ios::binary);
        if (stream) {
            std::string contents((std::istreambuf_iterator<char>(stream)),
                                 std::istreambuf_iterator<char>());
            if (!contents.empty()) {
                text = std::move(contents);
                loaded = true;
            }
        }
    }

    if (!loaded) {
        if (!m_engine || !m_engine->initialized()) {
            statusBar()->showMessage(
                tr("Material \"%1\" needs a connected engine to read.").arg(gameName), 5000);
            return false;
        }
        auto readRes = m_engine->read_text_file("materials/" + name + ".vmt");
        if (!readRes) {
            statusBar()->showMessage(
                tr("Cannot read material \"%1\": %2")
                    .arg(gameName, QString::fromStdString(readRes.error().message)),
                6000);
            return false;
        }
        text = std::move(readRes.value());
    }

    auto openRes = m_session.open_text(std::move(text), name + ".vmt");
    if (!openRes) {
        statusBar()->showMessage(
            tr("Cannot open material \"%1\": %2")
                .arg(gameName, QString::fromStdString(openRes.error().message)),
            6000);
        return false;
    }
    m_currentModelMaterial = name;

    m_suppressSync = true;
    m_codeEditor->setPlainText(QString::fromStdString(m_session.text()));
    m_inspector->updateFromDocument(m_session.document(), m_schemas, m_showAdvanced);
    m_suppressSync = false;

    // The model keeps its own materials; only the edited slot is re-resolved
    // (the forced material preview is dropped, docs/IPC_PROTOCOL.md).
    if (m_engine && m_engine->initialized()) {
        (void)m_engine->set_material("");
        (void)m_engine->reload_material_by_name(name);
    }

    if (focusEditor) {
        m_codeEditor->setFocus();
    }
    updateTitle();
    updateStatus();
    checkBrushMaterial(true);
    return true;
}

void MainWindow::saveModelMaterialOverride() {
    const std::string name = m_currentModelMaterial;
    if (name.empty()) {
        return;
    }

    const std::filesystem::path file = preview_override_path(m_previewRoot, name);
    if (file.empty()) {
        QMessageBox::warning(this, tr("Save Failed"),
            tr("The material name cannot be mapped to a preview override path."));
        return;
    }

    // save_as() writes atomically and marks the session clean; the written
    // file IS the in-place override the engine re-reads.
    auto res = m_session.save_as(file);
    if (!res) {
        QMessageBox::warning(this, tr("Save Failed"), QString::fromStdString(res.error().message));
        return;
    }

    if (m_engine && m_engine->initialized()) {
        (void)m_engine->reload_material_by_name(name);
    }
    updateTitle();
    updateStatus();
    statusBar()->showMessage(
        tr("Model material saved as a loose override: %1")
            .arg(QString::fromStdWString(file.wstring())),
        5000);
}

void MainWindow::onModelMaterialsExported(const QStringList& materials) {
    if (materials.isEmpty()) {
        return;
    }
    if (!m_engine || !m_engine->initialized()) {
        statusBar()->showMessage(tr("Exporting needs a connected engine."), 4000);
        return;
    }
    ExportMaterialsDialog dlg(m_engine.get(), materials, m_currentModelName, this);
    dlg.exec();
}

void MainWindow::onExportModelMaterials() {
    if (!m_modelScene || m_modelScene->materials.empty()) {
        statusBar()->showMessage(tr("No model materials to export."), 4000);
        return;
    }
    QStringList materials;
    for (const bridge::ModelMaterial& material : m_modelScene->materials) {
        if (material.name.empty()) {
            continue;
        }
        const QString name = QString::fromStdString(material.name);
        if (!materials.contains(name, Qt::CaseInsensitive)) {
            materials.append(name);
        }
    }
    onModelMaterialsExported(materials);
}

void MainWindow::onIsolateToggled(const QString& gameName, bool enabled) {
    if (gameName.isEmpty() || m_currentModelName.isEmpty()) {
        return; // the dock cleared its own selection
    }
    if (enabled) {
        applyIsolation(gameName);
    } else {
        cleanupIsolation(true);
    }
}

void MainWindow::applyIsolation(const QString& keepName) {
    if (!m_engine || !m_engine->initialized() || !m_modelScene) {
        statusBar()->showMessage(
            tr("Isolate mode needs a connected engine and a loaded model."), 5000);
        return;
    }

    // Replacing a previous isolation restores those materials first.
    cleanupIsolation(true);

    const std::string keep = keepName.toStdString();
    int substituted = 0;
    for (const bridge::ModelMaterial& material : m_modelScene->materials) {
        // Never hide the kept material or the one open in the editor.
        if (material.name.empty() || material_name_equal(material.name, keep) ||
            material_name_equal(material.name, m_currentModelMaterial)) {
            continue;
        }

        const std::filesystem::path file = preview_override_path(m_previewRoot, material.name);
        if (file.empty()) {
            continue;
        }
        std::error_code ec;
        if (std::filesystem::exists(file, ec)) {
            continue; // a real override stays untouched
        }
        std::filesystem::create_directories(file.parent_path(), ec);

        std::ofstream stream(file, std::ios::binary);
        if (!stream) {
            continue;
        }
        stream << kTransparentVmt;
        stream.close();

        m_isolateSubstitutes.push_back({ file, material.name });
        (void)m_engine->reload_material_by_name(material.name);
        ++substituted;
    }

    if (substituted == 0) {
        statusBar()->showMessage(
            tr("Nothing to isolate: every other material already has an override."), 5000);
    } else {
        statusBar()->showMessage(
            tr("Isolating \"%1\": %2 material(s) hidden.").arg(keepName).arg(substituted), 5000);
    }
}

void MainWindow::cleanupIsolation(bool reload) {
    if (m_isolateSubstitutes.empty()) {
        return;
    }

    std::vector<std::string> restore;
    restore.reserve(m_isolateSubstitutes.size());
    std::error_code ec;
    for (const IsolateSubstitute& substitute : m_isolateSubstitutes) {
        std::filesystem::remove(substitute.file, ec);
        restore.push_back(substitute.material);
    }
    m_isolateSubstitutes.clear();

    if (reload && m_engine && m_engine->initialized()) {
        for (const std::string& material : restore) {
            (void)m_engine->reload_material_by_name(material);
        }
    }
}

void MainWindow::setAnimationSpeedScale(float speed) {
    m_animSpeedScale = ui::clamp_animation_speed(speed);
    if (m_engine && m_engine->initialized()) {
        (void)m_engine->set_speed(m_animSpeedScale);
    }
    statusBar()->showMessage(
        tr("Playback speed: %1x").arg(QString::number(static_cast<double>(m_animSpeedScale), 'f', 2)),
        3000);
}

void MainWindow::onAnimationSpeedDown() {
    setAnimationSpeedScale(m_animSpeedScale * 0.5f);
}

void MainWindow::onAnimationSpeedUp() {
    setAnimationSpeedScale(m_animSpeedScale * 2.0f);
}

void MainWindow::onOpenMaterialBrowser() {
    refreshMaterialRoots();
    MaterialBrowserDialog dlg(m_engine.get(), m_materialRoots, this);
    connect(&dlg, &MaterialBrowserDialog::materialSelected,
            this, [this](const QString& name, const QString& diskPath, bool fromEngine) {
        if (!diskPath.isEmpty()) {
            openFile(diskPath);
        } else if (fromEngine) {
            openGameMaterial(name);
        }
    });
    dlg.exec();
}

void MainWindow::onValidateMaterial() {
    refreshMaterialRoots();
    if (m_session.document() == nullptr) {
        m_validationTable->setRowCount(0);
        m_statusFindings->setText(tr("Diagnostics: 0 errors, 0 warnings"));
        return;
    }

    ui::ValidationOptions options;
    options.schemas = &m_schemas;
    options.material_roots = m_materialRoots;
    if (m_engine && m_engine->initialized()) {
        options.resolve_texture = [this](const std::string& name) {
            auto exists = m_engine->file_exists("materials/" + name + ".vtf");
            if (!exists) return ui::TextureStatus::Unavailable;
            return exists.value() ? ui::TextureStatus::Found : ui::TextureStatus::Missing;
        };
    }

    auto findings = ui::validate_document(*m_session.document(), options);

    m_validationTable->setRowCount(static_cast<int>(findings.size()));
    int errors = 0;
    int warnings = 0;

    for (int i = 0; i < static_cast<int>(findings.size()); ++i) {
        const auto& f = findings[i];
        QString sevStr = (f.severity == ui::Severity::Error) ? tr("Error") :
                         (f.severity == ui::Severity::Warning) ? tr("Warning") : tr("Info");
        if (f.severity == ui::Severity::Error) ++errors;
        else if (f.severity == ui::Severity::Warning) ++warnings;

        auto* itemSev = new QTableWidgetItem(sevStr);
        itemSev->setForeground(f.severity == ui::Severity::Error ? QColor("#f14c4c") :
                               f.severity == ui::Severity::Warning ? QColor("#cca700") : QColor("#4ec9b0"));

        auto* itemCode = new QTableWidgetItem(QString::fromStdString(f.code));
        auto* itemMsg = new QTableWidgetItem(QString::fromStdString(f.message));
        auto* itemLoc = new QTableWidgetItem(f.line > 0 ? tr("Line %1").arg(f.line) : tr("--"));

        m_validationTable->setItem(i, 0, itemSev);
        m_validationTable->setItem(i, 1, itemCode);
        m_validationTable->setItem(i, 2, itemMsg);
        m_validationTable->setItem(i, 3, itemLoc);
    }

    m_statusFindings->setText(tr("Diagnostics: %1 errors, %2 warnings").arg(errors).arg(warnings));
    m_validationDock->show();
    m_validationDock->raise();
}

void MainWindow::onToggleDebugConsole() {
    if (is_console_open()) {
        console_close();
    } else {
        console_open();
    }
}

void MainWindow::onOpenGameConnection() {
    ConnectionDialog dlg(m_settings, this);
    if (dlg.exec() == QDialog::Accepted) {
        saveSettings();
        refreshMaterialRoots();

        // Tear the model view down before the engine goes away: the animation
        // dock must never keep a dangling engine pointer.
        if (m_animationDock) {
            m_animationDock->setEngine(nullptr);
        }
        cleanupIsolation(false);
        clearModelView();

        if (m_engine) {
            m_engine->shutdown();
            m_engine.reset();
        }
        ensureEngineAsync();
    }
}

void MainWindow::onOpenAbout() {
    AboutDialog dlg(this);
    dlg.exec();
}

void MainWindow::onCodeEditorTextChanged() {
    if (m_suppressSync) return;

    std::string newText = m_codeEditor->toPlainText().toStdString();
    (void)m_session.set_text(newText);

    // Update inspector without clobbering editor focus
    m_suppressSync = true;
    m_inspector->updateFromDocument(m_session.document(), m_schemas, m_showAdvanced);
    m_suppressSync = false;

    updateTitle();
    updateStatus();
    checkBrushMaterial(false);
    syncPreview();
}

void MainWindow::onInspectorParameterChanged(const QString& name, const QString& value) {
    if (m_suppressSync) return;

    (void)m_session.set_parameter(name.toStdString(), value.toStdString());

    const int savedPosition = m_codeEditor->textCursor().position();

    m_suppressSync = true;
    m_codeEditor->setPlainText(QString::fromStdString(m_session.text()));
    QTextCursor restored = m_codeEditor->textCursor();
    restored.setPosition(std::min(savedPosition, static_cast<int>(m_codeEditor->toPlainText().size())));
    m_codeEditor->setTextCursor(restored);
    m_suppressSync = false;

    updateTitle();
    updateStatus();
    syncPreview();
}

void MainWindow::onInspectorShaderChanged(const QString& newShader) {
    if (m_suppressSync) return;

    (void)m_session.set_shader(newShader.toStdString());

    const int savedPosition = m_codeEditor->textCursor().position();

    m_suppressSync = true;
    m_codeEditor->setPlainText(QString::fromStdString(m_session.text()));
    m_inspector->updateFromDocument(m_session.document(), m_schemas, m_showAdvanced);
    QTextCursor restored = m_codeEditor->textCursor();
    restored.setPosition(std::min(savedPosition, static_cast<int>(m_codeEditor->toPlainText().size())));
    m_codeEditor->setTextCursor(restored);
    m_suppressSync = false;

    updateTitle();
    updateStatus();
    checkBrushMaterial(false);
    syncPreview();
}

void MainWindow::onBrowseTextureRequested(const QString& paramName, const QString& currentValue) {
    Q_UNUSED(currentValue);
    QString file = QFileDialog::getOpenFileName(
        this, tr("Select Texture (.vtf) for %1").arg(paramName),
        QString(), tr("Valve Texture Files (*.vtf);;All Files (*.*)"));
    if (!file.isEmpty()) {
        // Compute game-relative path if possible
        std::filesystem::path p(file.toStdWString());
        std::string relPath = p.stem().string();
        for (const auto& root : m_materialRoots) {
            std::error_code ec;
            auto rel = std::filesystem::relative(p, root, ec);
            if (!ec && !rel.empty() && rel.native().rfind(L"..", 0) != 0) {
                relPath = rel.parent_path().string() + "/" + p.stem().string();
                std::replace(relPath.begin(), relPath.end(), '\\', '/');
                if (relPath.rfind("materials/", 0) == 0) {
                    relPath = relPath.substr(10);
                }
                break;
            }
        }
        onInspectorParameterChanged(paramName, QString::fromStdString(relPath));
    }
}

void MainWindow::onViewportResized(int width, int height) {
    if (m_engine && m_engine->initialized()) {
        (void)m_engine->resize(width, height);
    }
}

void MainWindow::onMaterialDropped(const QString& filePath) {
    openFile(filePath);
}

void MainWindow::onModelDropped(const QString& filePath) {
    if (filePath.isEmpty()) {
        return;
    }
    openModel(filePath);
}

void MainWindow::onPlayheadTick(int frame, qintptr cycle_x10000) {
    if (!m_animationDock || !m_animationDock->isVisible() || m_currentModelName.isEmpty()) {
        return; // nothing to scrub without a model in view
    }
    m_animationDock->updatePlayhead(frame, static_cast<int>(cycle_x10000));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (maybeSave()) {
        if (m_notificationPopup) {
            m_notificationPopup->close();
        }
        event->accept();
    } else {
        event->ignore();
    }
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    QTimer::singleShot(50, this, [this]() {
        if (m_engine && m_engine->initialized()) {
            (void)m_engine->resize(0, 0);
        }
        checkBrushMaterial(true);
    });
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (m_notificationPopup && m_notificationPopup->isVisible()) {
        m_notificationPopup->reposition(m_viewport);
    }
}

void MainWindow::moveEvent(QMoveEvent* event) {
    QMainWindow::moveEvent(event);
    if (m_notificationPopup && m_notificationPopup->isVisible()) {
        m_notificationPopup->reposition(m_viewport);
    }
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        if (isMinimized()) {
            if (m_notificationPopup) {
                m_notificationPopup->hide();
            }
        }
    }
}

} // namespace qt
} // namespace ui
