#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include "ut.h"

#include "asset_preview_panel.h"
#include "asset_thumbnail_service.h"
#include "asset_list_model.h"
#include "app_updater.h"
#include "export_materials_dialog.h"
#include "connection_dialog.h"
#include "ui/asset_index.h"

#include <QApplication>
#include <QByteArray>
#include <QLabel>
#include <QThreadPool>
#include <QTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QPersistentModelIndex>
#include <QEvent>
#include <QThread>
#include <QProcess>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Reads {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> paths;
    std::vector<std::uint32_t> prefixes;
    std::vector<unsigned char> header;
    std::string heldPath;
    bool released = false;
    int active = 0;
    int maximum = 0;

    void hold(const std::string& path) {
        const std::lock_guard<std::mutex> lock(mutex);
        heldPath = path;
        released = false;
    }

    void release() {
        const std::lock_guard<std::mutex> lock(mutex);
        released = true;
        changed.notify_all();
    }

    void enter(const std::string& path, std::unique_lock<std::mutex>& lock,
               const std::function<bool()>& cancelled = {}) {
        paths.push_back(path);
        maximum = std::max(maximum, ++active);
        changed.notify_all();
        while (path == heldPath && !released && !(cancelled && cancelled())) {
            changed.wait_for(lock, std::chrono::milliseconds(5));
        }
    }

    std::vector<std::string> recorded() {
        const std::lock_guard<std::mutex> lock(mutex);
        return paths;
    }
};

// Only reads have behavior. The unused renderer operations deliberately do
// nothing so these checks exercise the real source, worker and delivery code.
class TestEngine final : public bridge::EngineView {
public:
    explicit TestEngine(std::shared_ptr<Reads> reads) : m_reads(std::move(reads)) {}
    vmt::Result<void> initialize(const bridge::EngineViewConfig&) override { return vmt::Ok(); }
    vmt::Result<void> mount_directory(const std::filesystem::path&, bool) override { return vmt::Ok(); }
    vmt::Result<void> set_material(const std::string&) override { return vmt::Ok(); }
    vmt::Result<void> reload_material() override { return vmt::Ok(); }
    vmt::Result<bool> file_exists(const std::string&) override { return vmt::Ok(true); }
    vmt::Result<bridge::AssetList> list_assets(const std::vector<std::string>&,
                                              const std::vector<std::string>&) override {
        return vmt::Ok(bridge::AssetList{});
    }
    vmt::Result<std::string> read_text_file(
        const std::string& path, const std::atomic<bool>* cancelled = nullptr) override {
        const auto generation = query_generation();
        const auto stopped = [this, generation, cancelled]() {
            return query_cancelled(generation) || (cancelled && cancelled->load());
        };
        std::unique_lock<std::mutex> lock(m_reads->mutex);
        m_reads->enter(path, lock, stopped);
        --m_reads->active;
        if (stopped()) return vmt::Err<std::string>(vmt::make_error(vmt::ErrorCode::Internal, "cancelled"));
        return vmt::Ok(std::string("UnlitGeneric {}\r\n"));
    }
    vmt::Result<std::vector<unsigned char>> read_binary_file(const std::string& path) override {
        std::unique_lock<std::mutex> lock(m_reads->mutex);
        m_reads->enter(path, lock);
        const std::string material = "UnlitGeneric {}\n";
        std::vector<unsigned char> result(material.begin(), material.end());
        --m_reads->active;
        return vmt::Ok(std::move(result));
    }
    vmt::Result<bridge::BinaryPrefix> read_binary_prefix(const std::string& path,
                                                        std::uint32_t maximum) override {
        std::unique_lock<std::mutex> lock(m_reads->mutex);
        m_reads->enter(path, lock);
        m_reads->prefixes.push_back(maximum);
        bridge::BinaryPrefix result;
        result.file_size = 4u * 1024u * 1024u;
        const std::size_t count = std::min<std::size_t>(maximum, m_reads->header.size());
        result.bytes.assign(m_reads->header.begin(), m_reads->header.begin() + count);
        --m_reads->active;
        return vmt::Ok(std::move(result));
    }
    vmt::Result<void> set_preview_shape(bridge::PreviewShape) override { return vmt::Ok(); }
    vmt::Result<void> set_lighting(const bridge::PreviewLighting&) override { return vmt::Ok(); }
    vmt::Result<void> set_model(const std::filesystem::path&) override { return vmt::Ok(); }
    vmt::Result<void> set_render_mode(const std::string&) override { return vmt::Ok(); }
    vmt::Result<void> set_animation(const std::string&) override { return vmt::Ok(); }
    vmt::Result<void> set_speed(float) override { return vmt::Ok(); }
    vmt::Result<bridge::ModelScene> query_model_scene() override { return vmt::Ok(bridge::ModelScene{}); }
    vmt::Result<std::vector<unsigned char>> read_game_binary(const std::string& path) override {
        return read_binary_file(path);
    }
    vmt::Result<void> set_sequence(int) override { return vmt::Ok(); }
    vmt::Result<void> set_sequence_label(const std::string&) override { return vmt::Ok(); }
    vmt::Result<void> set_frame(int) override { return vmt::Ok(); }
    vmt::Result<void> step_frame(int) override { return vmt::Ok(); }
    vmt::Result<void> set_loop(bool) override { return vmt::Ok(); }
    vmt::Result<void> stop_animation() override { return vmt::Ok(); }
    vmt::Result<void> set_blend_time(float) override { return vmt::Ok(); }
    vmt::Result<void> blend_now() override { return vmt::Ok(); }
    vmt::Result<void> set_overlay(int, int, float) override { return vmt::Ok(); }
    vmt::Result<void> set_pose_parameter(int, float) override { return vmt::Ok(); }
    vmt::Result<void> set_bodygroup(int, int) override { return vmt::Ok(); }
    vmt::Result<void> set_skin(int) override { return vmt::Ok(); }
    vmt::Result<void> set_view_flag(const std::string&, bool) override { return vmt::Ok(); }
    vmt::Result<void> reload_material_by_name(const std::string&) override { return vmt::Ok(); }
    vmt::Result<void> render() override { return vmt::Ok(); }
    vmt::Result<void> resize(int, int) override { return vmt::Ok(); }
    void shutdown() override {}
    bool initialized() const noexcept override { return true; }
private:
    std::shared_ptr<Reads> m_reads;
};

