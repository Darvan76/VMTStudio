#include "model_materials_dock.h"

#include "ui/model_scene.h"

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QShortcut>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QVariant>

namespace ui {
namespace qt {

namespace {

// Column layout of the material tree.
enum MaterialColumn {
    MaterialNameColumn = 0,
    ShaderColumn = 1,
    UsedByColumn = 2,
};

} // namespace

ModelMaterialsDock::ModelMaterialsDock(QWidget* parent) : QDockWidget(tr("Model Materials"), parent) {
    setObjectName("ModelMaterialsDock");
    // Keeps the dock usable when it is squeezed next to the viewport.
    setMinimumWidth(240);
    buildUi();
    updateHeader();
}

void ModelMaterialsDock::setScene(const bridge::ModelScene& scene) {
    m_scene = scene;
    // An empty model dump is the dock's hint state: no groups, no rows.
    m_groups = !m_scene.model_name.empty() ? ui::build_material_groups(m_scene)
                                           : std::vector<ui::ModelMaterialGroup>{};
    rebuildTree();
    resetSelection();
    updateHeader();
}

void ModelMaterialsDock::clear() {
    m_scene = bridge::ModelScene{};
    m_groups.clear();
    {
        // A fresh empty state starts without a stale filter.
        const QSignalBlocker blocker(m_searchEdit);
        m_searchEdit->clear();
    }
    rebuildTree();
    resetSelection();
    updateHeader();
}

QString ModelMaterialsDock::currentMaterial() const {
    return selectedMaterialName();
}

void ModelMaterialsDock::selectNextMaterial() {
    stepMaterial(1);
}

void ModelMaterialsDock::selectPreviousMaterial() {
    stepMaterial(-1);
}

bool ModelMaterialsDock::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_tree && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            activateCurrentItem();
            return true;
        }
    }
    return QDockWidget::eventFilter(watched, event);
}

void ModelMaterialsDock::buildUi() {
    auto* content = new QWidget(this);
    setWidget(content);

    auto* mainLayout = new QVBoxLayout(content);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    m_headerLabel = new QLabel(content);
    m_headerLabel->setWordWrap(true);
    mainLayout->addWidget(m_headerLabel);

    m_searchEdit = new QLineEdit(content);
    m_searchEdit->setPlaceholderText(tr("Filter materials..."));
    m_searchEdit->setClearButtonEnabled(true);
    mainLayout->addWidget(m_searchEdit);

    auto* toolRow = new QHBoxLayout();
    toolRow->setSpacing(6);

    m_previousButton = new QPushButton(tr("◀ Prev"), content);
    m_previousButton->setToolTip(tr("Select the previous material (Alt+Left)"));
    m_nextButton = new QPushButton(tr("Next ▶"), content);
    m_nextButton->setToolTip(tr("Select the next material (Alt+Right)"));
    toolRow->addWidget(m_previousButton);
    toolRow->addWidget(m_nextButton);
    toolRow->addStretch(1);

    m_exportAllButton = new QPushButton(tr("Export All..."), content);
    m_exportSelectedButton = new QPushButton(tr("Export Selected..."), content);
    m_isolateButton = new QPushButton(tr("Isolate"), content);
    m_isolateButton->setCheckable(true);
    m_isolateButton->setToolTip(tr("Show only the selected material"));
    toolRow->addWidget(m_exportAllButton);
    toolRow->addWidget(m_exportSelectedButton);
    toolRow->addWidget(m_isolateButton);
    mainLayout->addLayout(toolRow);

    m_tree = new QTreeWidget(content);
    m_tree->setHeaderLabels({tr("Material"), tr("Shader"), tr("Used by")});
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->header()->setSectionResizeMode(MaterialNameColumn, QHeaderView::Stretch);
    m_tree->header()->resizeSection(ShaderColumn, 90);
    m_tree->header()->resizeSection(UsedByColumn, 110);
    m_tree->installEventFilter(this);
    mainLayout->addWidget(m_tree, 1);

    m_hintLabel = new QLabel(tr("Load a model to list its materials"), content);
    m_hintLabel->setAlignment(Qt::AlignCenter);
    m_hintLabel->setWordWrap(true);
    m_hintLabel->setStyleSheet("color: #888888; font-size: 8.5pt;");
    mainLayout->addWidget(m_hintLabel, 1);

    connect(m_searchEdit, &QLineEdit::textChanged, this, &ModelMaterialsDock::onSearchTextChanged);
    connect(m_previousButton, &QPushButton::clicked, this,
            &ModelMaterialsDock::selectPreviousMaterial);
    connect(m_nextButton, &QPushButton::clicked, this, &ModelMaterialsDock::selectNextMaterial);
    connect(m_exportAllButton, &QPushButton::clicked, this, &ModelMaterialsDock::onExportAllClicked);
    connect(m_exportSelectedButton, &QPushButton::clicked, this,
            &ModelMaterialsDock::onExportSelectedClicked);
    connect(m_isolateButton, &QPushButton::toggled, this, &ModelMaterialsDock::onIsolateToggled);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            &ModelMaterialsDock::onSelectionChanged);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this,
            &ModelMaterialsDock::onItemDoubleClicked);
    connect(m_tree, &QWidget::customContextMenuRequested, this,
            &ModelMaterialsDock::onTreeContextMenu);

    auto* nextShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Right), this);
    nextShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(nextShortcut, &QShortcut::activated, this, &ModelMaterialsDock::selectNextMaterial);

    auto* previousShortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Left), this);
    previousShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(previousShortcut, &QShortcut::activated, this,
            &ModelMaterialsDock::selectPreviousMaterial);
}

