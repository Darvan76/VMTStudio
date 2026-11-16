#include "inspector_widget.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QScopedValueRollback>
#include <QSignalBlocker>

#include <utility>

namespace ui {
namespace qt {
namespace {

bool sameStructure(const std::vector<ui::PanelGroup>& previous,
                   const std::vector<ui::PanelGroup>& next) {
    if (previous.size() != next.size()) return false;
    for (std::size_t group = 0; group < next.size(); ++group) {
        if (previous[group].name != next[group].name ||
            previous[group].rows.size() != next[group].rows.size()) return false;
        for (std::size_t index = 0; index < next[group].rows.size(); ++index) {
            const auto& a = previous[group].rows[index];
            const auto& b = next[group].rows[index];
            if (a.name != b.name || a.kind != b.kind || a.type != b.type ||
                a.has_range != b.has_range || a.min_value != b.min_value ||
                a.max_value != b.max_value || a.enum_values != b.enum_values ||
                a.description != b.description || a.warning != b.warning) return false;
        }
    }
    return true;
}

} // namespace

InspectorWidget::InspectorWidget(QWidget* parent)
    : QWidget(parent) {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    mainLayout->setSpacing(6);

    // Top control bar: Shader selector & filter
    auto* headerLayout = new QHBoxLayout();
    headerLayout->addWidget(new QLabel(tr("Shader:"), this));

    m_shaderCombo = new QComboBox(this);
    m_shaderCombo->setEditable(true);
    m_shaderCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    headerLayout->addWidget(m_shaderCombo);
    mainLayout->addLayout(headerLayout);

    auto* filterLayout = new QHBoxLayout();
    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setObjectName(QStringLiteral("parameterFilter"));
    m_filterEdit->setPlaceholderText(tr("Search parameters..."));
    m_filterEdit->setClearButtonEnabled(true);
    filterLayout->addWidget(m_filterEdit);

    m_advancedCheck = new QCheckBox(tr("Show advanced"), this);
    filterLayout->addWidget(m_advancedCheck);
    mainLayout->addLayout(filterLayout);

    // Scrollable area for parameter groups
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);

    m_scrollContent = new QWidget(m_scrollArea);
    m_scrollLayout = new QVBoxLayout(m_scrollContent);
    m_scrollLayout->setContentsMargins(0, 4, 4, 4);
    m_scrollLayout->setSpacing(8);
    m_scrollArea->setWidget(m_scrollContent);

    mainLayout->addWidget(m_scrollArea);