struct Engines {
    std::shared_ptr<Reads> reads = std::make_shared<Reads>();
    TestEngine preview{reads};
    TestEngine thumbnails{reads};
    std::shared_ptr<ui::qt::EngineAccess> previewAccess = std::make_shared<ui::qt::EngineAccess>();
    std::shared_ptr<ui::qt::EngineAccess> thumbnailAccess = std::make_shared<ui::qt::EngineAccess>();

    Engines() {
        previewAccess->setEngine(&preview);
        thumbnailAccess->setEngine(&thumbnails);
    }
    ~Engines() {
        reads->release();
        previewAccess->setEngine(nullptr);
        thumbnailAccess->setEngine(nullptr);
        QThreadPool::globalInstance()->waitForDone(5000);
        QApplication::processEvents();
    }
};

template <class Predicate>
bool wait_until(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready()) {
        QApplication::processEvents();
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    QApplication::processEvents();
    return true;
}

bool loaded(ui::qt::AssetPreviewPanel& panel, const QString& path) {
    const auto* info = panel.findChild<QLabel*>(QStringLiteral("assetPreviewInfo"));
    return info && info->text().contains(path);
}

void show_material(ui::qt::AssetPreviewPanel& panel, const QString& path) {
    panel.showAsset(path, static_cast<int>(ui::AssetKind::Material), {}, ui::SourcePacked, 0);
}

bool load_gate_is_held() {
    if (!ui::qt::asset_load_mutex().try_lock()) return true;
    ui::qt::asset_load_mutex().unlock();
    return false;
}

} // namespace