// --- tree population ----------------------------------------------------------

void ModelMaterialsDock::rebuildTree() {
    {
        // Replaced rows must not fire selection handlers; the dropped selection
        // is reported exactly once by resetSelection().
        const QSignalBlocker blocker(m_tree);
        m_tree->clear();

        for (const ui::ModelMaterialGroup& group : m_groups) {
            auto* groupItem = new QTreeWidgetItem(m_tree, {QString::fromStdString(group.name)});
            groupItem->setFlags(groupItem->flags() & ~Qt::ItemIsSelectable & ~Qt::ItemIsEditable);
            groupItem->setExpanded(true);

            for (const ui::ModelMaterialRow& row : group.rows) {
                const QString gameName = QString::fromStdString(row.name);
                const QString displayName =
                    row.display_name.empty() ? QString::fromStdString(ui::material_display_name(row.name))
                                             : QString::fromStdString(row.display_name);
                const QString shader = QString::fromStdString(row.shader);
                auto* item = new QTreeWidgetItem(
                    groupItem,
                    {displayName, shader, QString::fromStdString(row.used_by)});
                item->setData(MaterialNameColumn, Qt::UserRole, gameName);
                QString tooltip = gameName;
                if (!shader.isEmpty()) {
                    tooltip += QLatin1Char('\n') + shader;
                }
                item->setToolTip(MaterialNameColumn, tooltip);
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            }
        }
    }

    applyFilter(m_searchEdit->text());
}

void ModelMaterialsDock::applyFilter(const QString& text) {
    const QString needle = text.trimmed();
    const int groupCount = m_tree->topLevelItemCount();
    for (int g = 0; g < groupCount; ++g) {
        QTreeWidgetItem* group = m_tree->topLevelItem(g);
        bool anyVisible = false;
        const int rowCount = group->childCount();
        for (int c = 0; c < rowCount; ++c) {
            QTreeWidgetItem* item = group->child(c);
            const bool matches =
                needle.isEmpty() ||
                item->text(MaterialNameColumn).contains(needle, Qt::CaseInsensitive) ||
                item->data(MaterialNameColumn, Qt::UserRole)
                    .toString()
                    .contains(needle, Qt::CaseInsensitive);
            item->setHidden(!matches);
            anyVisible = anyVisible || matches;
        }
        group->setHidden(!anyVisible);
    }
}

