#include "asset_thumbnail_service.h"

#include "ui/asset_index.h"
#include "ui/asset_pack.h"
#include "ui/material_browser.h"
#include "ui/texture_thumbnail.h"
#include "vtf/vtf_reader.h"

#include <QBuffer>
#include <QByteArray>
#include <QMetaObject>
#include <QSemaphore>
#include <QThreadPool>
#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace ui {
namespace qt {

namespace {

// How many thumbnails may be in flight at once. 3 keeps the whole pipeline on
// 2-4 threads as the plan asks, without ever saturating the global pool.
constexpr int kMaxConcurrent = 3;

// Longest edge of a grid thumbnail. 2x the ~100 px cell so it stays crisp if
// Windows scales the window, at a quarter of the memory of a 256 px decode.
constexpr int kThumbnailSize = 128;

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string path_to_slashes(std::string s) {
    for (char& c : s) {
        if (c == '\\') {
            c = '/';
        }
    }
    return s;
}

std::string hex64(std::uint64_t value) {
    char buffer[17] = {};
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(value));
    return buffer;
}

bool read_file_bytes(const std::filesystem::path& path, std::vector<unsigned char>& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return !stream.bad();
}

// Top-down BGRA -> QImage. The per-pixel loop (instead of a memcpy) keeps the
// channel order explicit for every endianness, which matters because the BMP
// writer below consumes the same bytes directly.
QImage image_from_bgra(const unsigned char* bgra, int width, int height) {
    if (bgra == nullptr || width <= 0 || height <= 0) {
        return {};
    }
    QImage image(width, height, QImage::Format_ARGB32);
    if (image.isNull()) {
        return {};
    }
    for (int y = 0; y < height; ++y) {
        const unsigned char* src = bgra + static_cast<std::size_t>(y) * width * 4;
        QRgb* dst = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            dst[x] = qRgba(src[x * 4 + 2], src[x * 4 + 1], src[x * 4 + 0], src[x * 4 + 3]);
        }
    }
    return image;
}

QImage image_from_rgba(const std::uint8_t* rgba, int width, int height) {
    if (rgba == nullptr || width <= 0 || height <= 0) {
        return {};
    }
    QImage image(width, height, QImage::Format_RGBA8888);
    if (image.isNull()) {
        return {};
    }
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src = rgba + static_cast<std::size_t>(y) * width * 4;
        std::memcpy(image.scanLine(y), src, static_cast<std::size_t>(width) * 4);
    }
    return image;
}

// QImage -> PNG bytes for the pack. The grid cache used to be 24-bit BMP;
// PNG keeps the same pixels in a fraction of the space and lets migration read
// the old BMPs transparently (QImage::fromData detects the format).
bool encode_thumbnail_png(const QImage& image, std::vector<unsigned char>& out) {
    if (image.isNull()) {
        return false;
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (!image.save(&buffer, "PNG")) {
        return false;
    }
    out.assign(bytes.begin(), bytes.end());
    return !out.empty();
}

// UI-thread object the pool posts through. Keeping the delivery in a separate
// QObject is what makes a job safe to outlive the service: the queued call
// holds a shared_ptr to this dispatcher, and a destroyed service has simply
// disconnected its slot.
class ThumbnailDispatcher : public QObject {
    Q_OBJECT
public:
    void deliver(const QString& virtualPath, const QImage& image, const QString& diskPath,
                 bool retryable, int generation) {
        emit ready(virtualPath, image, diskPath, retryable, generation);
    }

signals:
    void ready(QString virtualPath, QImage image, QString diskPath, bool retryable,
               int generation);
};

} // namespace
} // namespace qt
} // namespace ui

// The shared state lives in the header's forward declaration slot; it is
// defined here so the job does not need the service at all.
namespace ui {
namespace qt {

struct ThumbnailShared {
    std::atomic<int> generation{0};
    // Not named `slots`: moc treats that identifier as the Qt access keyword.
    std::shared_ptr<QSemaphore> semaphore = std::make_shared<QSemaphore>(kMaxConcurrent);
    std::shared_ptr<ThumbnailDispatcher> dispatcher = std::make_shared<ThumbnailDispatcher>();
};

namespace {

// Decodes one asset. Runs on a pool thread: it only touches shared pointers,
// the file system and the VTF decoder, never a widget.
class ThumbnailJob final : public QRunnable {
public:
    ThumbnailJob(std::shared_ptr<ThumbnailShared> shared, std::shared_ptr<EngineAccess> access,
                 std::vector<std::filesystem::path> roots, std::shared_ptr<ui::AssetPack> pack,
                 std::string pathLower, std::string path, std::filesystem::path diskPath,
                 std::uint64_t baseSignature, int generation)
        : m_shared(std::move(shared)),
          m_access(std::move(access)),
          m_roots(std::move(roots)),
          m_pack(std::move(pack)),
          m_pathLower(std::move(pathLower)),
          m_path(std::move(path)),
          m_diskPath(std::move(diskPath)),
          m_baseSignature(baseSignature),
          m_generation(generation) {}