UT_TEST_CASE("Qt assets: preview loads only active and latest selections while work is bounded") {
    Engines engines;
    engines.reads->hold("materials/first.vmt");
    ui::qt::AssetPreviewPanel panel(engines.previewAccess);
    show_material(panel, QStringLiteral("materials/first.vmt"));
    const bool first_started = wait_until([&]() { return !engines.reads->recorded().empty(); });
    UT_CHECK(first_started);
    if (!first_started) return;
    UT_CHECK(load_gate_is_held());

    for (int index = 0; index < 100; ++index) {
        show_material(panel, QStringLiteral("materials/obsolete%1.vmt").arg(index));
    }
    const QString latest = QStringLiteral("materials/latest.vmt");
    show_material(panel, latest);
    engines.reads->release();
    UT_CHECK(wait_until([&]() { return loaded(panel, latest); }));
    UT_CHECK(QThreadPool::globalInstance()->waitForDone(5000));
    const auto paths = engines.reads->recorded();
    UT_CHECK(paths == std::vector<std::string>({"materials/first.vmt", "materials/latest.vmt"}));
    UT_CHECK(engines.reads->maximum == 1);
}

UT_TEST_CASE("Qt assets: cancelled work does not read and previews share the thumbnail load gate") {
    Engines engines;
    ui::qt::AssetThumbnailService thumbnails(engines.thumbnailAccess);
    {
        std::unique_lock<std::mutex> gate(ui::qt::asset_load_mutex());
        auto panel = std::make_unique<ui::qt::AssetPreviewPanel>(engines.previewAccess);
        show_material(*panel, QStringLiteral("materials/closed.vmt"));
        panel->clear();
        show_material(*panel, QStringLiteral("materials/closed-again.vmt"));
        panel.reset();
        thumbnails.request(QStringLiteral("materials/stale-thumb.vmt"), {});
        thumbnails.invalidate();
    }
    UT_CHECK(QThreadPool::globalInstance()->waitForDone(5000));
    QApplication::processEvents();
    UT_CHECK(engines.reads->recorded().empty());

    engines.reads->hold("materials/thumbnail.vmt");
    thumbnails.request(QStringLiteral("materials/thumbnail.vmt"), {});
    const bool thumbnail_started = wait_until([&]() { return !engines.reads->recorded().empty(); });
    UT_CHECK(thumbnail_started);
    if (!thumbnail_started) return;
    UT_CHECK(load_gate_is_held());

    ui::qt::AssetPreviewPanel panel(engines.previewAccess);
    const QString preview_path = QStringLiteral("materials/preview.vmt");
    show_material(panel, preview_path);
    engines.reads->release();
    UT_CHECK(wait_until([&]() { return loaded(panel, preview_path) && thumbnails.pendingCount() == 0; }));
    UT_CHECK(QThreadPool::globalInstance()->waitForDone(5000));
    const auto paths = engines.reads->recorded();
    UT_CHECK(paths.size() == 2);
    UT_CHECK(engines.reads->maximum == 1);
}

UT_TEST_CASE("Qt assets: unsupported texture preview reads metadata prefixes without loading pixel payload") {
    Engines engines;
    const std::filesystem::path fixture = std::filesystem::path(VTF_TEST_DATA_DIR) / "rgba8888.vtf";
    std::ifstream input(fixture, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),
                                      std::istreambuf_iterator<char>());
    UT_CHECK(bytes.size() >= 80);
    if (bytes.size() < 80) return;
    bytes.resize(80);
    // A 7.2 volume header is valid metadata but not a 2D preview. No image
    // payload is supplied by this fake engine, so an accidental full read fails.
    const auto write32 = [&bytes](std::size_t offset, std::uint32_t value) {
        for (unsigned index = 0; index < 4; ++index)
            bytes[offset + index] = static_cast<unsigned char>(value >> (index * 8));
    };
    write32(4, 7);
    write32(8, 2);
    write32(12, 80);
    bytes[63] = 2;
    bytes[64] = 0;
    engines.reads->header = std::move(bytes);
    ui::qt::AssetPreviewPanel panel(engines.previewAccess);
    const QString path = QStringLiteral("materials/volume.vtf");
    panel.showAsset(path, static_cast<int>(ui::AssetKind::Texture), {}, ui::SourcePacked, 0);
    UT_CHECK(wait_until([&]() { return loaded(panel, path); }));
    UT_CHECK(QThreadPool::globalInstance()->waitForDone(5000));
    UT_CHECK(engines.reads->prefixes == std::vector<std::uint32_t>({16, 80}));
    UT_CHECK(engines.reads->recorded().size() == 2);
    const auto* info = panel.findChild<QLabel*>(QStringLiteral("assetPreviewInfo"));
    UT_CHECK(info && info->text().contains(QStringLiteral("Profundidad: 2")));
    UT_CHECK(info && info->text().contains(QStringLiteral("no tiene preview")));
}

