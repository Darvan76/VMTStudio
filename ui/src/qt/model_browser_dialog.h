#pragma once

// Model browser: lists every .mdl reachable through the engine file system
// (loose folders and VPK archives) and hands the selection back to the main
// window.
//
// EngineView::list_models() blocks until the 32-bit backend answers through
// its response files, so every scan runs on a detached worker thread and posts
// its result with a queued invocation. A shared alive flag (the same
// discipline as MainWindow::ensureEngineAsync) guarantees that a scan which
// outlives the dialog never touches freed widgets.

#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPoint>
#include <QPushButton>
#include <QTreeWidget>

#include "bridge/engine_view.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace ui {
namespace qt {

class ModelBrowserDialog : public QDialog {
    Q_OBJECT

public:
    explicit ModelBrowserDialog(bridge::EngineView* engine, QWidget* parent = nullptr);
    ~ModelBrowserDialog() override;

signals:
    // `gameRelative` addresses the model through the engine file system
    // ("models/props_c17/oildrum001.mdl"); the main window resolves the disk
    // path when it needs one. `loose` is true for models that live on disk
    // instead of inside a VPK.
    void modelSelected(const QString& gameRelative, bool loose);

protected:
    // The dialog does not scan on construction: the first show starts the
    // initial scan so the window appears instantly.
    void showEvent(QShowEvent* event) override;

private slots:
    void onScanClicked();
    void onSearchTextChanged();
    void onSourceFilterChanged();
    void onItemSelectionChanged();
    void onItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onLoadClicked();
    void onTreeContextMenu(const QPoint& pos);

private:
    void startScan();
    void applyFilter();
    void populateTree(const std::vector<const bridge::ModelEntry*>& matches);
    void updateSummary(std::size_t matched, bool filtered);
    void loadItem(QTreeWidgetItem* item);
    void setStatus(const QString& text);

    bridge::EngineView* m_engine = nullptr;

    std::vector<bridge::ModelEntry> m_entries;
    bool m_scanned = false;     // m_entries holds the result of a completed scan
    bool m_scanRunning = false; // a worker thread is querying the backend
    bool m_firstShow = true;    // the first showEvent starts the initial scan
    QString m_summary;          // last scan outcome, restored by updateSummary()

    // Set by the destructor; the scan worker checks it before posting results.
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);

    QLineEdit* m_searchEdit = nullptr;
    QPushButton* m_scanBtn = nullptr;
    QComboBox* m_sourceCombo = nullptr;
    QLabel* m_statusLabel = nullptr;

    QTreeWidget* m_tree = nullptr;
    QLabel* m_countLabel = nullptr;
    QPushButton* m_loadBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;
};

} // namespace qt
} // namespace ui
