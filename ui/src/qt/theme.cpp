#include "theme.h"

#include <QPalette>
#include <QColor>
#include <QFont>
#include <QStyleFactory>

namespace ui {
namespace qt {

void apply_dark_theme(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));

    QFont font("Segoe UI", 9);
    font.setStyleHint(QFont::SansSerif);
    app.setFont(font);

    QPalette dark;
    dark.setColor(QPalette::Window, QColor(30, 30, 30));
    dark.setColor(QPalette::WindowText, QColor(220, 220, 220));
    dark.setColor(QPalette::Base, QColor(24, 24, 24));
    dark.setColor(QPalette::AlternateBase, QColor(35, 35, 35));
    dark.setColor(QPalette::ToolTipBase, QColor(45, 45, 45));
    dark.setColor(QPalette::ToolTipText, QColor(240, 240, 240));
    dark.setColor(QPalette::Text, QColor(220, 220, 220));
    dark.setColor(QPalette::Button, QColor(45, 45, 48));
    dark.setColor(QPalette::ButtonText, QColor(220, 220, 220));
    dark.setColor(QPalette::BrightText, QColor(255, 128, 0)); // Source orange
    dark.setColor(QPalette::Link, QColor(0, 122, 204));
    dark.setColor(QPalette::Highlight, QColor(0, 122, 204));
    dark.setColor(QPalette::HighlightedText, QColor(255, 255, 255));

