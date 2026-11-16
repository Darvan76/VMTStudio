// Manual, deterministic workloads over the product's real core and Qt widgets.
// Timings are observations, never machine-dependent pass/fail thresholds.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include "asset_list_model.h"
#include "asset_browser_dialog.h"
#include "asset_preview_panel.h"
#include "asset_thumbnail_service.h"
#include "inspector_widget.h"
#include "schema_resources.h"
#include "vmt_highlighter.h"
#include "ui/asset_cache.h"
#include "ui/asset_pack.h"
#include "ui/material_info.h"
#include "ui/preview_sync.h"
#include "ui/session.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr double kMiB = 1024.0 * 1024.0;
QJsonArray measurements;
QJsonObject details;
int sample_count = 15;
double largest_ui_gap_ms = 0;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<class T>
void require_result(const vmt::Result<T>& result) {
    if (!result) throw std::runtime_error(result.error().message + ": " + result.error().detail);
}

QJsonObject memory(HANDLE process = GetCurrentProcess()) {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    require(GetProcessMemoryInfo(process,
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != FALSE,
        "Cannot read process memory counters");
    return {{"private_mib", static_cast<double>(counters.PrivateUsage) / kMiB},
            {"working_set_mib", static_cast<double>(counters.WorkingSetSize) / kMiB},
            {"peak_working_set_mib", static_cast<double>(counters.PeakWorkingSetSize) / kMiB}};
}

template<class Work>
void measure(const char* name, Work work, bool first_only = false) {
    std::cerr << "Measuring " << name << '\n';
    if (!first_only) work(); // warm OS-cache/component measurements
    const int repetitions = first_only ? 1 : sample_count;
    std::vector<double> times;
    times.reserve(repetitions);
    for (int i = 0; i < repetitions; ++i) {
        const auto start = Clock::now();
        work();
        times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    std::sort(times.begin(), times.end());
    auto result = memory();
    result.insert("metric", name);
    result.insert("samples", repetitions);
    result.insert("warmup", first_only ? 0 : 1);
    result.insert("median_ms", times[times.size() / 2]);
    result.insert("p95_ms", times[(times.size() * 95 + 99) / 100 - 1]);
    result.insert("min_ms", times.front());
    result.insert("max_ms", times.back());
    measurements.append(result);
    std::cerr << name << ": " << times[times.size() / 2] << " ms\n";
}

template<class Ready>
void wait_until(Ready ready) {
    const auto deadline = Clock::now() + std::chrono::seconds(90);
    auto previous = Clock::now();
    while (!ready()) {
        QApplication::processEvents();
        // Offscreen Qt has no native windows of its own. Service cross-thread
        // sent messages too, as MainWindow does while draining a native worker.
        MSG message{};
        (void)PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        const auto now = Clock::now();
        largest_ui_gap_ms = std::max(largest_ui_gap_ms,
            std::chrono::duration<double, std::milli>(now - previous).count());
        previous = now;
        require(now < deadline, "Worker did not complete within 90 seconds");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::filesystem::path native_path(const QString& text) { return text.toStdWString(); }

void write_file(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(file), "Cannot write benchmark fixture");
}

void browser(int count, const QString& root) {
    std::vector<ui::ScannedAsset> rows;
    rows.reserve(count);
    for (int i = 0; i < count; ++i) {
        char path[160]{};
        std::snprintf(path, sizeof(path), "%s/Pack_%03d/Workshop_Long_Folder_%03d/Asset_%06d.%s",
            i % 3 == 2 ? "models" : "materials", i % 97, i % 131, i,
            i % 3 == 0 ? "vmt" : (i % 3 == 1 ? "vtf" : "mdl"));
        rows.push_back({path, i % 5 == 0});
    }
    ui::AssetIndex index;
    measure("index_ingest", [&]() {
        index.clear();
        require(index.add_scanned_assets(rows, ui::SourcePacked) == static_cast<std::size_t>(count),
                "Index lost fixture rows");
    });
    rows.clear();
    rows.shrink_to_fit();
    ui::AssetCacheMeta meta;
    meta.game_dir = native_path(root + "/game").u8string();
    std::string serialized;
    measure("index_serialize", [&]() { serialized = ui::serialize_asset_cache(index, meta); });
    measure("index_parse", [&]() {
        const auto loaded = ui::parse_asset_cache(serialized, meta.game_dir);
        require(loaded.loaded && loaded.index.size() == index.size(), "Cache parse lost fixture rows");
    });
    const auto cache_path = native_path(root + "/cache.pack");
    {
        ui::AssetPack pack;
        require(pack.open(cache_path), "Cannot create fixture cache");
        require(pack.put(ui::kAssetPackTagMeta, "index", {serialized.begin(), serialized.end()}, false),
                "Cannot store fixture index");
    }
    measure("index_open_pack_parse", [&]() {
        ui::AssetPack pack;
        require(pack.open(cache_path, false), "Cannot open fixture cache");
        std::vector<unsigned char> bytes;
        require(pack.get(ui::kAssetPackTagMeta, "index", bytes), "Cannot read fixture cache");
        const auto loaded = ui::parse_asset_cache(
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), meta.game_dir);
        require(loaded.loaded && loaded.index.size() == index.size(), "Pack load lost fixture rows");
    });
    ui::qt::AssetListModel model;
    model.setIndex(&index);
    measure("search_default_empty", [&]() { model.setFilter({}); model.refresh(); });
    ui::AssetFilter filter;
    filter.tokens = {"workshop", "asset_0"};
    measure("search_default_broad", [&]() {
        model.setFilter(filter); model.refresh();
        require(model.visibleRowCount() > 0, "Broad search found no fixture rows");
    });
    filter.tokens = {"no_such_asset"};
    measure("search_default_miss", [&]() {
        model.setFilter(filter); model.refresh();
        require(model.visibleRowCount() == 0, "Missing token returned rows");
    });
    model.setFilter({}); model.refresh();
    bool descending = false;
    measure("sort_type", [&]() {
        descending = !descending;
        model.setSort(ui::qt::AssetListModel::TypeColumn,
                      descending ? Qt::DescendingOrder : Qt::AscendingOrder);
    });
    measure("search_sorted_empty", [&]() { model.setFilter({}); model.refresh(); });
    filter.tokens = {"workshop", "asset_0"};
    measure("search_sorted_broad", [&]() { model.setFilter(filter); model.refresh(); });
    filter.tokens = {"no_such_asset"};
    measure("search_sorted_miss", [&]() { model.setFilter(filter); model.refresh(); });
    model.setFilter({}); model.refresh();
    const QString last = QString::fromStdString(index.entries().back().virtual_path);
    measure("locate_selected_path", [&]() { require(model.indexForPath(last).isValid(), "Selection lost"); });
}

void browser_widgets(int count, const QString& root) {
    const auto game_dir = native_path(root + "/game");
    const auto app_data = native_path(root + "/browser-data");
    const auto pack_path = app_data / "cache" / ui::qt::game_key_for(game_dir).toStdWString() / "cache.pack";
    std::filesystem::create_directories(pack_path.parent_path());
    std::filesystem::copy_file(native_path(root + "/cache.pack"), pack_path);
    ui::qt::AssetBrowserDialog dialog(nullptr, app_data);
    auto* model = dialog.findChild<ui::qt::AssetListModel*>();
    auto* search = dialog.findChild<QLineEdit*>();
    require(model && search, "Missing browser widgets");
    measure("browser_open_visible", [&]() {
        dialog.setGame(game_dir); dialog.show();
        wait_until([&]() { return model->rowCount() == count; });
        require(!dialog.grab().isNull(), "Cannot render the browser");
    }, true);
    int resets = 0;
    const auto reset_connection = QObject::connect(model, &QAbstractItemModel::modelReset,
        &dialog, [&]() { ++resets; });
    bool uppercase = false;
    const auto search_visible = [&]() {
        uppercase = !uppercase;
        const int before = resets;
        search->setText(uppercase ? "ASSET_0" : "asset_0");
        wait_until([&]() { return resets > before; });
        require(model->rowCount() == std::min(count, 100000), "Browser search returned wrong rows");
        require(!dialog.grab().isNull(), "Cannot render filtered results");
    };
    auto state = dialog.state();
    state.sortColumn = ui::qt::AssetListModel::TypeColumn;
    state.sortAsc = true;
    dialog.setState(state);
    measure("browser_table_search_visible", search_visible);
    state.grid = true; dialog.setState(state);
    measure("browser_grid_search_visible", search_visible);
    QObject::disconnect(reset_connection);
    dialog.hide();
    QThreadPool::globalInstance()->waitForDone();
}

void editor(int count, const QString& root) {
    const auto& schemas = ui::qt::bundled_schemas();
    require_result(schemas);
    std::string text = "LightmappedGeneric\n{\n\t\"$basetexture\" \"brick/brickwall001a\"\n";
    for (int i = 0; i < count; ++i) {
        text += "\t\"$fixture_" + std::to_string(i) + "\" \"" + std::to_string(i) + "\" // retained comment\n";
    }
    text += "}\n";
    const auto path = native_path(root + "/large.vmt");
    write_file(path, text);
    ui::EditorSession session;
    measure("open_parse", [&]() { require_result(session.open_file(path)); require(session.text_is_valid(), "Invalid VMT"); });
    QPlainTextEdit code;
    ui::qt::VmtHighlighter highlighter(code.document());
    ui::qt::InspectorWidget inspector;
    code.resize(800, 650); inspector.resize(400, 650);
    code.show(); inspector.show();
    const auto open_editor = [&]() {
        require_result(session.open_file(path));
        code.setPlainText(QString::fromStdString(session.text()));
        inspector.updateFromDocument(session.document(), schemas.value(), true);
        QApplication::processEvents();
    };
    measure("open_editor_first", open_editor, true);
    measure("open_editor_inspector", open_editor);
    bool alternate = false;
    measure("typing_parse_inspector", [&]() {
        alternate = !alternate;
        std::string changed = text;
        const auto location = changed.find("brickwall001a");
        changed[location + 12] = alternate ? 'b' : 'a';
        require_result(session.set_text(std::move(changed)));
        inspector.updateFromDocument(session.document(), schemas.value(), true);
    });
    measure("panel_edit_parse_inspector", [&]() {
        alternate = !alternate;
        require_result(session.set_parameter("$fixture_0", alternate ? "123" : "456"));
        code.setPlainText(QString::fromStdString(session.text()));
        inspector.updateFromDocument(session.document(), schemas.value(), true);
        QApplication::processEvents();
    });
    measure("save_atomic", [&]() { require_result(session.save()); });
    ui::PreviewSync preview;
    preview.configure([&](const std::string& bytes) {
        return ui::write_preview_material(native_path(root + "/preview"), bytes).ok();
    }, []() { return true; });
    preview.set_active(true);
    measure("preview_write_only", [&]() { require_result(preview.flush_now(session.text(), true)); });
    require(session.text().find("// retained comment") != std::string::npos, "Edit lost comments");
}

void write_texture(const std::filesystem::path& path, unsigned edge) {
    std::array<unsigned char, 80> header{};
    const auto put = [&header](std::size_t offset, std::uint32_t value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) header[offset + i] = static_cast<unsigned char>(value >> (i * 8));
    };
    header[0] = 'V'; header[1] = 'T'; header[2] = 'F';
    put(4, 7, 4); put(8, 2, 4); put(12, 80, 4);
    put(16, edge, 2); put(18, edge, 2); put(24, 1, 2);
    put(48, 0x3f800000u, 4); put(57, 0xffffffffu, 4); put(63, 1, 2);
    unsigned mip_count = 1;
    for (unsigned dimension = edge; dimension > 1; dimension >>= 1) ++mip_count;
    header[56] = static_cast<unsigned char>(mip_count);
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(header.data()), header.size());
    std::array<unsigned char, 65536> block{};
    for (int mip = static_cast<int>(mip_count) - 1; mip >= 0; --mip) {
        for (std::size_t i = 0; i < block.size(); i += 4) {
            block[i] = static_cast<unsigned char>(20 + mip * 12);
            block[i + 1] = 40; block[i + 2] = 60; block[i + 3] = 255;
        }
        const std::uint64_t side = std::max(1u, edge >> mip);
        std::uint64_t remaining = side * side * 4;
        while (remaining != 0) {
            const auto bytes = static_cast<std::streamsize>(std::min<std::uint64_t>(remaining, block.size()));
            file.write(reinterpret_cast<const char*>(block.data()), bytes);
            remaining -= bytes;
        }
    }
    require(static_cast<bool>(file), "Cannot write VTF fixture");
}