    // Connect signals
    connect(m_shaderCombo, &QComboBox::currentTextChanged,
            this, &InspectorWidget::onShaderComboChanged);
    connect(m_advancedCheck, &QCheckBox::toggled,
            this, &InspectorWidget::onAdvancedCheckBoxToggled);
    connect(m_filterEdit, &QLineEdit::textChanged,
            this, &InspectorWidget::onFilterTextChanged);
}

void InspectorWidget::setAvailableShaders(const QStringList& shaders) {
    const QScopedValueRollback<bool> updating(m_updating, true);
    m_shaderCombo->clear();
    m_shaderCombo->addItems(shaders);
}

void InspectorWidget::setCurrentShader(const QString& shader) {
    const QScopedValueRollback<bool> updating(m_updating, true);
    int idx = m_shaderCombo->findText(shader, Qt::MatchFixedString);
    if (idx >= 0) {
        m_shaderCombo->setCurrentIndex(idx);
    } else {
        m_shaderCombo->setEditText(shader);
    }
}

void InspectorWidget::onShaderComboChanged(const QString& text) {
    if (m_updating) return;
    emit shaderChanged(text);
}

void InspectorWidget::onAdvancedCheckBoxToggled(bool checked) {
    if (m_updating) return;
    emit showAdvancedToggled(checked);
}

void InspectorWidget::onFilterTextChanged(const QString& filter) {
    m_currentFilter = filter.trimmed().toLower();
    applyFilter();
}

void InspectorWidget::updateFromDocument(const vmt::Document* doc,
                                        const vmt::SchemaLibrary& schemas,
                                        bool showAdvanced) {
    const QScopedValueRollback<bool> updating(m_updating, true);
    m_advancedCheck->setChecked(showAdvanced);

    std::string shaderName = (doc != nullptr) ? std::string(doc->shader()) : "LightmappedGeneric";
    if (shaderName.empty()) shaderName = "LightmappedGeneric";
    setCurrentShader(QString::fromStdString(shaderName));

    vmt::ShaderSchema schema = schemas.schema_for(shaderName);

    auto groups = ui::build_panel_model(doc, schema, showAdvanced);
    if (m_lastShader != shaderName || !sameStructure(m_lastGroups, groups)) {
        rebuildRows(groups);
        m_lastGroups = std::move(groups);
        m_lastShader = std::move(shaderName);
        applyFilter();
    } else {
        updateRowValues(groups);
        m_lastGroups = std::move(groups);
    }
}

void InspectorWidget::rebuildRows(const std::vector<ui::PanelGroup>& groups) {
    m_rows.clear();
    m_groups.clear();
    // Clear old items
    QLayoutItem* item = nullptr;
    while ((item = m_scrollLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }

    for (const auto& group : groups) {
        auto* groupBox = new QGroupBox(QString::fromStdString(group.name), m_scrollContent);
        m_groups.push_back(groupBox);
        auto* formLayout = new QFormLayout(groupBox);
        formLayout->setContentsMargins(8, 8, 8, 8);
        formLayout->setSpacing(6);
        formLayout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        for (const auto& row : group.rows) {
            QString rowName = QString::fromStdString(row.name);
            QString rowVal = QString::fromStdString(row.value);

            auto* label = new QLabel(rowName, groupBox);
            QString tooltip = QString::fromStdString(row.description);
            if (!row.warning.empty()) {
                if (!tooltip.isEmpty()) tooltip += "\n";
                tooltip += QString::fromStdString(row.warning);
            }
            if (!tooltip.isEmpty()) {
                label->setToolTip(tooltip);
            }

            if (row.type == vmt::ParamType::Bool) {
                auto* check = new QCheckBox(groupBox);
                check->setChecked(rowVal == "1" || rowVal.compare("true", Qt::CaseInsensitive) == 0);
                if (!tooltip.isEmpty()) check->setToolTip(tooltip);
                connect(check, &QCheckBox::toggled, this, [this, rowName](bool checked) {
                    if (!m_updating) emit parameterChanged(rowName, checked ? "1" : "0");
                });
                formLayout->addRow(label, check);
                m_rows.push_back({check, check});
            } else if (row.type == vmt::ParamType::Texture) {
                auto* rowWidget = new QWidget(groupBox);
                auto* hBox = new QHBoxLayout(rowWidget);
                hBox->setContentsMargins(0, 0, 0, 0);
                hBox->setSpacing(4);

                auto* edit = new QLineEdit(rowVal, rowWidget);
                if (!tooltip.isEmpty()) edit->setToolTip(tooltip);
                connect(edit, &QLineEdit::textEdited, this, [this, rowName](const QString& text) {
                    if (!m_updating) emit parameterChanged(rowName, text);
                });
                hBox->addWidget(edit);

                auto* browseBtn = new QPushButton("...", rowWidget);
                browseBtn->setMaximumWidth(28);
                browseBtn->setToolTip(tr("Browse or pick texture"));
                connect(browseBtn, &QPushButton::clicked, this, [this, rowName, edit]() {
                    emit browseTextureRequested(rowName, edit->text());
                });
                hBox->addWidget(browseBtn);

                formLayout->addRow(label, rowWidget);
                m_rows.push_back({edit, rowWidget});
            } else if (row.type == vmt::ParamType::Enum && !row.enum_values.empty()) {
                auto* combo = new QComboBox(groupBox);
                for (const auto& ev : row.enum_values) {
                    combo->addItem(QString::fromStdString(ev));
                }
                combo->setPlaceholderText(rowVal);
                combo->setCurrentIndex(combo->findText(rowVal, Qt::MatchFixedString));
                if (!tooltip.isEmpty()) combo->setToolTip(tooltip);
                connect(combo, &QComboBox::currentTextChanged, this, [this, rowName](const QString& text) {
                    if (!m_updating) emit parameterChanged(rowName, text);
                });
                formLayout->addRow(label, combo);
                m_rows.push_back({combo, combo});
            } else if ((row.type == vmt::ParamType::Float || row.type == vmt::ParamType::Int) && row.has_range) {
                auto* rowWidget = new QWidget(groupBox);
                auto* hBox = new QHBoxLayout(rowWidget);
                hBox->setContentsMargins(0, 0, 0, 0);
                hBox->setSpacing(4);

                auto* slider = new QSlider(Qt::Horizontal, rowWidget);
                slider->setObjectName(rowName + QStringLiteral(":slider"));
                slider->setRange(0, 100);

                double val = rowVal.toDouble();
                double norm = (row.max_value > row.min_value) ?
                    (val - row.min_value) / (row.max_value - row.min_value) : 0.0;
                slider->setValue(static_cast<int>(norm * 100.0));

                auto* spin = new QDoubleSpinBox(rowWidget);
                spin->setRange(row.min_value, row.max_value);
                spin->setValue(val);
                spin->setSingleStep((row.max_value - row.min_value) / 20.0);
                spin->setMaximumWidth(70);

                connect(slider, &QSlider::valueChanged, this, [this, rowName, spin, row](int sVal) {
                    if (m_updating) return;
                    double calcVal = row.min_value + (sVal / 100.0) * (row.max_value - row.min_value);
                    {
                        const QSignalBlocker blockSpin(spin);
                        spin->setValue(calcVal);
                    }
                    emit parameterChanged(rowName, QString::number(calcVal, 'g', 3));
                });

                connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                        this, [this, rowName, slider, row](double dVal) {
                    if (m_updating) return;
                    double normVal = (row.max_value > row.min_value) ?
                        (dVal - row.min_value) / (row.max_value - row.min_value) : 0.0;
                    {
                        const QSignalBlocker blockSlider(slider);
                        slider->setValue(static_cast<int>(normVal * 100.0));
                    }
                    emit parameterChanged(rowName, QString::number(dVal, 'g', 3));
                });

                hBox->addWidget(slider);
                hBox->addWidget(spin);
                formLayout->addRow(label, rowWidget);
                m_rows.push_back({spin, rowWidget, slider});
            } else {
                auto* edit = new QLineEdit(rowVal, groupBox);
                if (!tooltip.isEmpty()) edit->setToolTip(tooltip);
                connect(edit, &QLineEdit::textEdited, this, [this, rowName](const QString& text) {
                    if (!m_updating) emit parameterChanged(rowName, text);
                });
                formLayout->addRow(label, edit);
                m_rows.push_back({edit, edit});
            }
            m_rows.back().editor->setObjectName(rowName);
            label->setBuddy(m_rows.back().editor);
        }

        m_scrollLayout->addWidget(groupBox);
    }

    m_scrollLayout->addStretch();
}

void InspectorWidget::updateRowValues(const std::vector<ui::PanelGroup>& groups) {
    std::size_t flatIndex = 0;
    for (std::size_t group = 0; group < groups.size(); ++group) {
        for (std::size_t index = 0; index < groups[group].rows.size(); ++index, ++flatIndex) {
            const auto& row = groups[group].rows[index];
            // Preserve cursor, selection and partial input when the document's
            // effective value did not change (including absent/default rows).
            if (row.value == m_lastGroups[group].rows[index].value) continue;
            const QString value = QString::fromStdString(row.value);
            const auto& widgets = m_rows[flatIndex];
            const QSignalBlocker blockEditor(widgets.editor);
            if (auto* edit = qobject_cast<QLineEdit*>(widgets.editor)) {
                if (edit->text() != value) edit->setText(value);
            } else if (auto* check = qobject_cast<QCheckBox*>(widgets.editor)) {
                check->setChecked(value == "1" || value.compare("true", Qt::CaseInsensitive) == 0);
            } else if (auto* combo = qobject_cast<QComboBox*>(widgets.editor)) {
                combo->setPlaceholderText(value);
                combo->setCurrentIndex(combo->findText(value, Qt::MatchFixedString));
            } else if (auto* spin = qobject_cast<QDoubleSpinBox*>(widgets.editor)) {
                const QSignalBlocker blockSlider(widgets.slider);
                const double number = value.toDouble();
                if (spin->value() != number) spin->setValue(number);
                const double normalized = row.max_value > row.min_value
                    ? (number - row.min_value) / (row.max_value - row.min_value) : 0.0;
                widgets.slider->setValue(static_cast<int>(normalized * 100.0));
            }
        }
    }
}

void InspectorWidget::applyFilter() {
    std::size_t flatIndex = 0;
    for (std::size_t group = 0; group < m_lastGroups.size(); ++group) {
        auto* form = qobject_cast<QFormLayout*>(m_groups[group]->layout());
        bool anyVisible = false;
        for (const auto& row : m_lastGroups[group].rows) {
            const bool visible = m_currentFilter.isEmpty() ||
                QString::fromStdString(row.name).contains(m_currentFilter, Qt::CaseInsensitive) ||
                QString::fromStdString(row.description).contains(m_currentFilter, Qt::CaseInsensitive);
            form->setRowVisible(m_rows[flatIndex++].field, visible);
            anyVisible = anyVisible || visible;
        }
        m_groups[group]->setVisible(anyVisible);
    }
}

} // namespace qt
} // namespace ui
