#include "about_dialog.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QIcon>
#include <QPixmap>

namespace ui {
namespace qt {

AboutDialog::AboutDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("About VMT-Studio"));
    setWindowIcon(QIcon(":/icons/app.png"));
    setFixedSize(440, 390);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(10);

    auto* logoLabel = new QLabel(this);
    QPixmap logoPixmap(":/icons/app.png");
    if (!logoPixmap.isNull()) {
        logoLabel->setPixmap(logoPixmap.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        logoLabel->setAlignment(Qt::AlignCenter);
        layout->addWidget(logoLabel);
    }

    auto* titleLabel = new QLabel(tr("<h2>VMT-Studio</h2>"), this);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setStyleSheet("color: #e08214;"); // Source orange
    layout->addWidget(titleLabel);

    auto* versionLabel = new QLabel(tr("<b>Version 0.1.0 (Qt 6 Edition)</b>"), this);
    versionLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(versionLabel);

    auto* descLabel = new QLabel(
        tr("<p>Modern material studio, parameter inspector and real-time 3D previewer "
           "for Valve's Source Engine materials (.vmt / .vtf).</p>"
           "<p><b>Architecture:</b><br>"
           "• Frontend: 64-bit Qt 6 MSVC<br>"
           "• Rendering Backend: 32-bit Out-of-Process HLMV Engine<br>"
           "• Texture Decoder: VTFLib++<br>"
           "• Material Core: vmt_core lossless parser</p>"
           "<p style='color: #888888; font-size: 8pt;'>"
           "Valve, Half-Life and Source Engine are trademarks or registered "
           "trademarks of Valve Corporation.</p>"), this);
    descLabel->setWordWrap(true);
    layout->addWidget(descLabel);

    layout->addStretch();

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttonBox);
}

} // namespace qt
} // namespace ui
