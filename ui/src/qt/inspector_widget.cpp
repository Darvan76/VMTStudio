#include "inspector_widget.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QToolTip>

namespace ui {
namespace qt {

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
    m_updating = true;
    m_shaderCombo->clear();
    m_shaderCombo->addItems(shaders);
    m_updating = false;
}

void InspectorWidget::setCurrentShader(const QString& shader) {
    m_updating = true;
    int idx = m_shaderCombo->findText(shader, Qt::MatchFixedString);
    if (idx >= 0) {
        m_shaderCombo->setCurrentIndex(idx);
    } else {
        m_shaderCombo->setEditText(shader);
    }
    m_updating = false;
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
    rebuildRows(m_lastGroups);
}

void InspectorWidget::updateFromDocument(const vmt::Document* doc,
                                        const vmt::SchemaLibrary& schemas,
                                        bool showAdvanced) {
    m_updating = true;
    m_advancedCheck->setChecked(showAdvanced);

    std::string shaderName = (doc != nullptr) ? std::string(doc->shader()) : "LightmappedGeneric";
    if (shaderName.empty()) shaderName = "LightmappedGeneric";
    setCurrentShader(QString::fromStdString(shaderName));

    vmt::ShaderSchema schema = schemas.schema_for(shaderName);

    m_lastGroups = ui::build_panel_model(doc, schema, showAdvanced);
    m_updating = false;

    rebuildRows(m_lastGroups);
}

void InspectorWidget::rebuildRows(const std::vector<ui::PanelGroup>& groups) {
    // Clear old items
    QLayoutItem* item = nullptr;
    while ((item = m_scrollLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }

    for (const auto& group : groups) {
        // Check filter
        std::vector<const ui::PanelRow*> visibleRows;
        for (const auto& row : group.rows) {
            if (!m_currentFilter.isEmpty()) {
                QString rowName = QString::fromStdString(row.name).toLower();
                QString rowDesc = QString::fromStdString(row.description).toLower();
                if (!rowName.contains(m_currentFilter) && !rowDesc.contains(m_currentFilter)) {
                    continue;
                }
            }
            visibleRows.push_back(&row);
        }

        if (visibleRows.empty()) {
            continue;
        }

        auto* groupBox = new QGroupBox(QString::fromStdString(group.name), m_scrollContent);
        auto* formLayout = new QFormLayout(groupBox);
        formLayout->setContentsMargins(8, 8, 8, 8);
        formLayout->setSpacing(6);
        formLayout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        for (const auto* rowPtr : visibleRows) {
            const auto& row = *rowPtr;
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
            } else if (row.type == vmt::ParamType::Enum && !row.enum_values.empty()) {
                auto* combo = new QComboBox(groupBox);
                for (const auto& ev : row.enum_values) {
                    combo->addItem(QString::fromStdString(ev));
                }
                int curIdx = combo->findText(rowVal, Qt::MatchFixedString);
                if (curIdx >= 0) combo->setCurrentIndex(curIdx);
                else combo->setEditText(rowVal);
                if (!tooltip.isEmpty()) combo->setToolTip(tooltip);
                connect(combo, &QComboBox::currentTextChanged, this, [this, rowName](const QString& text) {
                    if (!m_updating) emit parameterChanged(rowName, text);
                });
                formLayout->addRow(label, combo);
            } else if ((row.type == vmt::ParamType::Float || row.type == vmt::ParamType::Int) && row.has_range) {
                auto* rowWidget = new QWidget(groupBox);
                auto* hBox = new QHBoxLayout(rowWidget);
                hBox->setContentsMargins(0, 0, 0, 0);
                hBox->setSpacing(4);

                auto* slider = new QSlider(Qt::Horizontal, rowWidget);
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

                connect(slider, &QSlider::valueChanged, this, [this, rowName, slider, spin, row](int sVal) {
                    if (m_updating) return;
                    double calcVal = row.min_value + (sVal / 100.0) * (row.max_value - row.min_value);
                    spin->blockSignals(true);
                    spin->setValue(calcVal);
                    spin->blockSignals(false);
                    emit parameterChanged(rowName, QString::number(calcVal, 'g', 3));
                });

                connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                        this, [this, rowName, slider, row](double dVal) {
                    if (m_updating) return;
                    double normVal = (row.max_value > row.min_value) ?
                        (dVal - row.min_value) / (row.max_value - row.min_value) : 0.0;
                    slider->blockSignals(true);
                    slider->setValue(static_cast<int>(normVal * 100.0));
                    slider->blockSignals(false);
                    emit parameterChanged(rowName, QString::number(dVal, 'g', 3));
                });

                hBox->addWidget(slider);
                hBox->addWidget(spin);
                formLayout->addRow(label, rowWidget);
            } else {
                auto* edit = new QLineEdit(rowVal, groupBox);
                if (!tooltip.isEmpty()) edit->setToolTip(tooltip);
                connect(edit, &QLineEdit::textEdited, this, [this, rowName](const QString& text) {
                    if (!m_updating) emit parameterChanged(rowName, text);
                });
                formLayout->addRow(label, edit);
            }
        }

        m_scrollLayout->addWidget(groupBox);
    }

    m_scrollLayout->addStretch();
}

} // namespace qt
} // namespace ui