UT_TEST_CASE("Qt export: blocked reads keep the UI responsive and files are written atomically") {
    Engines engines;
    QTemporaryDir destination;
    UT_CHECK(destination.isValid());
    engines.reads->hold("materials/first.vmt");
    ui::qt::ExportMaterialsDialog dialog(&engines.preview, {"first", "second"}, "test");
    auto* edit = dialog.findChild<QLineEdit*>("exportDestination");
    auto* start = dialog.findChild<QPushButton*>("exportStart");
    UT_CHECK(edit && start);
    if (!edit || !start) return;
    edit->setText(destination.path());
    start->click();
    const bool started = wait_until([&]() { return !engines.reads->recorded().empty(); });
    UT_CHECK(started);
    bool pulse = false;
    QTimer::singleShot(0, &dialog, [&]() { pulse = true; });
    UT_CHECK(wait_until([&]() { return pulse; }));
    UT_CHECK(!start->isEnabled());
    engines.reads->release();
    UT_CHECK(wait_until([&]() { return start->isEnabled(); }));
    QFile first(destination.filePath("first.vmt"));
    UT_CHECK(first.open(QIODevice::ReadOnly));
    UT_CHECK(first.readAll() == QByteArray("UnlitGeneric {}\r\n"));
    first.close();
    UT_CHECK(QFile::exists(destination.filePath("second.vmt")));
    const auto reads = engines.reads->recorded().size();
    auto* overwrite = dialog.findChild<QCheckBox*>();
    UT_CHECK(overwrite);
    if (overwrite) overwrite->setChecked(false);
    start->click();
    UT_CHECK(wait_until([&]() { return start->isEnabled(); }));
    UT_CHECK(engines.reads->recorded().size() == reads);
}

UT_TEST_CASE("Qt export: closing during a read cancels before writing or starting the next file") {
    Engines engines;
    QTemporaryDir destination;
    engines.reads->hold("materials/first.vmt");
    ui::qt::ExportMaterialsDialog dialog(&engines.preview, {"first", "second"}, "test");
    dialog.findChild<QLineEdit*>("exportDestination")->setText(destination.path());
    auto* start = dialog.findChild<QPushButton*>("exportStart");
    bool finished = false;
    QObject::connect(&dialog, &QDialog::finished, &dialog, [&](int) { finished = true; });
    start->click();
    UT_CHECK(wait_until([&]() { return !engines.reads->recorded().empty(); }));
    dialog.reject();
    UT_CHECK(!finished);
    engines.reads->release();
    UT_CHECK(wait_until([&]() { return finished; }));
    UT_CHECK(engines.reads->recorded() == std::vector<std::string>({"materials/first.vmt"}));
    UT_CHECK(!QFile::exists(destination.filePath("first.vmt")));
    UT_CHECK(!QFile::exists(destination.filePath("second.vmt")));
}