void texture(unsigned edge, const QString& root) {
    require(QDir().mkpath(root + "/materials"), "Cannot create materials fixture");
    const auto path = native_path(root + "/materials/large.vtf");
    write_texture(path, edge);
    auto access = std::make_shared<ui::qt::EngineAccess>();
    ui::qt::GameTextureSource source(access, {native_path(root)});
    measure("vtf_metadata", [&]() {
        auto result = source.metadata("materials/large.vtf");
        require_result(result); require_result(result.value().info);
        require(result.value().info.value().width == edge, "Wrong header dimensions");
    });
    ui::EditorSession session;
    measure("vtf_session_open", [&]() { require_result(session.open_file(path)); require(session.is_vtf(), "Not a VTF session"); });
    measure("vtf_thumbnail_decode_128", [&]() {
        const auto image = vtf::decode_vtf_preview(path, 0, 128);
        require_result(image); require(image.value().width == 128, "Wrong decoded mip");
    });
    ui::qt::AssetPreviewPanel panel(access);
    panel.setSearchRoots({native_path(root)}); panel.resize(400, 700); panel.show();
    auto* info = panel.findChild<QLabel*>("assetPreviewInfo");
    require(info != nullptr, "Missing preview result label");
    measure("vtf_panel_preview_512", [&]() {
        panel.clear();
        panel.showAsset("materials/large.vtf", static_cast<int>(ui::AssetKind::Texture), {}, ui::SourceLoose, 0);
        wait_until([&]() { return info->text().contains(QString::number(edge) + " x " + QString::number(edge)); });
        bool has_image = false;
        for (auto* label : panel.findChildren<QLabel*>()) has_image |= !label->pixmap().isNull();
        require(has_image, "Preview failed to decode");
    });
    ui::qt::AssetThumbnailService thumbnails(access);
    thumbnails.setSearchRoots({native_path(root)});
    measure("vtf_service_thumbnail_128", [&]() {
        thumbnails.invalidate();
        thumbnails.request("materials/large.vtf", QString::fromStdWString(path.wstring()));
        wait_until([&]() { return thumbnails.pendingCount() == 0; });
        require(thumbnails.contains("materials/large.vtf"), "Thumbnail service failed");
    });
    QThreadPool::globalInstance()->waitForDone();
}

