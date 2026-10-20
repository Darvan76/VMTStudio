#include "asset_preview_panel.h"

#include "ui/asset_index.h"
#include "ui/material_info.h"
#include "vmt/document.h"
#include "vmt/schema.h"
#include "vtf/vtf_reader.h"

#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QObject>
#include <QPushButton>
#include <QScrollArea>
#include <QStringList>
#include <QVBoxLayout>

#include <cctype>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <utility>

#ifndef VMT_SCHEMA_DIR
// Only the vmtstudio target defines the schema location; an empty path makes
// every shader unknown instead of breaking the build.
#define VMT_SCHEMA_DIR ""
#endif

namespace ui {
namespace qt {

namespace {

// Preview decodes are capped at 512 px: enough to judge a texture on screen,
// a quarter of the memory of a full 1024 decode, and the same order of
// magnitude as the grid thumbnails.
constexpr std::uint32_t kPreviewMaxEdge = 512;

// The QLabel box the picture is scaled into. Fixed so switching assets never
// reflows the panel.
constexpr int kImageBox = 320;

// Strings that are composed in the worker thread. Q_DECLARE_TR_FUNCTIONS gives
// them the same translation context as the panel without touching `this`.
class WorkerText {
    Q_DECLARE_TR_FUNCTIONS(AssetPreviewPanel)
};

QString source_label(int sources) {
    QStringList parts;
    if ((sources & ui::SourceLoose) != 0) {
        parts.append(WorkerText::tr("Loose"));
    }
    if ((sources & ui::SourcePacked) != 0) {
        parts.append(WorkerText::tr("VPK"));
    }
    if ((sources & ui::SourceFolder) != 0) {
        parts.append(WorkerText::tr("Carpeta"));
    }
    return parts.isEmpty() ? WorkerText::tr("Desconocida")
                           : parts.join(QStringLiteral(" + "));
}

QString format_texture_info(const vtf::VtfInfo& info) {
    QStringList lines;

    const QString dimensions =
        QStringLiteral("%1 x %2").arg(info.width).arg(info.height);
    lines.append(WorkerText::tr("Dimensiones: %1").arg(dimensions));

    QString format = QString::fromStdString(info.format_name);
    if (format.isEmpty()) {
        format = WorkerText::tr("desconocido");
    }
    lines.append(WorkerText::tr("Formato: %1 (id %2)").arg(format).arg(info.format_id));
    lines.append(WorkerText::tr("Versión VTF: %1.%2")
                     .arg(info.version_major)
                     .arg(info.version_minor));
    lines.append(WorkerText::tr("Mips: %1").arg(info.mip_count));
    if (info.frame_count > 1) {
        lines.append(WorkerText::tr("Frames: %1").arg(info.frame_count));
    }
    if (info.depth > 1) {
        lines.append(WorkerText::tr("Profundidad: %1").arg(info.depth));
    }
    if (info.is_cubemap) {
        lines.append(WorkerText::tr("Cubemap: sí (%1 caras)").arg(info.face_count));
    }

    QStringList features;
    features.append(info.has_alpha ? WorkerText::tr("alfa") : WorkerText::tr("sin alfa"));
    if (info.is_normal_map) {
        features.append(WorkerText::tr("mapa normal"));
    }
    if (info.is_hdr) {
        features.append(WorkerText::tr("HDR"));
    }
    lines.append(WorkerText::tr("Características: %1").arg(features.join(QStringLiteral(", "))));

    return lines.join(QLatin1Char('\n'));
}

// Loaded once per process. A broken or missing data/schema does not fail the
// preview: every shader is simply reported as having no schema.
const vmt::SchemaLibrary& schema_library() {
    static const vmt::SchemaLibrary library = [] {
        const vmt::Result<vmt::SchemaLibrary> loaded =
            vmt::SchemaLibrary::load_dir(std::filesystem::path(VMT_SCHEMA_DIR));
        if (loaded) {
            return loaded.value();
        }
        return vmt::SchemaLibrary{};
    }();
    return library;
}

std::string to_lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// The value a $basetexture would carry: no "materials/" prefix, no ".vtf".
QString texture_name_for_copy(const QString& virtual_path) {
    QString name = virtual_path;
    if (name.startsWith(QStringLiteral("materials/"), Qt::CaseInsensitive)) {
        name.remove(0, 10);
    }
    if (name.endsWith(QStringLiteral(".vtf"), Qt::CaseInsensitive)) {
        name.chop(4);
    }
    return name;
}

} // namespace

// --- worker result ------------------------------------------------------------

struct PreviewResult {
    QString virtualPath;
    int generation = 0;
    QImage image;   // null when the kind has no picture
    QString text;   // metadata/errors; always filled
};

// Lives on the UI thread. A job keeps it alive with a shared_ptr and posts a
// queued call to it, so a panel that goes away only leaves a disconnected (and
// then cleared) handler behind.
class PreviewDelivery : public QObject {
public:
    std::function<void(const PreviewResult&)> handler;
};

namespace {

PreviewResult build_preview(const QString& virtual_path, int kind, const QString& disk_path,
                            int sources, quint64 size,
                            const std::shared_ptr<EngineAccess>& access,
                            const std::vector<std::filesystem::path>& roots,
                            int generation) {
    PreviewResult result;
    result.virtualPath = virtual_path;
    result.generation = generation;

    QStringList lines;
    lines.append(WorkerText::tr("Ruta: %1").arg(virtual_path));
    if (!disk_path.isEmpty()) {
        lines.append(WorkerText::tr("Disco: %1").arg(disk_path));
    }
    lines.append(WorkerText::tr("Fuente: %1").arg(source_label(sources)));
    if (size != 0) {
        lines.append(WorkerText::tr("Tamaño: %1")
                         .arg(QString::fromStdString(ui::format_bytes(size))));
    }

    // Disk first, then the engine. Blocking because the user is looking at
    // this asset; waiting for a scan in flight is preferable to an empty panel.
    GameTextureSource source(access, roots, GameTextureSource::LockMode::Blocking);

    if (kind == static_cast<int>(ui::AssetKind::Texture)) {
        const vmt::Result<std::vector<unsigned char>> bytes =
            source.read(virtual_path.toStdString());
        if (!bytes) {
            lines.append(QString());
            lines.append(WorkerText::tr("No se pudo leer la textura: %1")
                             .arg(QString::fromStdString(bytes.error().message)));
        } else {
            const std::vector<unsigned char>& data = bytes.value();
            const std::filesystem::path origin(virtual_path.toStdWString());
            const vmt::Result<vtf::VtfInfo> meta =
                vtf::open_vtf_info_from_memory(data.data(), data.size(), origin);
            if (!meta) {
                lines.append(QString());
                lines.append(WorkerText::tr("No es un VTF válido: %1")
                                 .arg(QString::fromStdString(meta.error().message)));
            } else {
                lines.append(QString());
                lines.append(format_texture_info(meta.value()));

                if (!meta.value().preview_supported) {
                    lines.append(QString());
                    lines.append(WorkerText::tr(
                        "Este formato no tiene preview de 8 bits (HDR, profundidad o volumen)."));
                } else {
                    const vmt::Result<vtf::RgbaImage> decoded =
                        vtf::decode_vtf_preview_from_memory(data.data(), data.size(), 0,
                                                            kPreviewMaxEdge, origin);
                    if (!decoded) {
                        lines.append(QString());
                        lines.append(WorkerText::tr("No se pudo decodificar la imagen: %1")
                                         .arg(QString::fromStdString(decoded.error().message)));
                    } else {
                        const vtf::RgbaImage& image = decoded.value();
                        // copy(): the QImage must own its pixels before the
                        // RgbaImage goes out of scope.
                        result.image = QImage(image.pixels.data(),
                                              static_cast<int>(image.width),
                                              static_cast<int>(image.height),
                                              QImage::Format_RGBA8888)
                                           .copy();
                    }
                }
            }
        }
    } else if (kind == static_cast<int>(ui::AssetKind::Material)) {
        const vmt::Result<std::vector<unsigned char>> bytes =
            source.read(virtual_path.toStdString());
        if (!bytes) {
            lines.append(QString());
            lines.append(WorkerText::tr("No se pudo leer el material: %1")
                             .arg(QString::fromStdString(bytes.error().message)));
        } else {
            const std::vector<unsigned char>& data = bytes.value();
            const std::string text(data.begin(), data.end());

            // The schema decides which parameters are textures beyond the
            // $basetexture/$textureN heuristic. Unknown shaders fall back to
            // the heuristic and are labelled "(no schema)" by the formatter.
            std::string shader;
            const vmt::Result<vmt::Document> document =
                vmt::Document::parse(text, virtual_path.toStdString());
            if (document) {
                shader = std::string(document.value().shader());
            }

            bool shader_known = false;
            std::set<std::string> schema_textures;
            if (!shader.empty()) {
                const vmt::SchemaLibrary& library = schema_library();
                shader_known = library.has_shader(shader);
                if (shader_known) {
                    const vmt::ShaderSchema schema = library.schema_for(shader);
                    for (const vmt::SchemaGroup& group : schema.groups()) {
                        for (const vmt::SchemaParam& param : group.parameters) {
                            if (param.type == vmt::ParamType::Texture) {
                                schema_textures.insert(to_lower(param.name));
                            }
                        }
                    }
                }
            }

            const ui::MaterialInfo info =
                ui::collect_material_info(text, source, shader_known, schema_textures);
            lines.append(QString());
            lines.append(QString::fromStdString(ui::format_material_info(info)).trimmed());
        }
    } else if (kind == static_cast<int>(ui::AssetKind::Model)) {
        lines.append(QString());
        lines.append(WorkerText::tr("Pulsa «Cargar modelo» para abrirlo en el visor 3D."));
    }

    result.text = lines.join(QLatin1Char('\n'));
    return result;
}

} // namespace

// --- AssetPreviewPanel --------------------------------------------------------

AssetPreviewPanel::AssetPreviewPanel(std::shared_ptr<EngineAccess> access, QWidget* parent)
    : QWidget(parent),
      m_access(std::move(access)),
      m_delivery(std::make_shared<PreviewDelivery>()) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 0, 0, 0);
    layout->setSpacing(6);

