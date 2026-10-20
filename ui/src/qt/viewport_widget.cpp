#include "viewport_widget.h"
#include "bridge/engine_view.h"

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QDataStream>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include <QDir>

namespace ui {
namespace qt {

namespace {

// Must match AssetListModel::mimeType() (the browser is the only producer).
constexpr const char* kAssetMimeType = "application/x-vmtstudio-asset";
constexpr quint32 kAssetPayloadVersion = 1;

bool has_asset_payload(const QMimeData* mime) {
    return mime != nullptr && mime->hasFormat(QString::fromLatin1(kAssetMimeType));
}

// Reads the QDataStream written by AssetListModel::mimeData(): version, kind,
// virtual path, disk path. A malformed payload is ignored so the standard
// text/uri-list fallback can still open a loose file.
bool decode_asset_payload(const QMimeData* mime, QString& virtualPath, int& kind,
                          QString& diskPath) {
    if (!has_asset_payload(mime)) {
        return false;
    }
    const QByteArray payload = mime->data(QString::fromLatin1(kAssetMimeType));
    if (payload.isEmpty()) {
        return false;
    }

    QDataStream stream(payload);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 version = 0;
    qint32 rawKind = -1;
    QString rawVirtualPath;
    QString rawDiskPath;
    stream >> version >> rawKind >> rawVirtualPath >> rawDiskPath;
    if (stream.status() != QDataStream::Ok || version != kAssetPayloadVersion ||
        rawVirtualPath.isEmpty()) {
        return false;
    }

    // Validate against path traversal and null bytes
    if (rawVirtualPath.contains(QChar('\0'))) {
        return false;
    }
    const QString cleanVirtual = QDir::cleanPath(rawVirtualPath);
    if (cleanVirtual.startsWith("..") || cleanVirtual.contains("/../") || cleanVirtual.contains("\\..\\")) {
        return false;
    }

    if (!rawDiskPath.isEmpty()) {
        if (rawDiskPath.contains(QChar('\0'))) {
            return false;
        }
        const QString cleanDisk = QDir::cleanPath(rawDiskPath);
        if (!QFileInfo(cleanDisk).isAbsolute() &&
            (cleanDisk.startsWith("..") || cleanDisk.contains("/../") || cleanDisk.contains("\\..\\"))) {
            return false;
        }
        rawDiskPath = cleanDisk;
    }

    virtualPath = cleanVirtual;
    kind = rawKind;
    diskPath = rawDiskPath;
    return true;
}

} // namespace

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
    if (m_engine && m_engine->initialized()) {
        (void)m_engine->resize(0, 0);
    }
    update();
}

void ViewportWidget::setStatusText(const QString& status) {
    m_statusText = status;
    update();
}

void ViewportWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_engine && m_engine->initialized()) {
        HWND hwnd = hostHwnd();
        RECT rc{};
        if (hwnd && GetClientRect(hwnd, &rc)) {
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            if (w > 0 && h > 0) {
                (void)m_engine->resize(w, h);
            }
        } else {
            const qreal dpr = devicePixelRatioF();
            int w = static_cast<int>(event->size().width() * dpr);
            int h = static_cast<int>(event->size().height() * dpr);
            (void)m_engine->resize(w, h);
        }
    }
    m_resizeTimer->start();
}

void ViewportWidget::onResizeTimeout() {
    if (m_engine && m_engine->initialized()) {
        HWND hwnd = hostHwnd();
        RECT rc{};
        int w = 0, h = 0;
        if (hwnd && GetClientRect(hwnd, &rc)) {
            w = rc.right - rc.left;
            h = rc.bottom - rc.top;
        }
        if (w <= 0 || h <= 0) {
            const qreal dpr = devicePixelRatioF();
            w = static_cast<int>(width() * dpr);
            h = static_cast<int>(height() * dpr);
        }
        if (w > 0 && h > 0) {
            (void)m_engine->resize(w, h);
            emit viewportResized(w, h);
        }
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
    painter.drawText(subRect, Qt::AlignCenter,
                     tr("Drop a .vmt or .mdl file here, or drag an asset from the browser (F3)"));
}

void ViewportWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (has_asset_payload(event->mimeData())) {
        event->acceptProposedAction();
        return;
    }
    if (event->mimeData()->hasUrls()) {
        for (const QUrl& url : event->mimeData()->urls()) {
            const QString path = url.toLocalFile();
            if (path.isEmpty() || path.contains(QChar('\0'))) {
                continue;
            }
            const QString clean = QDir::cleanPath(path);
            if (clean.endsWith(".vmt", Qt::CaseInsensitive) ||
                clean.endsWith(".mdl", Qt::CaseInsensitive)) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    QWidget::dragEnterEvent(event);
}

void ViewportWidget::dropEvent(QDropEvent* event) {
    // The custom format wins over text/uri-list: a loose asset carries both,
    // and the custom payload also knows the game-relative path and its kind.
    QString virtualPath;
    QString diskPath;
    int kind = -1;
    if (decode_asset_payload(event->mimeData(), virtualPath, kind, diskPath)) {
        emit assetDropped(virtualPath, kind, diskPath);
        event->acceptProposedAction();
        return;
    }

    if (event->mimeData()->hasUrls()) {
        for (const QUrl& url : event->mimeData()->urls()) {
            const QString path = url.toLocalFile();
            if (path.isEmpty() || path.contains(QChar('\0'))) {
                continue;
            }
            const QString clean = QDir::cleanPath(path);
            if (clean.endsWith(".vmt", Qt::CaseInsensitive)) {
                emit fileDropped(clean);
                event->acceptProposedAction();
                return;
            }
            if (clean.endsWith(".mdl", Qt::CaseInsensitive)) {
                emit modelDropped(clean);
                event->acceptProposedAction();
                return;
            }
        }
    }
    QWidget::dropEvent(event);
}

bool ViewportWidget::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    // The native renderer posts WM_USER+422 while a model is playing (see
    // docs/IPC_PROTOCOL.md). The tick is re-emitted as a signal and the message
    // is never swallowed: return false so the default handling still sees it.
    if (eventType == "windows_generic_MSG" && message != nullptr) {
        const MSG* msg = static_cast<const MSG*>(message);
        if (msg->message == WM_USER + 422u) {
            emit playheadTick(static_cast<int>(msg->wParam), static_cast<qintptr>(msg->lParam));
            return false;
        }
    }
    return QWidget::nativeEvent(eventType, message, result);
}

} // namespace qt
} // namespace ui