template<class Work>
auto background(Work work) {
    auto job = std::async(std::launch::async, std::move(work));
    wait_until([&]() { return job.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; });
    return job.get();
}

void engine(const QString& bin, const QString& game, const QString& root) {
    // A real Win32 host, kept hidden: the offscreen Qt plugin has no HWND.
    using Window = std::unique_ptr<std::remove_pointer_t<HWND>, decltype(&DestroyWindow)>;
    Window host(CreateWindowExW(0, L"STATIC", L"VMT performance host", WS_OVERLAPPED,
        0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr), &DestroyWindow);
    require(host != nullptr, "Cannot create native benchmark host");
    auto created = bridge::EngineView::create();
    require_result(created);
    auto view = std::move(created.value());
    bridge::EngineViewConfig config;
    config.bin_dir = native_path(bin); config.game_dir = native_path(game);
    config.child_window = host.get(); config.width = 800; config.height = 600;
    measure("engine_startup", [&]() {
        require_result(background([&]() { return view->initialize(config); }));
    }, true);
    DWORD backend_pid = 0;
    GetWindowThreadProcessId(GetWindow(host.get(), GW_CHILD), &backend_pid);
    using Process = std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&CloseHandle)>;
    Process process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, backend_pid), &CloseHandle);
    require(process != nullptr, "Cannot observe backend memory");
    require(QDir().mkpath(root + "/materials"), "Cannot create preview root");
    write_texture(native_path(root + "/materials/large.vtf"), 512);
    require_result(view->mount_directory(native_path(root), true));
    const std::string material = "UnlitGeneric { \"$basetexture\" \"large\" }\n";
    require_result(ui::write_preview_material(native_path(root), material));
    require_result(view->set_material(ui::kPreviewMaterialName));
    ui::PreviewSync preview;
    preview.configure([&](const std::string& bytes) {
        return ui::write_preview_material(native_path(root), bytes).ok();
    }, [&]() { return view->reload_material().ok(); });
    preview.set_active(true);
    measure("engine_preview_write_reload_ack", [&]() { require_result(preview.flush_now(material, true)); });

    bridge::AssetList assets;
    measure("engine_vfs_catalogue", [&]() {
        auto result = background([&]() { return view->list_assets({"materials", "models"}, {".vmt", ".vtf", ".mdl"}); });
        require_result(result); assets = std::move(result.value());
        require(!assets.entries.empty(), "Engine returned an empty catalogue");
    }, true);
    details.insert("engine_assets", static_cast<qint64>(assets.entries.size()));
    details.insert("engine_catalogue_truncated", assets.truncated);
    auto access = std::make_shared<ui::qt::EngineAccess>();
    access->setEngine(view.get());
    ui::qt::GameTextureSource source(access, {});
    std::string packed_path;
    std::uint64_t packed_bytes = 0;
    for (const auto& asset : assets.entries) {
        if (asset.loose || !QString::fromStdString(asset.path).endsWith(".vtf", Qt::CaseInsensitive)) continue;
        auto info = background([&]() { return source.metadata(asset.path); });
        if (info && info.value().info && info.value().info.value().preview_supported &&
            info.value().file_size <= 16 * 1024 * 1024) {
            packed_path = asset.path; packed_bytes = info.value().file_size; break;
        }
    }
    require(!packed_path.empty(), "No previewable VPK texture found");
    details.insert("vpk_texture", QString::fromStdString(packed_path));
    details.insert("vpk_texture_bytes", static_cast<qint64>(packed_bytes));
    DWORD handles_before = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &handles_before) != FALSE, "Cannot count handles");
    measure("engine_vpk_vtf_metadata", [&]() {
        const auto result = background([&]() { return source.metadata(packed_path); });
        require_result(result); require_result(result.value().info);
        require(result.value().file_size == packed_bytes, "VPK metadata changed size");
    });
    DWORD handles_after = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &handles_after) != FALSE, "Cannot recount handles");
    details.insert("metadata_handle_growth", static_cast<int>(handles_after) - static_cast<int>(handles_before));
    require(handles_after <= handles_before + 4, "Repeated response watches leaked native handles");
    measure("engine_vpk_thumbnail_decode_128", [&]() {
        const auto result = background([&]() {
            const auto bytes = source.read(packed_path);
            require_result(bytes);
            require(bytes.value().size() == packed_bytes, "VPK RAW length differs from prefix metadata");
            return vtf::decode_vtf_preview_from_memory(bytes.value().data(), bytes.value().size(), 0, 128);
        });
        require_result(result);
    });
    details.insert("backend_memory_before_cancel", memory(process.get()));

    const auto response_dir = native_path(QDir::tempPath() + "/VMTStudio/ipc");
    const auto missing = background([&]() {
        return view->read_text_file("materials/performance_missing_" + std::to_string(GetCurrentProcessId()) + ".vmt");
    });
    require(!missing, "Missing file unexpectedly resolved");
    const std::string response_prefix = "ui_" + std::to_string(GetCurrentProcessId()) + "_";
    for (const auto& file : std::filesystem::directory_iterator(response_dir)) {
        require(file.path().filename().string().rfind(response_prefix, 0) != 0, "Failed query left response files open");
    }
    details.insert("backend_error_cleanup_verified", true);
    const std::string pre_cancelled = "probe_" + std::to_string(GetCurrentProcessId());
    write_file(response_dir / (pre_cancelled + ".cancel"), {});
    const std::string command = "DUMP exists " + pre_cancelled + " " + packed_path;
    COPYDATASTRUCT request{};
    request.cbData = static_cast<DWORD>(command.size());
    request.lpData = const_cast<char*>(command.data());
    DWORD_PTR accepted = 0;
    require(SendMessageTimeoutA(GetWindow(host.get(), GW_CHILD), WM_COPYDATA,
        reinterpret_cast<WPARAM>(host.get()), reinterpret_cast<LPARAM>(&request),
        SMTO_ABORTIFHUNG, 1000, &accepted) != 0 && accepted == 1, "Backend rejected cancellation probe");
    wait_until([&]() {
        std::ifstream error(response_dir / (pre_cancelled + ".err"));
        std::string text; std::getline(error, text);
        return text.find("cancelled") != std::string::npos;
    });
    require(!std::filesystem::exists(response_dir / (pre_cancelled + ".dat")),
            "Pre-cancelled request published a payload");
    for (const char* extension : {".cancel", ".err"}) std::filesystem::remove(response_dir / (pre_cancelled + extension));
    details.insert("queued_cancellation_verified", true);

    auto pending = std::async(std::launch::async, [&]() {
        return view->list_assets({"materials", "models"}, {".vmt", ".vtf", ".mdl"});
    });
    const auto started = Clock::now();
    while (Clock::now() - started < std::chrono::milliseconds(50)) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready,
            "Catalogue finished before cancellation could be exercised");
    measure("engine_cancel_active_query", [&]() {
        view->cancel_pending_queries();
        wait_until([&]() { return pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; });
        const auto cancelled = pending.get();
        require(!cancelled && cancelled.error().message.find("cancelled") != std::string::npos,
                "Active query was not cancelled");
    }, true);
    measure("engine_backend_cancel_ack", [&]() {
        const std::string prefix = "ui_" + std::to_string(GetCurrentProcessId()) + "_";
        wait_until([&]() {
            for (const auto& file : std::filesystem::directory_iterator(response_dir)) {
                if (file.path().filename().string().rfind(prefix, 0) != 0 || file.path().extension() != ".err") continue;
                std::ifstream error(file.path()); std::string text; std::getline(error, text);
                if (text.find("cancelled") != std::string::npos) return true;
            }
            return false;
        });
    }, true);
    details.insert("active_backend_cancellation_verified", true);
    const auto continued = background([&]() { return view->file_exists(packed_path); });
    require_result(continued); require(continued.value(), "Backend cannot query after cancellation");

    // Exercise teardown during a second active scan, before waiting for its
    // backend acknowledgement, matching MainWindow's close/reconnect ordering.
    pending = std::async(std::launch::async, [&]() {
        return view->list_assets({"materials", "models"}, {".vmt", ".vtf", ".mdl"});
    });
    const auto second_started = Clock::now();
    while (Clock::now() - second_started < std::chrono::milliseconds(50)) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready, "Second scan was not active");
    view->cancel_pending_queries();
    wait_until([&]() { return pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; });
    require(!pending.get(), "Second scan was not cancelled");
    access->setEngine(nullptr);
    measure("engine_shutdown_after_cancel", [&]() { view->shutdown(); }, true);
    require(!view->initialized(), "Engine remained connected after shutdown");
    const std::string own_prefix = "ui_" + std::to_string(GetCurrentProcessId()) + "_";
    for (const auto& file : std::filesystem::directory_iterator(response_dir)) {
        require(file.path().filename().string().rfind(own_prefix, 0) != 0, "Abandoned response survived engine shutdown");
    }
    auto replacement = bridge::EngineView::create();
    require_result(replacement);
    view = std::move(replacement.value());
    measure("engine_reconnect_after_cancel", [&]() {
        require_result(background([&]() { return view->initialize(config); }));
        const auto exists = background([&]() { return view->file_exists(packed_path); });
        require_result(exists); require(exists.value(), "Fresh connection cannot see the VPK texture");
    }, true);
    view->shutdown();

    // Cancel an initialization only after an actual child process is live,
    // rather than proving just the early "already cancelled" guard.
    auto startup_view = bridge::EngineView::create();
    require_result(startup_view);
    config.cancelled = std::make_shared<std::atomic<bool>>(false);
    auto starting = std::async(std::launch::async, [&]() { return startup_view.value()->initialize(config); });
    Process starting_process(nullptr, &CloseHandle);
    wait_until([&]() {
        HANDLE raw = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        require(raw != INVALID_HANDLE_VALUE, "Cannot observe startup child");
        Process snapshot(raw, &CloseHandle);
        PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
        for (BOOL found = Process32FirstW(snapshot.get(), &entry); found; found = Process32NextW(snapshot.get(), &entry)) {
            if (entry.th32ParentProcessID != GetCurrentProcessId() || _wcsicmp(entry.szExeFile, L"hlmv_backend.exe") != 0) continue;
            Process candidate(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID), &CloseHandle);
            DWORD exit_code = 0;
            if (candidate && GetExitCodeProcess(candidate.get(), &exit_code) && exit_code == STILL_ACTIVE) {
                starting_process = std::move(candidate); return true;
            }
        }
        return false;
    });
    require(starting.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready, "Startup already completed before cancellation");
    measure("engine_cancel_live_startup", [&]() {
        config.cancelled->store(true);
        wait_until([&]() { return starting.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; });
        const auto result = starting.get();
        require(!result && result.error().message.find("cancelled") != std::string::npos, "Live startup did not cancel");
        require(WaitForSingleObject(starting_process.get(), 0) == WAIT_OBJECT_0, "Cancelled startup left its child alive");
    }, true);
    details.insert("cancelled_live_startup_verified", true);
}

} // namespace