    m_title = new QLabel(tr("Preview"), this);
    QFont title_font = m_title->font();
    title_font.setBold(true);
    m_title->setFont(title_font);
    m_title->setWordWrap(true);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_title);

    auto* buttons = new QHBoxLayout();
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(6);
    m_action = new QPushButton(tr("Abrir en editor"), this);
    m_action->setVisible(false);
    buttons->addWidget(m_action);
    m_copyName = new QPushButton(tr("Copiar nombre de textura"), this);
    m_copyName->setToolTip(tr("Copia el valor listo para un $basetexture (sin materials/ ni .vtf)"));
    m_copyName->setVisible(false);
    buttons->addWidget(m_copyName);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content = new QWidget(m_scroll);
    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(8);

    m_image = new QLabel(content);
    m_image->setAlignment(Qt::AlignCenter);
    m_image->setMinimumHeight(180);
    m_image->setMaximumHeight(340);
    m_image->setTextFormat(Qt::PlainText);
    m_image->setWordWrap(true);
    m_image->setStyleSheet(QStringLiteral(
        "background-color: #1a1a1c; border: 1px solid #3e3e42; border-radius: 4px;"
        "color: #7a7a7e;"));
    m_image->setText(tr("Sin selección"));
    content_layout->addWidget(m_image);

    m_info = new QLabel(content);
    m_info->setTextFormat(Qt::PlainText);
    m_info->setWordWrap(true);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_info->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_info->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
    QFont info_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    // A touch smaller than the UI font: these are long, technical lines.
    info_font.setPointSizeF(qMax(7.0, info_font.pointSizeF() - 0.5));
    m_info->setFont(info_font);
    m_info->setStyleSheet(QStringLiteral("color: #c8c8c8;"));
    content_layout->addWidget(m_info, 1);
    content_layout->addStretch(1);

    m_scroll->setWidget(content);
    layout->addWidget(m_scroll, 1);

    connect(m_action, &QPushButton::clicked, this, [this]() {
        if (!m_path.isEmpty()) {
            emit activateRequested(m_path, m_kind);
        }
    });
    connect(m_copyName, &QPushButton::clicked, this, [this]() {
        if (!m_path.isEmpty()) {
            QApplication::clipboard()->setText(texture_name_for_copy(m_path));
        }
    });

    m_delivery->handler = [this](const PreviewResult& result) { applyResult(result); };
}

