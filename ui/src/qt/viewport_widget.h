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

protected:
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

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
