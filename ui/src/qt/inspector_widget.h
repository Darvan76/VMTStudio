#pragma once

#include <QWidget>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QMap>

#include "ui/panel_model.h"
#include "vmt/document.h"
#include "vmt/schema.h"

namespace ui {
namespace qt {

class InspectorWidget : public QWidget {
    Q_OBJECT

public:
    explicit InspectorWidget(QWidget* parent = nullptr);
    ~InspectorWidget() override = default;

    void updateFromDocument(const vmt::Document* doc,
                            const vmt::SchemaLibrary& schemas,
                            bool showAdvanced);

    void setAvailableShaders(const QStringList& shaders);
    void setCurrentShader(const QString& shader);

signals:
    void parameterChanged(const QString& name, const QString& value);
    void parameterRemoved(const QString& name);
    void shaderChanged(const QString& newShader);
    void showAdvancedToggled(bool show);
    void browseTextureRequested(const QString& paramName, const QString& currentValue);

private slots:
    void onShaderComboChanged(const QString& text);
    void onAdvancedCheckBoxToggled(bool checked);
    void onFilterTextChanged(const QString& filter);

private:
    void rebuildRows(const std::vector<ui::PanelGroup>& groups);

    QComboBox* m_shaderCombo = nullptr;
    QCheckBox* m_advancedCheck = nullptr;
    QLineEdit* m_filterEdit = nullptr;
    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_scrollContent = nullptr;
    QVBoxLayout* m_scrollLayout = nullptr;

    bool m_updating = false;
    QString m_currentFilter;
    std::vector<ui::PanelGroup> m_lastGroups;
};

} // namespace qt
} // namespace ui