AssetPreviewPanel::~AssetPreviewPanel() {
    // A decode still running finishes into nothing: the queued delivery sees
    // no handler. The generation bump also covers a handler already captured
    // by an event that has not been dispatched yet.
    ++m_generation;
    if (m_delivery) {
        m_delivery->handler = {};
    }
}

void AssetPreviewPanel::setSearchRoots(std::vector<std::filesystem::path> roots) {
    m_roots = std::move(roots);
}

void AssetPreviewPanel::showAsset(const QString& virtualPath, int kind, const QString& diskPath,
                                  int sources, quint64 size) {
    if (virtualPath.isEmpty()) {
        clear();
        return;
    }
    if (virtualPath == m_path && kind == m_kind && (m_loading || m_hasResult)) {
        return; // switching list/grid must not repeat the decode
    }

    m_path = virtualPath;
    m_kind = kind;
    m_loading = true;
    m_hasResult = false;

    m_title->setText(virtualPath);
    m_title->setToolTip(virtualPath);
    m_image->setPixmap(QPixmap());
    m_image->setText(tr("Cargando…"));
    m_info->setText(tr("Cargando…"));
    updateActions();

    const int generation = ++m_generation;
    const std::shared_ptr<PreviewDelivery> delivery = m_delivery;
    const std::shared_ptr<EngineAccess> access = m_access;
    std::vector<std::filesystem::path> roots = m_roots;

    // A thread per selection: at most one preview is
    // in flight (the newer request bumps the generation), and a thread waiting
    // on the engine gate must not keep the process alive at shutdown.
    std::thread([delivery, generation, access, roots = std::move(roots), virtualPath, kind,
                 diskPath, sources, size]() {
        PreviewResult result = build_preview(virtualPath, kind, diskPath, sources, size,
                                             access, roots, generation);
        QMetaObject::invokeMethod(
            delivery.get(),
            [delivery, result]() {
                if (delivery->handler) {
                    delivery->handler(result);
                }
            },
            Qt::QueuedConnection);
    }).detach();
}

