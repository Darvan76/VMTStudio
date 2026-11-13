#include "schema_resources.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QStringList>

#include <cstddef>
#include <string_view>
#include <utility>

namespace ui {
namespace qt {
namespace {

vmt::Result<vmt::SchemaLibrary> read_schema_resources() {
    const QDir directory(QStringLiteral(":/schemas"));
    const QStringList names = directory.entryList(
        QStringList{QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    if (names.isEmpty()) {
        return vmt::Err<vmt::SchemaLibrary>(vmt::make_error(
            vmt::ErrorCode::SchemaNotFound, "no bundled shader schemas were found",
            "Reinstall the complete VMT Studio application.", ":/schemas"));
    }

    vmt::SchemaLibrary library;
    for (const QString& name : names) {
        const QString resource_path = directory.filePath(name);
        QFile file(resource_path);
        if (!file.open(QIODevice::ReadOnly)) {
            return vmt::Err<vmt::SchemaLibrary>(vmt::make_error(
                vmt::ErrorCode::SchemaLoadError, "cannot open bundled shader schema",
                "Reinstall the complete VMT Studio application.", resource_path.toStdString()));
        }
        const QByteArray bytes = file.readAll();
        if (file.error() != QFile::NoError) {
            return vmt::Err<vmt::SchemaLibrary>(vmt::make_error(
                vmt::ErrorCode::SchemaLoadError, "cannot read bundled shader schema",
                "Reinstall the complete VMT Studio application.", resource_path.toStdString()));
        }
        const vmt::Result<void> added = library.add_json(
            std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())),
            resource_path.toStdString());
        if (!added) return vmt::Err<vmt::SchemaLibrary>(added.error());
    }
    return vmt::Ok(std::move(library));
}

} // namespace

const vmt::Result<vmt::SchemaLibrary>& bundled_schemas() {
    static const vmt::Result<vmt::SchemaLibrary> library = read_schema_resources();
    return library;
}

vmt::Result<vmt::SchemaLibrary> load_bundled_schemas() {
    return bundled_schemas();
}

} // namespace qt
} // namespace ui