UT_TEST_CASE("Qt updater: preparation cancellation completes asynchronously and a failed retry stays in the app") {
    QTemporaryDir directory;
    ui::qt::AppUpdater updater;
    bool cancelled = false;
    bool failed = false;
    auto extraction = std::make_shared<Reads>();
    extraction->hold("extraction");
    QObject::connect(&updater, &ui::qt::AppUpdater::extractionStatus, &updater,
        [extraction](const QString&) {
            std::unique_lock<std::mutex> lock(extraction->mutex);
            extraction->enter("extraction", lock);
            --extraction->active;
        }, Qt::DirectConnection);
    QObject::connect(&updater, &ui::qt::AppUpdater::updateCancelled, &updater, [&]() { cancelled = true; });
    QObject::connect(&updater, &ui::qt::AppUpdater::updateFailed, &updater, [&](const QString& error) {
        failed = !error.isEmpty();
    });
    updater.applyUpdate(directory.filePath("missing.zip"));
    UT_CHECK(updater.isPreparing());
    UT_CHECK(wait_until([&]() { return !extraction->recorded().empty(); }));
    updater.cancelDownload();
    extraction->release();
    UT_CHECK(wait_until([&]() { return !updater.isPreparing(); }));
    UT_CHECK(cancelled);
    UT_CHECK(!failed);
    updater.applyUpdate(directory.filePath("missing.zip"));
    UT_CHECK(wait_until([&]() { return !updater.isPreparing(); }));
    UT_CHECK(failed);
}

UT_TEST_CASE("Qt engine access: clearing cancels active queries and a later connection can read again") {
    Engines engines;
    engines.reads->hold("materials/held.vmt");
    auto access = engines.previewAccess;
    auto read = std::async(std::launch::async, [access]() {
        access->lock();
        const std::unique_lock<ui::qt::EngineAccess> device(*access, std::adopt_lock);
        auto* engine = access->engine();
        return engine && engine->read_text_file("materials/held.vmt").ok();
    });
    UT_CHECK(wait_until([&]() { return !engines.reads->recorded().empty(); }));
    const auto start = std::chrono::steady_clock::now();
    access->setEngine(nullptr);
    UT_CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    UT_CHECK(!read.get());
    UT_CHECK(access->engine() == nullptr);
    engines.reads->release();
    access->setEngine(&engines.preview);
    UT_CHECK(engines.preview.read_text_file("materials/held.vmt").ok());
}

UT_TEST_CASE("Qt connection: discovery fills the catalogue without changing remembered paths") {
    QTemporaryDir directory;
    QDir().mkpath(directory.filePath("game/bin"));
    QFile gameinfo(directory.filePath("game/gameinfo.txt"));
    UT_CHECK(gameinfo.open(QIODevice::WriteOnly));
    gameinfo.write("GameInfo { game \"Fixture\" }\n");
    gameinfo.close();
    ui::UISettings settings;
    settings.last_bin_dir = directory.filePath("game/bin").toStdString();
    const auto remembered = settings.last_bin_dir;
    ui::qt::ConnectionDialog dialog(settings);
    auto* games = dialog.findChild<QComboBox*>();
    UT_CHECK(games && !games->isEnabled());
    UT_CHECK(wait_until([&]() { return games && games->isEnabled(); }));
    UT_CHECK(games && games->count() == 2);
    UT_CHECK(settings.last_bin_dir == remembered);
    UT_CHECK(settings.last_game_dir.empty());
    dialog.reject();
}

