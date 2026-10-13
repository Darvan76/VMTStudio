#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QLabel>
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QSplitter>

#include "ui/material_browser.h"
#include <filesystem>
#include <vector>

namespace bridge {
class EngineView;
}

namespace ui {
namespace qt {

class MaterialBrowserDialog : public QDialog {
    Q_OBJECT

public:
    explicit MaterialBrowserDialog(bridge::EngineView* engine,
                                  const std::vector<std::filesystem::path>& materialRoots,
                                  QWidget* parent = nullptr);
    ~MaterialBrowserDialog() override = default;

signals:
    void materialSelected(const QString& name, const QString& diskPath, bool fromEngine);

private slots:
    void onSearchTextChanged(const QString& text);
    void onScanGameClicked();
    void onScanFolderClicked();
    void onItemSelectionChanged();
    void onItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onOpenClicked();

private:
    void populateTree(const std::vector<std::size_t>& indices);
    void showMaterialDetails(const ui::MaterialEntry& entry);

    bridge::EngineView* m_engine = nullptr;
    std::vector<std::filesystem::path> m_materialRoots;

    ui::MaterialIndex m_index;

    QLineEdit* m_searchEdit = nullptr;
    QPushButton* m_scanGameBtn = nullptr;
    QPushButton* m_scanFolderBtn = nullptr;

    QTreeWidget* m_treeWidget = nullptr;
    QLabel* m_thumbLabel = nullptr;
    QPlainTextEdit* m_infoEdit = nullptr;
    QPushButton* m_openBtn = nullptr;
    QCheckBox* m_flipCheck = nullptr;
    QLabel* m_statusLabel = nullptr;

    int m_selectedEntryIndex = -1;
};

} // namespace qt
} // namespace ui