int main(int argc, char** argv) {
    const bool native_engine = argc > 1 && std::string_view(argv[1]) == "engine";
    qputenv("QT_QPA_PLATFORM", native_engine ? QByteArrayLiteral("windows") : QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QThreadPool::globalInstance()->setMaxThreadCount(4);
    const QStringList args = QCoreApplication::arguments();
    try {
        require(args.size() >= 3, "Usage: performance_benchmarks browser|editor|texture <workload> [samples], or engine <bin-dir> <game-dir> [samples]");
        const QString scenario = args[1];
        bool valid = false;
        const bool engine_scenario = scenario == "engine";
        const int count = engine_scenario ? 0 : args[2].toInt(&valid);
        require(engine_scenario || (valid && count > 0), "Workload must be positive");
        valid = true;
        const int samples_argument = engine_scenario ? 4 : 3;
        if (args.size() > samples_argument) sample_count = args[samples_argument].toInt(&valid);
        require(valid && sample_count >= 3 && sample_count <= 100, "Samples must be 3..100");
        QTemporaryDir directory(QDir::tempPath() + "/vmtstudio-performance-XXXXXX");
        require(directory.isValid(), "Cannot create isolated fixture directory");
        const auto baseline = memory();
        if (scenario == "browser") {
            require(count <= static_cast<int>(ui::AssetIndex::kMaxEntries), "Asset count exceeds product limit");
            browser(count, directory.path());
            browser_widgets(count, directory.path());
        } else if (scenario == "editor") {
            require(count <= 8192, "Parameter count must be <=8192");
            editor(count, directory.path());
        } else if (scenario == "texture") {
            require(count >= 128 && count <= 4096 && (count & (count - 1)) == 0, "Texture edge must be a power of two in 128..4096");
            texture(static_cast<unsigned>(count), directory.path());
        } else if (engine_scenario) {
            require(args.size() >= 4, "Engine scenario requires bin and game directories");
            engine(args[2], args[3], directory.path());
        } else {
            throw std::runtime_error("Unknown scenario");
        }
        QJsonObject report{{"scenario", scenario}, {"workload", count}, {"qt", QT_VERSION_STR},
            {"architecture", QSysInfo::currentCpuArchitecture()}, {"logical_cpus", QThread::idealThreadCount()},
            {"kernel", QSysInfo::kernelVersion()}, {"warmup_per_metric", 1}, {"baseline_memory", baseline},
            {"largest_ui_event_gap_ms", largest_ui_gap_ms}, {"measurements", measurements}, {"details", details}};
#ifdef NDEBUG
        report.insert("configuration", "Release");
#else
        report.insert("configuration", "Debug");
#endif
        std::cout << QJsonDocument(report).toJson(QJsonDocument::Indented).constData();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