UT_TEST_CASE("Qt assets: sort keys retain column order, UTF-8 paths and persistent selections") {
    using Model = ui::qt::AssetListModel;
    ui::AssetIndex index;
    index.add_scanned_assets({{"Materials/Zzz.vmt", false}, {"materials/apple.vtf", true},
        {"materials/Sub/c.vmt", false}, {"materials/sub/A.vtf", true}, {"models/a.mdl", false},
        {"Materials/a.vmt", true}, {u8"materials/é.vmt", false}, {u8"materials/ä.vtf", true}}, ui::SourcePacked);
    Model model;
    model.setIndex(&index);
    model.refresh();
    QStringList names;
    for (const auto& entry : index.entries()) names.append(QString::fromStdString(entry.virtual_path));
    const std::vector<QStringList> ascending = {
        names,
        {"Materials/a.vmt", "materials/Sub/c.vmt", "Materials/Zzz.vmt", QString::fromUtf8(u8"materials/é.vmt"),
         "materials/apple.vtf", "materials/sub/A.vtf", QString::fromUtf8(u8"materials/ä.vtf"), "models/a.mdl"},
        {"Materials/a.vmt", "materials/apple.vtf", "Materials/Zzz.vmt", QString::fromUtf8(u8"materials/ä.vtf"),
         QString::fromUtf8(u8"materials/é.vmt"), "materials/sub/A.vtf", "materials/Sub/c.vmt", "models/a.mdl"},
        {"Materials/a.vmt", "materials/apple.vtf", "materials/sub/A.vtf", QString::fromUtf8(u8"materials/ä.vtf"),
         "materials/Sub/c.vmt", "Materials/Zzz.vmt", QString::fromUtf8(u8"materials/é.vmt"), "models/a.mdl"},
        {"Materials/a.vmt", "materials/apple.vtf", "materials/sub/A.vtf", "materials/Sub/c.vmt", "Materials/Zzz.vmt",
         QString::fromUtf8(u8"materials/ä.vtf"), QString::fromUtf8(u8"materials/é.vmt"), "models/a.mdl"}
    };
    const auto shown = [&]() {
        QStringList paths;
        for (int row = 0; row < model.rowCount(); ++row) paths.append(model.virtualPathAt(model.index(row, 0)));
        return paths;
    };
    for (int column = 0; column < Model::ColumnCount; ++column) {
        const QPersistentModelIndex selected(model.indexForPath("materials/sub/A.vtf"));
        auto expected = ascending[static_cast<std::size_t>(column)];
        model.setSort(column, Qt::DescendingOrder);
        std::reverse(expected.begin(), expected.end());
        UT_CHECK(shown() == expected);
        UT_CHECK(selected.isValid() && selected.data(Model::VirtualPathRole).toString() == "materials/sub/A.vtf");
        model.setSort(column, Qt::AscendingOrder);
        std::reverse(expected.begin(), expected.end());
        UT_CHECK(shown() == expected);
        UT_CHECK(selected.isValid() && selected.data(Model::VirtualPathRole).toString() == "materials/sub/A.vtf");
        model.refresh();
        UT_CHECK(shown() == expected);
    }
    ui::AssetFilter filter;
    filter.tokens = {"sub"};
    model.setSort(Model::TypeColumn, Qt::AscendingOrder);
    model.setFilter(filter); model.refresh();
    UT_CHECK(shown() == QStringList({"materials/Sub/c.vmt", "materials/sub/A.vtf"}));
}

