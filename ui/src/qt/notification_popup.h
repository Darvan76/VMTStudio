#pragma once

#include <QWidget>
#include <QString>
#include <QTimer>
#include <QLabel>
#include <QPushButton>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>

namespace ui {
namespace qt {

enum class NotificationSeverity {
    Info,
    Warning
};

class NotificationPopup : public QWidget {
    Q_OBJECT

public:
    explicit NotificationPopup(QWidget* parent = nullptr);
    ~NotificationPopup() override = default;

    void showMessage(const QString& text, NotificationSeverity severity = NotificationSeverity::Info, int autoDismissMs = 6000);
    void reposition(QWidget* targetViewport);
    void dismiss();

protected:
    void mousePressEvent(QMouseEvent* event) override;

private slots:
    void onFadeOutFinished();

private:
    QFrame* m_card = nullptr;
    QLabel* m_iconLabel = nullptr;
    QLabel* m_textLabel = nullptr;
    QPushButton* m_closeBtn = nullptr;
    QTimer* m_dismissTimer = nullptr;
    QGraphicsOpacityEffect* m_opacityEffect = nullptr;
    QPropertyAnimation* m_fadeAnim = nullptr;
    bool m_isFadingOut = false;
};

} // namespace qt
} // namespace ui
