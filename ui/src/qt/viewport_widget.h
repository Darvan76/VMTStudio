#pragma once

#include <QWidget>
#include <QTimer>
#include <QString>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace bridge {
class EngineView;
}

namespace ui {
namespace qt {

class ViewportWidget : public QWidget {
    Q_OBJECT

public:
    explicit ViewportWidget(QWidget* parent = nullptr);
    ~ViewportWidget() override = default;

    HWND hostHwnd() const;
    void setEngineView(bridge::EngineView* engine);

    void setStatusText(const QString& status);

signals:
    void viewportResized(int width, int height);
    void fileDropped(const QString& filePath);
    // A .mdl file was dropped on the viewport; the path is a local disk path.
    void modelDropped(const QString& filePath);
    // An asset dragged from the Asset Browser ("application/x-vmtstudio-asset"):
    // the game-relative path, its ui::AssetKind and, for loose rows, the disk
    // path (empty for VPK-only assets).
    void assetDropped(const QString& virtualPath, int kind, const QString& diskPath);
    // Live playback tick posted by the backend (WM_USER+422): the current frame
    // and cycle * 10000. Best-effort; the receiver may ignore it entirely.
    void playheadTick(int frame, qintptr cycle_x10000);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private slots:
    void onResizeTimeout();

private:
    bridge::EngineView* m_engine = nullptr;
    QTimer* m_resizeTimer = nullptr;
    int m_pendingWidth = 0;
    int m_pendingHeight = 0;
    QString m_statusText;
};

} // namespace qt
} // namespace ui