UT_TEST_CASE("Qt assets: bulk loose reads retain all binary bytes, Unicode paths and empty files") {
    QTemporaryDir directory;
    const QString filename = directory.filePath(QString::fromUtf8(u8"textura_ñ.vtf"));
    QFile file(filename);
    UT_CHECK(file.open(QIODevice::WriteOnly));
    std::vector<unsigned char> expected(100003);
    for (std::size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<unsigned char>(i % 251);
    UT_CHECK(file.write(reinterpret_cast<const char*>(expected.data()), static_cast<qint64>(expected.size())) ==
             static_cast<qint64>(expected.size()));
    file.close();
    ui::qt::GameTextureSource source({}, {std::filesystem::path(directory.path().toStdWString())});
    auto bytes = source.read(u8"textura_ñ.vtf");
    UT_CHECK(bytes.ok() && bytes.value() == expected);
    UT_CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.close();
    bytes = source.read(u8"textura_ñ.vtf");
    UT_CHECK(bytes.ok() && bytes.value().empty());
    UT_CHECK(!source.read("missing.vtf"));
}

UT_TEST_CASE("Qt assets: closing active loads destroys delivery objects on the UI thread") {
    class Deliveries : public QObject {
    public:
        std::vector<QObject*> seen;
        std::atomic<int> destroyed{0};
        std::atomic<int> wrongThread{0};
        Deliveries() { qApp->installEventFilter(this); }
        ~Deliveries() override { qApp->removeEventFilter(this); }
        bool eventFilter(QObject* object, QEvent* event) override {
            if (event->type() != QEvent::MetaCall ||
                (object->objectName() != "assetPreviewDelivery" && object->objectName() != "assetThumbnailDelivery") ||
                std::find(seen.begin(), seen.end(), object) != seen.end()) return false;
            seen.push_back(object);
            QThread* expected = qApp->thread();
            connect(object, &QObject::destroyed, this, [this, expected]() {
                if (QThread::currentThread() != expected) wrongThread.fetch_add(1);
                destroyed.fetch_add(1);
            }, Qt::DirectConnection);
            return false;
        }
    } deliveries;
    Engines engines;
    auto panel = std::make_unique<ui::qt::AssetPreviewPanel>(engines.previewAccess);
    auto thumbnails = std::make_unique<ui::qt::AssetThumbnailService>(engines.thumbnailAccess);
    show_material(*panel, "materials/warm.vmt");
    thumbnails->request("materials/warm_thumbnail.vmt", {});
    UT_CHECK(wait_until([&]() { return deliveries.seen.size() == 2 && thumbnails->pendingCount() == 0; }));
    engines.reads->hold("materials/held.vmt");
    show_material(*panel, "materials/held.vmt");
    UT_CHECK(wait_until([&]() {
        const auto paths = engines.reads->recorded();
        return std::find(paths.begin(), paths.end(), "materials/held.vmt") != paths.end();
    }));
    thumbnails->request("materials/cancelled_thumbnail.vmt", {});
    panel.reset(); thumbnails.reset();
    engines.reads->release();
    UT_CHECK(QThreadPool::globalInstance()->waitForDone(5000));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    UT_CHECK(deliveries.destroyed.load() == 2);
    UT_CHECK(deliveries.wrongThread.load() == 0);
}

UT_TEST_CASE("Qt updater: cancelling a live archive extractor stops its child without applying an update") {
    QTemporaryDir directory;
    const QString archive = directory.filePath("fixture.zip");
    QString escaped = archive;
    escaped.replace("'", "''");
    // Many empty entries keep the real extractor alive long enough to observe
    // it, without storing a large binary fixture or writing a large payload.
    QProcess generator;
    generator.start("powershell.exe", {"-NoProfile", "-NonInteractive", "-Command",
        "$ErrorActionPreference='Stop'; Add-Type -AssemblyName System.IO.Compression.FileSystem; "
        "$z=[IO.Compression.ZipFile]::Open('" + escaped + "','Create'); "
        "try { for($i=0;$i -lt 8192;$i++){ [void]$z.CreateEntry(('fixture/file{0}.txt' -f $i)) } } finally { $z.Dispose() }"});
    const bool generated = generator.waitForFinished(15000) && generator.exitCode() == 0;
    UT_CHECK(generated);
    if (!generated) return;
    ui::qt::AppUpdater updater;
    bool cancelled = false;
    bool failed = false;
    QObject::connect(&updater, &ui::qt::AppUpdater::updateCancelled, &updater, [&]() { cancelled = true; });
    QObject::connect(&updater, &ui::qt::AppUpdater::updateFailed, &updater, [&](const QString&) { failed = true; });
    updater.applyUpdate(archive);
    HANDLE child = nullptr;
    const bool observed = wait_until([&]() {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
        for (BOOL found = Process32FirstW(snapshot, &entry); found; found = Process32NextW(snapshot, &entry)) {
            if (entry.th32ParentProcessID != GetCurrentProcessId() ||
                (_wcsicmp(entry.szExeFile, L"tar.exe") != 0 && _wcsicmp(entry.szExeFile, L"powershell.exe") != 0)) continue;
            HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            DWORD exit_code = 0;
            if (process && GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE) {
                child = process; break;
            }
            if (process) CloseHandle(process);
        }
        CloseHandle(snapshot);
        return child != nullptr;
    });
    UT_CHECK(observed);
    updater.cancelDownload();
    UT_CHECK(wait_until([&]() { return !updater.isPreparing(); }));
    UT_CHECK(cancelled && !failed);
    if (child) {
        UT_CHECK(WaitForSingleObject(child, 0) == WAIT_OBJECT_0);
        CloseHandle(child);
    }
    UT_CHECK(QDir(directory.path()).entryList({"extracted-*"}, QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}

UT_TEST_CASE("Qt updater: release selection skips debug archives") {
    ui::qt::ReleaseInfo release;
    release.assets = {{"vmtstudio-v0.2.2-win64-debug.zip", "https://example.test/debug.zip", 42},
                      {"vmtstudio-v0.2.2-win64.zip", "https://example.test/release.zip", 84}};
    UT_CHECK(release.bestAsset().downloadUrl == "https://example.test/release.zip");
    release.assets.removeLast();
    UT_CHECK(release.bestAsset().downloadUrl.isEmpty());
}

UT_TEST_CASE("Qt updater: application script copies literal special-character paths and retains a failed payload") {
    QTemporaryDir directory;
    const QString source = directory.filePath("source !%USERNAME%& O'Brien");
    const QString destination = directory.filePath("destination !%USERNAME%& O'Brien");
    UT_CHECK(QDir().mkpath(source));
    QFile executable(QDir(source).filePath("vmtstudio.exe"));
    UT_CHECK(executable.open(QIODevice::WriteOnly));
    executable.write("intentionally invalid executable");
    executable.close();
    const QString archive = directory.filePath("update.zip");
    QString escapedSource = source;
    QString escapedArchive = archive;
    escapedSource.replace("'", "''");
    escapedArchive.replace("'", "''");
    QProcess generator;
    generator.start("powershell.exe", {"-NoProfile", "-NonInteractive", "-Command",
        "$ErrorActionPreference='Stop'; Add-Type -AssemblyName System.IO.Compression.FileSystem; "
        "[IO.Compression.ZipFile]::CreateFromDirectory('" + escapedSource + "','" + escapedArchive + "')"});
    UT_CHECK(generator.waitForFinished(15000) && generator.exitCode() == 0);
    if (!QFile::exists(archive)) return;

    ui::qt::AppUpdater updater;
    updater.applyUpdate(archive);
    QString preparedScript;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    // Keep GUI events queued so a completed preparation cannot launch an update
    // against the real test installation before we cancel it.
    while (preparedScript.isEmpty() && std::chrono::steady_clock::now() < deadline) {
        QDirIterator files(directory.path(), {"apply_update.ps1"}, QDir::Files, QDirIterator::Subdirectories);
        if (files.hasNext()) {
            const QString candidate = files.next();
            QFile file(candidate);
            if (file.open(QIODevice::ReadOnly)) {
                const auto bytes = file.readAll();
                if (bytes.endsWith("}\n") || bytes.endsWith("}\r\n")) preparedScript = candidate;
            }
        }
        QThread::msleep(5);
    }
    const QString savedScript = directory.filePath("apply !%USERNAME%& O'Brien.ps1");
    const bool saved = !preparedScript.isEmpty() && QFile::copy(preparedScript, savedScript);
    updater.cancelDownload();
    UT_CHECK(wait_until([&]() { return !updater.isPreparing(); }));
    UT_CHECK(saved);
    if (!saved) return;

    QProcess apply;
    apply.start("powershell.exe", {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", savedScript,
        "-AppProcessId", "2147483647", "-SourceDir", QDir::toNativeSeparators(source),
        "-DestinationDir", QDir::toNativeSeparators(destination), "-ExecutableName", "vmtstudio.exe",
        "-CleanupDir", QDir::toNativeSeparators(source)});
    UT_CHECK(apply.waitForFinished(15000));
    // Launching the deliberately invalid exe must fail after copying. Recovery
    // files stay available, and the supplied path characters retain their bytes.
    UT_CHECK(apply.exitCode() != 0);
    QFile copied(QDir(destination).filePath("vmtstudio.exe"));
    UT_CHECK(copied.open(QIODevice::ReadOnly));
    UT_CHECK(copied.readAll() == "intentionally invalid executable");
    UT_CHECK(QFile::exists(QDir(source).filePath("vmtstudio.exe")));
    UT_CHECK(QFile::exists(archive));
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QThreadPool::globalInstance()->setMaxThreadCount(4);
    const int result = ut::run_all();
    QThreadPool::globalInstance()->waitForDone();
    return result;
}
