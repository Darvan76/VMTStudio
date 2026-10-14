#include "notification_popup.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStyle>
#include <QApplication>
#include <QMouseEvent>
#include <algorithm>

namespace ui {
namespace qt {

NotificationPopup::NotificationPopup(QWidget* parent)
    : QWidget(parent) {
    setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);

    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(6, 6, 6, 6);

    m_card = new QFrame(this);
    m_card->setObjectName("NotificationCard");

    auto* cardLayout = new QHBoxLayout(m_card);
    cardLayout->setContentsMargins(14, 10, 14, 10);
    cardLayout->setSpacing(10);

    m_iconLabel = new QLabel(m_card);
    m_iconLabel->setFixedSize(20, 20);
    m_iconLabel->setScaledContents(true);
    cardLayout->addWidget(m_iconLabel);

    m_textLabel = new QLabel(m_card);
    m_textLabel->setTextFormat(Qt::RichText);
    m_textLabel->setStyleSheet("color: #f1f1f1; font-size: 13px; font-weight: normal;");
    cardLayout->addWidget(m_textLabel, 1);

    m_closeBtn = new QPushButton("✕", m_card);
    m_closeBtn->setFixedSize(20, 20);
    m_closeBtn->setCursor(Qt::PointingHandCursor);
    m_closeBtn->setStyleSheet(
        "QPushButton { background: transparent; border: none; color: #888888; font-size: 13px; font-weight: bold; border-radius: 3px; }"
        "QPushButton:hover { color: #ffffff; background-color: #3e3e42; }"
    );
    connect(m_closeBtn, &QPushButton::clicked, this, &NotificationPopup::dismiss);
    cardLayout->addWidget(m_closeBtn);

    outerLayout->addWidget(m_card);

    m_opacityEffect = new QGraphicsOpacityEffect(this);
    setGraphicsEffect(m_opacityEffect);
    m_opacityEffect->setOpacity(1.0);

    m_dismissTimer = new QTimer(this);
    m_dismissTimer->setSingleShot(true);
    connect(m_dismissTimer, &QTimer::timeout, this, &NotificationPopup::dismiss);

    m_fadeAnim = new QPropertyAnimation(m_opacityEffect, "opacity", this);
    connect(m_fadeAnim, &QPropertyAnimation::finished, this, &NotificationPopup::onFadeOutFinished);

    hide();
}

void NotificationPopup::showMessage(const QString& text, NotificationSeverity severity, int autoDismissMs) {
    m_dismissTimer->stop();
    m_fadeAnim->stop();
    m_isFadingOut = false;

    if (severity == NotificationSeverity::Warning) {
        m_card->setStyleSheet(
            "QFrame#NotificationCard {"
            "    background-color: #25221b;"
            "    border: 1px solid #4a3e20;"
            "    border-left: 5px solid #cca700;"
            "    border-radius: 6px;"
            "}"
        );
        m_iconLabel->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(20, 20));
    } else {
        m_card->setStyleSheet(
            "QFrame#NotificationCard {"
            "    background-color: #1e2025;"
            "    border: 1px solid #3c3f48;"
            "    border-left: 5px solid #007acc;"
            "    border-radius: 6px;"
            "}"
        );
        m_iconLabel->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxInformation).pixmap(20, 20));
    }

    m_textLabel->setText(text);
    adjustSize();

    m_opacityEffect->setOpacity(1.0);
    show();
    raise();

    if (autoDismissMs > 0) {
        m_dismissTimer->start(autoDismissMs);
    }
}

void NotificationPopup::reposition(QWidget* targetViewport) {
    if (!targetViewport || !targetViewport->isVisible()) return;

    QPoint gp = targetViewport->mapToGlobal(QPoint(0, 0));
    adjustSize();
    int w = width();
    int h = height();
    int x = gp.x() + (targetViewport->width() - w) / 2;
    int y = gp.y() + targetViewport->height() - h - 20;

    if (x < gp.x() + 8) {
        x = gp.x() + 8;
    }
    move(x, y);
}

void NotificationPopup::dismiss() {
    if (!isVisible() || m_isFadingOut) return;

    m_dismissTimer->stop();
    m_isFadingOut = true;
    m_fadeAnim->stop();
    m_fadeAnim->setDuration(220);
    m_fadeAnim->setStartValue(m_opacityEffect->opacity());
    m_fadeAnim->setEndValue(0.0);
    m_fadeAnim->start();
}

void NotificationPopup::onFadeOutFinished() {
    if (m_isFadingOut) {
        m_isFadingOut = false;
        hide();
        m_opacityEffect->setOpacity(1.0);
    }
}

void NotificationPopup::mousePressEvent(QMouseEvent* event) {
    Q_UNUSED(event);
    dismiss();
}

} // namespace qt
} // namespace ui
