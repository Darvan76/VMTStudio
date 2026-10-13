#include "viewport_widget.h"
#include "bridge/engine_view.h"

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>

namespace ui {
namespace qt {

ViewportWidget::ViewportWidget(QWidget* parent)
    : QWidget(parent) {
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_PaintOnScreen, false);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);

    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(150);
    connect(m_resizeTimer, &QTimer::timeout, this, &ViewportWidget::onResizeTimeout);

    m_statusText = tr("Source Engine Preview (HLMV Backend)");
}

HWND ViewportWidget::hostHwnd() const {
    return reinterpret_cast<HWND>(winId());
}

void ViewportWidget::setEngineView(bridge::EngineView* engine) {
    m_engine = engine;
    update();
}

void ViewportWidget::setStatusText(const QString& status) {
    m_statusText = status;
    update();
}

void ViewportWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    m_pendingWidth = event->size().width();
    m_pendingHeight = event->size().height();
    m_resizeTimer->start();
}

void ViewportWidget::onResizeTimeout() {
    if (m_engine && m_engine->initialized() && m_pendingWidth > 0 && m_pendingHeight > 0) {
        (void)m_engine->resize(m_pendingWidth, m_pendingHeight);
        emit viewportResized(m_pendingWidth, m_pendingHeight);
    }
}

void ViewportWidget::paintEvent(QPaintEvent* event) {
    if (m_engine && m_engine->initialized()) {
        QWidget::paintEvent(event);
        return;
    }

    QPainter painter(this);
    painter.fillRect(rect(), QColor(26, 26, 28));

    // Subtle background grid
    painter.setPen(QPen(QColor(38, 38, 42), 1, Qt::DotLine));
    const int step = 40;
    for (int x = 0; x < width(); x += step) {
        painter.drawLine(x, 0, x, height());
    }
    for (int y = 0; y < height(); y += step) {
        painter.drawLine(0, y, width(), y);
    }

    // Centered status badge
    painter.setPen(QColor(160, 160, 165));
    QFont font = painter.font();
    font.setPointSize(11);
    font.setBold(true);
    painter.setFont(font);

    QRect textRect = rect();
    painter.drawText(textRect, Qt::AlignCenter, m_statusText);

    font.setPointSize(8);
    font.setBold(false);
    painter.setFont(font);
    painter.setPen(QColor(110, 110, 115));
    QRect subRect = textRect.adjusted(0, 40, 0, 0);
    painter.drawText(subRect, Qt::AlignCenter, tr("Drop a .vmt file here or use File > Open"));
}

void ViewportWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        for (const QUrl& url : event->mimeData()->urls()) {
            if (url.toLocalFile().endsWith(".vmt", Qt::CaseInsensitive)) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    QWidget::dragEnterEvent(event);
}

void ViewportWidget::dropEvent(QDropEvent* event) {
    if (event->mimeData()->hasUrls()) {
        for (const QUrl& url : event->mimeData()->urls()) {
            QString path = url.toLocalFile();
            if (path.endsWith(".vmt", Qt::CaseInsensitive)) {
                emit fileDropped(path);
                event->acceptProposedAction();
                return;
            }
        }
    }
    QWidget::dropEvent(event);
}

} // namespace qt
} // namespace ui