void ModelMaterialsDock::updateHeader() {
    const bool hasModel = !m_scene.model_name.empty();

    if (hasModel) {
        const QString modelName = QString::fromStdString(m_scene.model_name);
        m_headerLabel->setText(tr("%1 - %2 materials")
                                   .arg(modelName)
                                   .arg(static_cast<int>(m_scene.materials.size())));
        m_headerLabel->setToolTip(modelName);
    } else {
        m_headerLabel->clear();
        m_headerLabel->setToolTip(QString());
    }
    m_headerLabel->setVisible(hasModel);

    m_searchEdit->setEnabled(hasModel);
    m_tree->setVisible(hasModel);
    m_hintLabel->setVisible(!hasModel);
    updateActions();
}

void ModelMaterialsDock::updateActions() {
    const bool hasRows = !m_groups.empty();
    const bool hasSelection = !selectedMaterialName().isEmpty();

    m_previousButton->setEnabled(hasRows);
    m_nextButton->setEnabled(hasRows);
    m_exportAllButton->setEnabled(hasRows);
    m_exportSelectedButton->setEnabled(hasSelection);
    m_isolateButton->setEnabled(hasSelection);
    if (!hasSelection && m_isolateButton->isChecked()) {
        // Never latch "isolate" without a material behind it.
        suspendIsolate(QString());
    }
}

// --- selection ----------------------------------------------------------------

QString ModelMaterialsDock::selectedMaterialName() const {
    QTreeWidgetItem* current = m_tree->currentItem();
    if (current != nullptr && current->isSelected() &&
        current->data(MaterialNameColumn, Qt::UserRole).isValid()) {
        return current->data(MaterialNameColumn, Qt::UserRole).toString();
    }
    const QList<QTreeWidgetItem*> selected = m_tree->selectedItems();
    for (QTreeWidgetItem* item : selected) {
        if (item->data(MaterialNameColumn, Qt::UserRole).isValid()) {
            return item->data(MaterialNameColumn, Qt::UserRole).toString();
        }
    }
    return {};
}

QTreeWidgetItem* ModelMaterialsDock::findMaterialItem(const QString& name) const {
    const int groupCount = m_tree->topLevelItemCount();
    for (int g = 0; g < groupCount; ++g) {
        QTreeWidgetItem* group = m_tree->topLevelItem(g);
        const int rowCount = group->childCount();
        for (int c = 0; c < rowCount; ++c) {
            QTreeWidgetItem* item = group->child(c);
            if (item->data(MaterialNameColumn, Qt::UserRole).toString() == name) {
                return item;
            }
        }
    }
    return nullptr;
}

void ModelMaterialsDock::resetSelection() {
    {
        const QSignalBlocker blocker(m_tree);
        m_tree->clearSelection();
        m_tree->setCurrentItem(nullptr);
    }

    const QString previous = m_selectedMaterial;
    m_selectedMaterial.clear();
    suspendIsolate(previous);
    if (!previous.isEmpty()) {
        emit materialSelected(QString());
    }
    updateActions();
}

void ModelMaterialsDock::suspendIsolate(const QString& material) {
    if (!m_isolateButton->isChecked()) {
        return;
    }
    {
        const QSignalBlocker blocker(m_isolateButton);
        m_isolateButton->setChecked(false);
    }
    if (!material.isEmpty()) {
        emit isolateToggled(material, false);
    }
}

void ModelMaterialsDock::onSelectionChanged() {
    const QString selected = selectedMaterialName();
    if (selected == m_selectedMaterial) {
        updateActions();
        return;
    }

    // Isolating is a per-material latch: moving to a different material (or to
    // nothing) releases it explicitly so the main window can show the whole
    // model again.
    const QString previous = m_selectedMaterial;
    m_selectedMaterial = selected;
    suspendIsolate(previous);
    emit materialSelected(selected);
    updateActions();
}

void ModelMaterialsDock::onIsolateToggled(bool checked) {
    const QString name = selectedMaterialName();
    if (name.isEmpty()) {
        suspendIsolate(name);
        return;
    }
    emit isolateToggled(name, checked);
}