    dark.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 120, 120));
    dark.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(120, 120, 120));
    dark.setColor(QPalette::Disabled, QPalette::WindowText, QColor(120, 120, 120));

    app.setPalette(dark);

    const QString stylesheet = R"(
        QMainWindow {
            background-color: #1e1e1e;
        }
        QMenuBar {
            background-color: #252526;
            color: #d4d4d4;
            border-bottom: 1px solid #333333;
        }
        QMenuBar::item:selected {
            background-color: #3e3e42;
        }
        QMenu {
            background-color: #252526;
            color: #d4d4d4;
            border: 1px solid #3f3f46;
            padding: 4px;
        }
        QMenu::item {
            padding: 4px 24px 4px 20px;
            border-radius: 2px;
        }
        QMenu::item:selected {
            background-color: #094771;
            color: #ffffff;
        }
        QMenu::separator {
            height: 1px;
            background-color: #3f3f46;
            margin: 4px 8px;
        }
        QToolBar {
            background-color: #252526;
            border-bottom: 1px solid #333333;
            spacing: 6px;
            padding: 3px 6px;
        }
        QToolButton {
            background-color: transparent;
            color: #d4d4d4;
            border: 1px solid transparent;
            border-radius: 3px;
            padding: 4px 8px;
        }
        QToolButton:hover {
            background-color: #3e3e42;
            border-color: #555555;
        }
        QToolButton:pressed {
            background-color: #007acc;
            color: #ffffff;
        }
        QStatusBar {
            background-color: #007acc;
            color: #ffffff;
            font-size: 8.5pt;
        }
        QSplitter::handle {
            background-color: #2d2d30;
        }
        QSplitter::handle:hover {
            background-color: #007acc;
        }
        QScrollBar:vertical {
            background-color: #1e1e1e;
            width: 12px;
            margin: 0px;
        }
        QScrollBar::handle:vertical {
            background-color: #424242;
            min-height: 20px;
            border-radius: 4px;
            margin: 2px;
        }
        QScrollBar::handle:vertical:hover {
            background-color: #686868;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0px;
        }
        QScrollBar:horizontal {
            background-color: #1e1e1e;
            height: 12px;
            margin: 0px;
        }
        QScrollBar::handle:horizontal {
            background-color: #424242;
            min-width: 20px;
            border-radius: 4px;
            margin: 2px;
        }
        QScrollBar::handle:horizontal:hover {
            background-color: #686868;
        }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
            width: 0px;
        }
        QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QDoubleSpinBox {
            background-color: #1e1e1e;
            color: #d4d4d4;
            border: 1px solid #3e3e42;
            border-radius: 3px;
            padding: 3px 6px;
            selection-background-color: #264f78;
        }
        QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus {
            border: 1px solid #007acc;
        }
        QPushButton {
            background-color: #333337;
            color: #d4d4d4;
            border: 1px solid #3e3e42;
            border-radius: 3px;
            padding: 5px 12px;
            min-width: 60px;
        }
        QPushButton:hover {
            background-color: #3e3e42;
            border-color: #555555;
        }
        QPushButton:pressed {
            background-color: #007acc;
            color: #ffffff;
            border-color: #007acc;
        }
        QPushButton:disabled {
            background-color: #252526;
            color: #707070;
            border-color: #333333;
        }
        QComboBox {
            background-color: #2d2d30;
            color: #d4d4d4;
            border: 1px solid #3e3e42;
            border-radius: 3px;
            padding: 3px 8px;
            min-height: 20px;
        }
        QComboBox:hover {
            border-color: #555555;
        }
        QComboBox::drop-down {
            subcontrol-origin: padding;
            subcontrol-position: top right;
            width: 18px;
            border-left: 1px solid #3e3e42;
        }
        QComboBox QAbstractItemView {
            background-color: #252526;
            color: #d4d4d4;
            border: 1px solid #3e3e42;
            selection-background-color: #094771;
        }
        QSlider::groove:horizontal {
            border: 1px solid #3e3e42;
            height: 4px;
            background: #252526;
            border-radius: 2px;
        }
        QSlider::sub-page:horizontal {
            background: #007acc;
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            background: #d4d4d4;
            border: 1px solid #555555;
            width: 12px;
            margin-top: -5px;
            margin-bottom: -5px;
            border-radius: 6px;
        }
        QSlider::handle:horizontal:hover {
            background: #ffffff;
            border-color: #007acc;
        }
        QGroupBox {
            font-weight: bold;
            color: #e0e0e0;
            border: 1px solid #333337;
            border-radius: 4px;
            margin-top: 10px;
            padding-top: 12px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            subcontrol-position: top left;
            left: 10px;
            padding: 0 4px;
            background-color: #1e1e1e;
        }
        QTreeWidget, QTreeView, QListWidget, QListView, QTableWidget, QTableView {
            background-color: #252526;
            color: #d4d4d4;
            border: 1px solid #333337;
            border-radius: 2px;
            selection-background-color: #094771;
            selection-color: #ffffff;
        }
        QHeaderView::section {
            background-color: #2d2d30;
            color: #d4d4d4;
            padding: 4px;
            border: 1px solid #333337;
        }
        QCheckBox, QRadioButton {
            color: #d4d4d4;
            spacing: 6px;
        }
        QCheckBox::indicator, QRadioButton::indicator {
            width: 14px;
            height: 14px;
            border: 1px solid #3e3e42;
            border-radius: 2px;
            background-color: #1e1e1e;
        }
        QCheckBox::indicator:hover, QRadioButton::indicator:hover {
            border-color: #007acc;
        }
        QCheckBox::indicator:checked {
            background-color: #007acc;
            border-color: #007acc;
        }
        QTabWidget::pane {
            border: 1px solid #333337;
            background-color: #1e1e1e;
        }
        QTabBar::tab {
            background-color: #2d2d30;
            color: #969696;
            padding: 6px 14px;
            border-top-left-radius: 3px;
            border-top-right-radius: 3px;
            border: 1px solid #333337;
            border-bottom: none;
            margin-right: 2px;
        }
        QTabBar::tab:selected {
            background-color: #1e1e1e;
            color: #ffffff;
            border-color: #333337;
        }
        QTabBar::tab:hover:!selected {
            background-color: #383838;
            color: #d4d4d4;
        }
    )";

    app.setStyleSheet(stylesheet);
}

} // namespace qt
} // namespace ui