void AssetPreviewPanel::clear() {
    ++m_generation;
    m_loading = false;
    m_hasResult = false;
    m_path.clear();
    m_kind = static_cast<int>(ui::AssetKind::Other);
    m_title->setText(tr("Preview"));
    m_title->setToolTip(QString());
    m_image->setPixmap(QPixmap());
    m_image->setText(tr("Sin selección"));
    m_info->clear();
    updateActions();
}

void AssetPreviewPanel::setPickMode(bool enabled) {
    if (m_pickMode == enabled) {
        return;
    }
    m_pickMode = enabled;
    updateActions();
}

void AssetPreviewPanel::applyResult(const PreviewResult& result) {
    if (result.generation != m_generation || result.virtualPath != m_path) {
        return; // a newer selection (or a cleared panel) already won
    }

    m_loading = false;
    m_hasResult = true;

    if (result.image.isNull()) {
        m_image->setPixmap(QPixmap());
        m_image->setText(m_kind == static_cast<int>(ui::AssetKind::Model)
                             ? tr("Sin preview de modelo")
                             : tr("Sin preview"));
    } else {
        m_image->setText(QString());
        m_image->setPixmap(QPixmap::fromImage(
            result.image.scaled(kImageBox, kImageBox, Qt::KeepAspectRatio,
                                Qt::SmoothTransformation)));
    }
    m_info->setText(result.text);
    updateActions();
}

void AssetPreviewPanel::updateActions() {
    const bool has_asset = !m_path.isEmpty() && !m_pickMode;
    const bool is_material = m_kind == static_cast<int>(ui::AssetKind::Material);
    const bool is_model = m_kind == static_cast<int>(ui::AssetKind::Model);
    const bool is_texture = m_kind == static_cast<int>(ui::AssetKind::Texture);

    m_action->setVisible(has_asset && (is_material || is_model));
    m_action->setText(is_model ? tr("Cargar modelo") : tr("Abrir en editor"));
    m_copyName->setVisible(has_asset && is_texture);
}

} // namespace qt
} // namespace ui
