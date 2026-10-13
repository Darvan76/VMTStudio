#pragma once

#include <QDialog>

namespace ui {
namespace qt {

class AboutDialog : public QDialog {
    Q_OBJECT

public:
    explicit AboutDialog(QWidget* parent = nullptr);
    ~AboutDialog() override = default;
};

} // namespace qt
} // namespace ui