void ModelMaterialsDock::stepMaterial(int delta) {
    const std::vector<const ui::ModelMaterialRow*> rows = ui::flatten_material_rows(m_groups);
    if (rows.empty()) {
        return;
    }

    const int count = static_cast<int>(rows.size());
    const QString current = selectedMaterialName();
    int index = -1;
    for (int i = 0; i < count; ++i) {
        if (QString::fromStdString(rows[static_cast<std::size_t>(i)]->name) == current) {
            index = i;
            break;
        }
    }

    int next = 0;
    if (index < 0) {
        next = delta > 0 ? 0 : count - 1;
    } else {
        next = (index + delta + count) % count;
    }
    selectMaterialByName(QString::fromStdString(rows[static_cast<std::size_t>(next)]->name));
}

void ModelMaterialsDock::selectMaterialByName(const QString& name) {
    QTreeWidgetItem* item = findMaterialItem(name);
    if (item == nullptr) {
        return;
    }

    const bool wasCurrent = (m_selectedMaterial == name);
    m_tree->setCurrentItem(item);
    m_tree->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    if (wasCurrent) {
        // Wrapping around a single row (or re-selecting the current one) does
        // not move the selection, so the tree stays silent: report the
        // deliberate step once here.
        emit materialSelected(name);
    }
}

void ModelMaterialsDock::activateCurrentItem() {
    const QString name = selectedMaterialName();
    if (!name.isEmpty()) {
        emit materialActivated(name);
    }
}

// --- widget handlers ----------------------------------------------------------

void ModelMaterialsDock::onSearchTextChanged(const QString& text) {
    applyFilter(text);
}

void ModelMaterialsDock::onItemDoubleClicked(QTreeWidgetItem* item) {
    if (item == nullptr) {
        return;
    }
    const QVariant material_name = item->data(MaterialNameColumn, Qt::UserRole);
    if (material_name.isValid()) {
        emit materialActivated(material_name.toString());
    }
}

void ModelMaterialsDock::onExportAllClicked() {
    QStringList names;
    for (const ui::ModelMaterialGroup& group : m_groups) {
        for (const ui::ModelMaterialRow& row : group.rows) {
            names.append(QString::fromStdString(row.name));
        }
    }
    if (!names.isEmpty()) {
        emit materialExportRequested(names);
    }
}

void ModelMaterialsDock::onExportSelectedClicked() {
    QStringList names;
    const QList<QTreeWidgetItem*> selected = m_tree->selectedItems();
    for (QTreeWidgetItem* item : selected) {
        const QVariant material_name = item->data(MaterialNameColumn, Qt::UserRole);
        if (material_name.isValid()) {
            names.append(material_name.toString());
        }
    }
    if (!names.isEmpty()) {
        emit materialExportRequested(names);
    }
}

void ModelMaterialsDock::onTreeContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_tree->itemAt(pos);
    if (item == nullptr || !item->data(MaterialNameColumn, Qt::UserRole).isValid()) {
        return; // empty area or a group header
    }

    const QString name = item->data(MaterialNameColumn, Qt::UserRole).toString();
    m_tree->setCurrentItem(item); // keep the selection in sync with the menu

    QMenu menu(this);
    QAction* openAction = menu.addAction(tr("Open in Editor"));
    QAction* exportAction = menu.addAction(tr("Export This Material..."));
    menu.addSeparator();
    QAction* isolateAction = menu.addAction(tr("Isolate"));
    isolateAction->setCheckable(true);
    isolateAction->setChecked(m_isolateButton->isChecked());
    menu.addSeparator();
    QAction* copyPathAction = menu.addAction(tr("Copy Game Path"));
    QAction* copyShaderAction = menu.addAction(tr("Copy Shader Name"));

    const QAction* chosen = menu.exec(m_tree->viewport()->mapToGlobal(pos));
    if (chosen == nullptr) {
        return;
    }
    if (chosen == openAction) {
        emit materialActivated(name);
    } else if (chosen == exportAction) {
        emit materialExportRequested(QStringList{name});
    } else if (chosen == isolateAction) {
        // The checkable item mirrors the button; toggling the button keeps the
        // widget state and the isolateToggled emission in one place.
        m_isolateButton->toggle();
    } else if (chosen == copyPathAction) {
        QGuiApplication::clipboard()->setText(name);
    } else if (chosen == copyShaderAction) {
        QGuiApplication::clipboard()->setText(item->text(ShaderColumn));
    }
}

} // namespace qt
} // namespace ui