    ~ThumbnailJob() override { m_shared->semaphore->release(); }

    void run() override {
        if (m_shared->generation.load() != m_generation) {
            return; // the service moved on (new filter, new game, closed)
        }

        // The signature stats the loose file, so it stays on the worker thread:
        // the UI thread must never wait on the file system. Packed assets use
        // the index's generated stamp.
        const std::uint64_t signature =
            ui::asset_signature(m_diskPath, m_baseSignature);
        const std::string key = ui::asset_pack_key(m_pathLower, signature);

        bool retryable = false;
        QImage image;
        if (m_pack) {
            std::vector<unsigned char> cached;
            if (m_pack->get(ui::kAssetPackTagThumb, key, cached) && !cached.empty()) {
                image = QImage::fromData(cached.data(), static_cast<int>(cached.size()));
            }
        }
        const bool from_cache = !image.isNull();
        if (!from_cache) {
            image = decode(retryable);
            if (!image.isNull() && m_pack &&
                m_shared->generation.load() == m_generation) {
                // Best effort: a read-only or full disk only costs a re-decode.
                std::vector<unsigned char> png;
                if (encode_thumbnail_png(image, png)) {
                    (void)m_pack->put(ui::kAssetPackTagThumb, key, png, false);
                }
            }
        }

        if (m_shared->generation.load() != m_generation) {
            return;
        }
        const std::shared_ptr<ThumbnailDispatcher> dispatcher = m_shared->dispatcher;
        const std::weak_ptr<ThumbnailDispatcher> weak = dispatcher;
        const QString virtual_path = QString::fromStdString(m_path);
        const QString disk_path = QString::fromStdWString(m_diskPath.wstring());
        const int generation = m_generation;
        QMetaObject::invokeMethod(dispatcher.get(), [weak, virtual_path, image, disk_path,
                                                     retryable, generation]() {
            if (const std::shared_ptr<ThumbnailDispatcher> alive = weak.lock()) {
                alive->deliver(virtual_path, image, disk_path, retryable, generation);
            }
        }, Qt::QueuedConnection);
    }

private:
    QImage decode(bool& retryable) const {
        retryable = false;
        const ui::AssetKind kind = ui::asset_kind_from_path(m_path);
        if (kind == ui::AssetKind::Other || kind == ui::AssetKind::Model) {
            return {}; // models get the delegate's placeholder, not a decode
        }

        GameTextureSource source(m_access, m_roots, GameTextureSource::LockMode::Try);
        const vmt::Result<std::vector<unsigned char>> bytes = source.read(m_path);
        if (!bytes) {
            retryable = source.last_engine_busy();
            return {};
        }

        if (kind == ui::AssetKind::Texture) {
            const std::vector<unsigned char>& data = bytes.value();
            const vmt::Result<vtf::RgbaImage> decoded = vtf::decode_vtf_preview_from_memory(
                data.data(), data.size(), 0, static_cast<std::uint32_t>(kThumbnailSize),
                std::filesystem::path(m_path));
            if (!decoded) {
                return {};
            }
            const vtf::RgbaImage& rgba = decoded.value();
            retryable = source.last_engine_busy();
            return image_from_rgba(rgba.pixels.data(), static_cast<int>(rgba.width),
                                   static_cast<int>(rgba.height));
        }

        // Material: the first texture the VMT references that resolves and
        // decodes wins (base texture first, then $textureN, then the rest).
        const std::vector<unsigned char>& data = bytes.value();
        const std::string text(data.begin(), data.end());
        const vmt::Result<TextureThumbnail> thumbnail =
            make_texture_thumbnail(text, source, static_cast<std::uint32_t>(kThumbnailSize));
        retryable = source.last_engine_busy();
        if (!thumbnail) {
            return {};
        }
        return image_from_bgra(thumbnail.value().bgra.data(), thumbnail.value().width,
                               thumbnail.value().height);
    }

