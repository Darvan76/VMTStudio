#pragma once

// Asset Browser thumbnails: decodes the visible grid cells off the UI thread
// and remembers the result in memory and on disk.
//
// Why this is a separate service: a grid of materials and textures can only be
// filled by decoding VMT/VTF files, and a
// VPK read goes through EngineView, which is neither cheap nor thread-safe.
// This service owns:
//   - the one gate every background EngineView access goes through
//     (EngineAccess), shared with the scan worker and the preview panel;
//   - a bounded worker queue (the global QThreadPool plus a small semaphore)
//     that skips stale generations and never blocks the UI thread;
//   - a UI-thread pixmap cache (~256 MB) and the per-game cache.pack, so
//     revisiting the same folder is instant across restarts.
//
// Threading contract:
//   - Every public method runs on the UI thread. The signal is emitted from the
//     UI thread as well; jobs post their result through a dispatcher object
//     that keeps the queued call safe even if the service dies mid-decode.
//   - Jobs never touch the service directly: they only hold shared pointers, so
//     closing the browser cannot leave a job with a dangling owner.

#include "bridge/engine_view.h"
#include "ui/texture_thumbnail.h"

#include <QCache>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QString>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ui {
class AssetPack;
namespace qt {

// Generation counter, worker slots and the delivery object shared with the
// pool jobs (defined in the .cpp so the job does not need the service).
struct ThumbnailShared;

// --- shared plumbing ---------------------------------------------------------

// FNV-1a 64 over the bytes of `text`. 64 bits because both the per-game cache
// folder and the thumbnail file names are built from paths; a 32-bit hash
// starts colliding at a few tens of thousands of entries.
std::uint64_t fnv1a64(std::string_view text);

// 16 lowercase hex chars identifying one game directory. The game path is
// lowercased and its separators normalized first, so "D:\Games\GMod\garrysmod"
// and "d:/games/gmod/garrysmod/" share one cache.
QString game_key_for(const std::filesystem::path& game_dir);

// Serializes every EngineView access made outside the UI thread.
//
// The bridge is a thin IPC client with no reentrancy guarantees, and the
// backend answers one request at a time, so concurrent readers would interleave
// response files. Two mutexes keep the critical sections honest:
//   - `pointer` is held for the time it takes to read/write m_engine;
//   - `device` is held for the whole round trip (a read, a dump, ...).
// setEngine(non-null) never waits for a scan in flight. Clearing it with
// setEngine(nullptr) does: the caller is about to destroy the EngineView, and
// every worker re-reads the pointer under `device` before using it.
class EngineAccess {
public:
    EngineAccess() = default;

    EngineAccess(const EngineAccess&) = delete;
    EngineAccess& operator=(const EngineAccess&) = delete;

    // UI thread. Safe to call while a worker holds deviceMutex().
    void setEngine(bridge::EngineView* engine);

    // Non-null only while a game is connected. Cheap, but callers that are
    // about to use the pointer must hold deviceMutex() first.
    bridge::EngineView* engine() const;

    // Waits for the current round trip to finish.
    void lock() noexcept;
    void unlock() noexcept;

    // True when no other thread is inside an engine call (thumbnails use this:
    // a stale thumbnail is better than a job that piles up behind a scan).
    bool tryLock() noexcept;

private:
    mutable QMutex m_pointerMutex;
    mutable QMutex m_deviceMutex;
    bridge::EngineView* m_engine = nullptr;
};

// Resolves a game path the way the browser needs it: disk first (the game dir
// and every user folder, with and without a leading "materials/"), then the
// engine file system. The engine call is routed through EngineAccess.
class GameTextureSource final : public TextureSource {
public:
    // How each engine round trip is serialized:
    //   Blocking - wait for the gate; the caller needs an answer (preview
    //              panel), and there is at most one such job at a time.
    //   Try      - skip the read when another job holds the gate (thumbnails
    //              during a scan); the caller can retry later.
    enum class LockMode { Blocking, Try };

    GameTextureSource(std::shared_ptr<EngineAccess> access,
                      std::vector<std::filesystem::path> roots,
                      LockMode mode = LockMode::Blocking);

    vmt::Result<std::vector<unsigned char>> read(const std::string& game_path) override;

    // True when the last read gave up because another thread was using the
    // engine (only possible in Try mode). The caller must then treat the whole
    // attempt as retryable instead of remembering a failure.
    bool last_engine_busy() const noexcept { return m_lastEngineBusy; }

private:
    std::shared_ptr<EngineAccess> m_access;
    std::vector<std::filesystem::path> m_roots;
    LockMode m_mode = LockMode::Blocking;
    bool m_lastEngineBusy = false;
};

// --- service -----------------------------------------------------------------

// One queued thumbnail. `diskPath` empty means the bytes must come from the
// engine (VPK entry). Paths already cached or already requested are ignored.
class AssetThumbnailService : public QObject {
    Q_OBJECT

public:
    explicit AssetThumbnailService(std::shared_ptr<EngineAccess> access, QObject* parent = nullptr);
    ~AssetThumbnailService() override;

    // Per-game pack (usually …/cache/<game_key>/cache.pack) and the signature
    // a packed asset's cache entry was produced from (the cache's `generated`
    // stamp). A null pack disables only the disk cache.
    void setGame(std::shared_ptr<AssetPack> pack, std::int64_t generated);

    // Search roots used to resolve a VMT's textures from disk: the game dir
    // plus every user folder. Copied on assignment.
    void setSearchRoots(std::vector<std::filesystem::path> roots);

    // Drops queued work (generation bump) and forgets what failed, without
    // discarding the decoded pixmaps. Used after a scan and when the engine or
    // the game changes.
    void invalidate();

    // Generation bump plus a full clear: a different game must not show the
    // previous game's pictures.
    void reset();

    void request(const QString& virtualPath, const QString& diskPath);

    bool contains(const QString& virtualPath) const;
    const QPixmap* pixmap(const QString& virtualPath) const;
    int pendingCount() const;

    // Memory cache ceiling in kilobytes; 256 MB matches the plan's LRU budget.
    static constexpr int kCacheBudgetKb = 256 * 1024;

signals:
    // Emitted on the UI thread. `image` is null when the thumbnail could not
    // be produced; `retryable` is true when the failure was "the engine was
    // busy" rather than a missing file or an undecodable format.
    void thumbnailReady(const QString& virtualPath, const QImage& image,
                        const QString& diskPath, bool retryable);
    void pendingChanged(int pending);

private:
    void onReady(const QString& virtualPath, const QImage& image, const QString& diskPath,
                 bool retryable, int generation);

    std::shared_ptr<EngineAccess> m_access;
    std::shared_ptr<ThumbnailShared> m_shared;          // generation + slots + dispatcher
    QCache<QString, QPixmap> m_pixels;                  // UI thread only

    std::shared_ptr<AssetPack> m_pack;
    std::vector<std::filesystem::path> m_roots;
    std::int64_t m_generated = 0;

    QSet<QString> m_inflight;  // UI thread
    QSet<QString> m_failed;    // UI thread: do not retry a broken decode this session
};

} // namespace qt
} // namespace ui