    std::shared_ptr<ThumbnailShared> m_shared;
    std::shared_ptr<EngineAccess> m_access;
    std::vector<std::filesystem::path> m_roots;
    std::shared_ptr<ui::AssetPack> m_pack;
    std::string m_pathLower;
    std::string m_path;
    std::filesystem::path m_diskPath;
    std::uint64_t m_baseSignature = 0;
    int m_generation = 0;
};

} // namespace

// --- path helpers -------------------------------------------------------------

std::uint64_t fnv1a64(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

QString game_key_for(const std::filesystem::path& game_dir) {
    if (game_dir.empty()) {
        return {};
    }
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(game_dir, ec);
    if (ec) {
        absolute = game_dir;
    }
    std::string key = to_lower(path_to_slashes(absolute.string()));
    // A trailing separator must not produce a second cache for the same game
    // ("…/garrysmod" vs "…/garrysmod/"). Keep a drive root ("c:/") intact.
    while (key.size() > 1 && key.back() == '/') {
        if (key.size() == 3 && key[1] == ':') {
            break;
        }
        key.pop_back();
    }
    return QString::fromStdString(hex64(fnv1a64(key)));
}

// --- EngineAccess -------------------------------------------------------------

void EngineAccess::setEngine(bridge::EngineView* engine) {
    if (engine == nullptr) {
        // Clearing the pointer is the lifecycle path: MainWindow destroys or
        // replaces the EngineView right after this call, so wait for a call in
        // flight to leave before returning. Workers re-read the pointer under
        // this same gate, which makes queued jobs a no-op too.
        QMutexLocker device(&m_deviceMutex);
        QMutexLocker pointer(&m_pointerMutex);
        m_engine = nullptr;
        return;
    }
    QMutexLocker locker(&m_pointerMutex);
    m_engine = engine;
}

bridge::EngineView* EngineAccess::engine() const {
    QMutexLocker locker(&m_pointerMutex);
    return m_engine;
}

void EngineAccess::lock() noexcept {
    m_deviceMutex.lock();
}

void EngineAccess::unlock() noexcept {
    m_deviceMutex.unlock();
}

bool EngineAccess::tryLock() noexcept {
    return m_deviceMutex.tryLock();
}

// --- GameTextureSource --------------------------------------------------------

GameTextureSource::GameTextureSource(std::shared_ptr<EngineAccess> access,
                                     std::vector<std::filesystem::path> roots,
                                     LockMode mode)
    : m_access(std::move(access)), m_roots(std::move(roots)), m_mode(mode) {}

vmt::Result<std::vector<unsigned char>> GameTextureSource::read(const std::string& game_path) {
    m_lastEngineBusy = false;

    // Disk first: a loose copy is the copy the user is editing.
    std::error_code ec;
    for (const std::filesystem::path& root : m_roots) {
        std::vector<std::filesystem::path> candidates;
        candidates.push_back(root / std::filesystem::path(game_path));
        if (game_path.rfind("materials/", 0) == 0) {
            // The user added the materials folder itself instead of the folder
            // that contains it; both spellings must resolve.
            candidates.push_back(root / std::filesystem::path(game_path.substr(10)));
        }
        for (const std::filesystem::path& candidate : candidates) {
            if (!std::filesystem::is_regular_file(candidate, ec)) {
                continue;
            }
            std::vector<unsigned char> bytes;
            if (read_file_bytes(candidate, bytes)) {
                return vmt::Ok(std::move(bytes));
            }
        }
    }

    bridge::EngineView* engine = m_access ? m_access->engine() : nullptr;
    if (engine == nullptr) {
        return vmt::Err<std::vector<unsigned char>>(vmt::make_error(
            vmt::ErrorCode::NotFound, "not found: " + game_path,
            "Connect a game to read assets stored inside VPKs"));
    }

    if (m_mode == LockMode::Blocking) {
        m_access->lock();
    } else if (!m_access->tryLock()) {
        m_lastEngineBusy = true;
        return vmt::Err<std::vector<unsigned char>>(vmt::make_error(
            vmt::ErrorCode::Internal, "the engine is busy: " + game_path));
    }

    // The pointer may have changed while waiting for the gate.
    engine = m_access->engine();
    vmt::Result<std::vector<unsigned char>> result =
        vmt::Err<std::vector<unsigned char>>(vmt::make_error(
            vmt::ErrorCode::NotFound, "no engine connected: " + game_path));
    if (engine != nullptr && engine->initialized()) {
        result = engine->read_binary_file(game_path);
    }
    m_access->unlock();
    return result;
}

// --- AssetThumbnailService ----------------------------------------------------

AssetThumbnailService::AssetThumbnailService(std::shared_ptr<EngineAccess> access, QObject* parent)
    : QObject(parent), m_access(std::move(access)), m_shared(std::make_shared<ThumbnailShared>()) {
    m_pixels.setMaxCost(kCacheBudgetKb);
    connect(m_shared->dispatcher.get(), &ThumbnailDispatcher::ready, this,
            &AssetThumbnailService::onReady);
}

AssetThumbnailService::~AssetThumbnailService() {
    // Jobs that have not started yet check this before doing any work; jobs
    // already decoding finish and drop their result through the dispatcher.
    m_shared->generation.fetch_add(1);
}

void AssetThumbnailService::setGame(std::shared_ptr<AssetPack> pack,
                                    std::int64_t generated) {
    if (m_pack == pack && m_generated == generated) {
        return;
    }
    m_pack = std::move(pack);
    m_generated = generated;
    m_failed.clear();
}

void AssetThumbnailService::setSearchRoots(std::vector<std::filesystem::path> roots) {
    m_roots = std::move(roots);
}

void AssetThumbnailService::invalidate() {
    // Queued jobs stop early; already delivered pictures stay (their paths did
    // not change), but a path that failed during the scan gets another chance.
    m_shared->generation.fetch_add(1);
    m_failed.clear();
    m_inflight.clear();
    emit pendingChanged(0);
}

void AssetThumbnailService::reset() {
    m_shared->generation.fetch_add(1);
    m_pixels.clear();
    m_inflight.clear();
    m_failed.clear();
    emit pendingChanged(0);
}

void AssetThumbnailService::request(const QString& virtualPath, const QString& diskPath) {
    if (virtualPath.isEmpty()) {
        return;
    }
    const ui::AssetKind kind = ui::asset_kind_from_path(virtualPath.toStdString());
    if (kind != ui::AssetKind::Material && kind != ui::AssetKind::Texture) {
        return; // models and unknowns only ever show the type placeholder
    }
    if (m_pixels.contains(virtualPath) || m_inflight.contains(virtualPath) ||
        m_failed.contains(virtualPath)) {
        return;
    }
    // Back-pressure: with every slot taken the request is simply dropped. The
    // browser re-asks for whatever is still visible on its next pump, so a fast
    // scroll never builds a queue of stale work.
    if (!m_shared->semaphore->tryAcquire()) {
        return;
    }

    // The cache key uses the lowercased path (the index dedups that way); the
    // engine read keeps the original spelling, which is what the game's own
    // file system was asked about.
    const std::string path = virtualPath.toStdString();
    std::string path_lower = path;
    for (char& c : path_lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    m_inflight.insert(virtualPath);
    const int generation = m_shared->generation.load();
    auto* job = new ThumbnailJob(m_shared, m_access, m_roots, m_pack,
                                 std::move(path_lower), path,
                                 std::filesystem::path(diskPath.toStdWString()),
                                 static_cast<std::uint64_t>(m_generated), generation);
    QThreadPool::globalInstance()->start(job);
    emit pendingChanged(static_cast<int>(m_inflight.size()));
}

bool AssetThumbnailService::contains(const QString& virtualPath) const {
    return m_pixels.contains(virtualPath);
}

const QPixmap* AssetThumbnailService::pixmap(const QString& virtualPath) const {
    return m_pixels.object(virtualPath);
}

int AssetThumbnailService::pendingCount() const {
    return static_cast<int>(m_inflight.size());
}

void AssetThumbnailService::onReady(const QString& virtualPath, const QImage& image,
                                    const QString& diskPath, bool retryable, int generation) {
    if (generation != m_shared->generation.load()) {
        // invalidate()/reset() already cleared the in-flight set. Removing the
        // path here could drop the bookkeeping of a newer job for the same
        // asset, so only report the current count.
        emit pendingChanged(static_cast<int>(m_inflight.size()));
        return;
    }
    m_inflight.remove(virtualPath);

    if (image.isNull()) {
        if (!retryable) {
            m_failed.insert(virtualPath);
        }
        emit pendingChanged(static_cast<int>(m_inflight.size()));
        return;
    }

    const int bytes = image.width() * image.height() * 4;
    const int cost = qMax(1, (bytes + 1023) / 1024);
    m_pixels.insert(virtualPath, new QPixmap(QPixmap::fromImage(image)), cost);
    emit thumbnailReady(virtualPath, image, diskPath, false);
    emit pendingChanged(static_cast<int>(m_inflight.size()));
}

} // namespace qt
} // namespace ui

// AUTOMOC: the dispatcher's signal lives in this translation unit.
#include "asset_thumbnail_service.moc"
