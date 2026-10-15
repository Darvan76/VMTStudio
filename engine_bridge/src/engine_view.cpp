#include "bridge/engine_view.h"
#include "bridge/ipc_dump.h"

#include <fstream>
#if !defined(_WIN64)
#include "engine_spike_engine.h"
#endif

#include <windows.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace bridge {
namespace {

vmt::Error engine_error(vmt::ErrorCode code, std::string message) {
    return vmt::make_error(std::move(code), std::move(message),
                           "Check the game folders in Game > Connection and try again");
}

// WM_COPYDATA round trip of the plain commands; DUMP/RAW jobs do real work in
// the backend and get the longer deadline below (docs/IPC_PROTOCOL.md).
constexpr unsigned kCommandTimeoutMs = 1000;
constexpr unsigned kDumpTimeoutMs = 10000;

// Response-file directory of the model viewer queries: %TEMP%\VMTStudio\ipc.
// initialize() creates it and passes it to the backend with -ipc-dir.
std::filesystem::path dump_dir() {
    wchar_t temp_path[MAX_PATH] = {};
    const DWORD length = GetTempPathW(MAX_PATH, temp_path);
    const std::filesystem::path base =
        (length > 0 && length < MAX_PATH) ? std::filesystem::path(temp_path)
                                          : std::filesystem::path(L".");
    return base / L"VMTStudio" / L"ipc";
}

// Removes every response file the backend may leave for `token`.
void remove_dump_files(const std::string& token) {
    constexpr const char* const kExtensions[] = { "dat", "bin", "done", "err" };
    const std::filesystem::path dir = dump_dir();
    for (const char* extension : kExtensions) {
        std::error_code ec;
        std::filesystem::remove(dir / (token + "." + extension), ec);
    }
}

std::filesystem::path find_hlmv_backend() {
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) != 0) {
        std::filesystem::path dir = std::filesystem::path(exe).parent_path();
        std::filesystem::path target = dir / L"hlmv_backend.exe";
        if (std::filesystem::exists(target)) return target;

        // Try sibling build directory for tests
        target = dir / L".." / L"bin" / L"Release" / L"hlmv_backend.exe";
        if (std::filesystem::exists(target)) return target;
    }
    return L"hlmv_backend.exe";
}

class HlmvEngineView final : public EngineView {
public:
    HlmvEngineView() = default;
    ~HlmvEngineView() override { shutdown(); }

    vmt::Result<void> initialize(const EngineViewConfig& config) override {
        if (initialized_) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::InvalidArgument,
                                               "the engine is already initialized"));
        }
        if (config.bin_dir.empty() || config.game_dir.empty()) {
            return vmt::Err<void>(engine_error(
                vmt::ErrorCode::InvalidArgument,
                "the game /bin folder or the game folder (gameinfo.txt) is not set"));
        }

        config_ = config;

#if !defined(_WIN64)
        // 1. Initialize in-process filesystem (for instant file reading, material list, etc.)
        if (!diagnostics_installed_) {
            spike::install_diagnostics();
            diagnostics_installed_ = true;
        }
        spike::apply_steam_appid(config.steam_appid);

        spike::EngineOptions fs_options;
        fs_options.bin_dir = config.bin_dir.string();
        fs_options.game_dir = config.game_dir.string();
        fs_options.child_hwnd = nullptr;
        fs_options.pump_messages = false;
        fs_options.mount_mode = spike::MountMode::All;

        (void)spike::init_filesystem_only(fs_options);
#endif

        // 2. Locate hlmv_backend.exe
        const std::filesystem::path backend_exe = find_hlmv_backend();
        if (!std::filesystem::exists(backend_exe)) {
            return vmt::Err<void>(engine_error(
                vmt::ErrorCode::EngineInitFailed,
                "hlmv_backend.exe not found at " + backend_exe.string()));
        }

        // 3. Build command line. The model viewer queries exchange response
        // files in %TEMP%\VMTStudio\ipc; the backend also creates the folder
        // when it is missing, so a failure here is not fatal.
        const std::filesystem::path ipc_dir = dump_dir();
        std::error_code ipc_error;
        std::filesystem::create_directories(ipc_dir, ipc_error);

        std::wstring cmd = L"\"" + backend_exe.wstring() + L"\"";
        if (config.child_window != nullptr) {
            cmd += L" -embed " + std::to_wstring(reinterpret_cast<uintptr_t>(config.child_window));
        }
        cmd += L" -game \"" + config.game_dir.wstring() + L"\"";
        cmd += L" -bin \"" + config.bin_dir.wstring() + L"\"";
        cmd += L" -ipc-dir \"" + ipc_dir.wstring() + L"\"";

        // 4. Temporarily update PATH so child process loader finds tier0.dll, etc.
        std::wstring old_path;
        DWORD path_len = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        if (path_len > 0) {
            old_path.resize(path_len);
            GetEnvironmentVariableW(L"PATH", old_path.data(), path_len);
            while (!old_path.empty() && old_path.back() == L'\0') {
                old_path.pop_back();
            }
        }
        std::wstring new_path = config.bin_dir.wstring() + L";" + old_path;
        SetEnvironmentVariableW(L"PATH", new_path.c_str());

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        BOOL created = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                      0, nullptr, nullptr, &si, &pi);

        SetEnvironmentVariableW(L"PATH", old_path.c_str());

        if (!created) {
            DWORD err = GetLastError();
            return vmt::Err<void>(engine_error(
                vmt::ErrorCode::EngineInitFailed,
                "failed to launch hlmv_backend.exe (code " + std::to_string(err) + ")"));
        }

        process_info_ = pi;
        host_hwnd_ = static_cast<HWND>(config.child_window);

        // 5. Wait for backend to initialize and embed
        HWND child = nullptr;
        const auto start_time = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start_time).count() < 6000) {
            DWORD exit_code = 0;
            if (GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code != STILL_ACTIVE) {
                return vmt::Err<void>(engine_error(
                    vmt::ErrorCode::EngineInitFailed,
                    "hlmv_backend exited prematurely with code " + std::to_string(exit_code)));
            }

            if (host_hwnd_ != nullptr) {
                child = GetWindow(host_hwnd_, GW_CHILD);
                if (child != nullptr && IsWindow(child)) {
                    DWORD_PTR ping_result = 0;
                    if (SendMessageTimeoutW(child, WM_USER + 420, 0, 0,
                                            SMTO_ABORTIFHUNG, 200, &ping_result) != 0 &&
                        ping_result == 1) {
                        break;
                    }
                }
            }
            Sleep(50);
        }

        if (host_hwnd_ != nullptr && (child == nullptr || !IsWindow(child))) {
            terminate_backend();
            return vmt::Err<void>(engine_error(
                vmt::ErrorCode::EngineInitFailed,
                "timed out waiting for hlmv_backend to embed into the host window"));
        }

        child_hwnd_ = child;
        initialized_ = true;
        (void)resize(0, 0);
        return vmt::Ok();
    }

    vmt::Result<void> mount_directory(const std::filesystem::path& dir,
                                      bool search_first) override {
#if !defined(_WIN64)
        (void)spike::mount_custom_path("GAME", dir.string(), search_first);
#endif
        send_command("MOUNT_DIR " + dir.string());
        return vmt::Ok();
    }

    vmt::Result<void> set_material(const std::string& material_name) override {
        material_name_ = material_name;
        if (!send_command("SET_MATERIAL " + material_name)) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::RenderFailed,
                                               "failed to send SET_MATERIAL to hlmv_backend"));
        }
        return vmt::Ok();
    }

    vmt::Result<void> reload_material() override {
        if (material_name_.empty()) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::InvalidArgument,
                                               "no preview material is set"));
        }
        if (!send_command("RELOAD_MATERIAL " + material_name_)) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::RenderFailed,
                                               "failed to send RELOAD_MATERIAL to hlmv_backend"));
        }
        return vmt::Ok();
    }

    vmt::Result<bool> file_exists(const std::string& game_path) override {
#if !defined(_WIN64)
        const spike::ExistsResult result = spike::file_exists(game_path);
        if (result.ok) {
            return vmt::Ok(result.exists);
        }
#endif
        if (!config_.game_dir.empty()) {
            std::filesystem::path full = config_.game_dir / std::filesystem::u8path(game_path);
            if (std::filesystem::exists(full)) {
                return vmt::Ok(true);
            }
        }
        return vmt::Ok(false);
    }

    vmt::Result<std::vector<std::string>> list_materials() override {
#if !defined(_WIN64)
        const spike::MaterialList result = spike::list_materials();
        if (result.ok) {
            return vmt::Ok(result.names);
        }
#endif
        std::vector<std::string> names;
        if (!config_.game_dir.empty()) {
            std::filesystem::path mat_dir = config_.game_dir / "materials";
            if (std::filesystem::exists(mat_dir)) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(mat_dir)) {
                    if (entry.is_regular_file() && entry.path().extension() == ".vmt") {
                        auto rel = std::filesystem::relative(entry.path(), mat_dir);
                        std::string name = rel.generic_string();
                        if (name.size() > 4 && name.substr(name.size() - 4) == ".vmt") {
                            name = name.substr(0, name.size() - 4);
                        }
                        names.push_back(std::move(name));
                    }
                }
            }
        }
        return vmt::Ok(names);
    }

    vmt::Result<std::vector<unsigned char>> read_binary_file(
        const std::string& game_path) override {
#if !defined(_WIN64)
        const spike::BinaryFile result = spike::read_binary_file(game_path);
        if (result.ok && result.found) {
            return vmt::Ok(result.bytes);
        }
#endif
        if (!config_.game_dir.empty()) {
            std::filesystem::path full = config_.game_dir / std::filesystem::u8path(game_path);
            if (std::filesystem::exists(full)) {
                std::ifstream f(full, std::ios::binary);
                if (f) {
                    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)),
                                                     std::istreambuf_iterator<char>());
                    return vmt::Ok(bytes);
                }
            }
        }
        return vmt::Err<std::vector<unsigned char>>(
            engine_error(vmt::ErrorCode::NotFound, "file not found: " + game_path));
    }

    vmt::Result<std::string> read_text_file(const std::string& game_path) override {
#if !defined(_WIN64)
        const spike::TextFile result = spike::read_text_file(game_path);
        if (result.ok && result.found) {
            return vmt::Ok(result.text);
        }
#endif
        if (!config_.game_dir.empty()) {
            std::filesystem::path full = config_.game_dir / std::filesystem::u8path(game_path);
            if (std::filesystem::exists(full)) {
                std::ifstream f(full, std::ios::binary);
                if (f) {
                    std::string text((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
                    return vmt::Ok(text);
                }
            }
        }
        return vmt::Err<std::string>(
            engine_error(vmt::ErrorCode::NotFound, "file not found: " + game_path));
    }

    vmt::Result<void> set_preview_shape(PreviewShape shape) override {
        std::string shape_str;
        switch (shape) {
        case PreviewShape::Sphere: shape_str = "sphere"; break;
        case PreviewShape::Cube: shape_str = "cube"; break;
        case PreviewShape::Cylinder: shape_str = "cylinder"; break;
        case PreviewShape::Plane: shape_str = "plane"; break;
        case PreviewShape::BrushWall: shape_str = "brush_wall"; break;
        case PreviewShape::BrushFloor: shape_str = "brush_floor"; break;
        case PreviewShape::BrushCube: shape_str = "brush_cube"; break;
        case PreviewShape::BrushDisplacement: shape_str = "brush_displacement"; break;
        }
        if (!send_command("SET_SHAPE " + shape_str)) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::RenderFailed,
                                               "failed to send SET_SHAPE to hlmv_backend"));
        }
        return vmt::Ok();
    }

    vmt::Result<void> set_lighting(const PreviewLighting& lighting) override {
        std::ostringstream ss;
        ss << "SET_LIGHTING "
           << lighting.ambient[0] << " " << lighting.ambient[1] << " " << lighting.ambient[2] << " "
           << lighting.key_dir[0] << " " << lighting.key_dir[1] << " " << lighting.key_dir[2] << " "
           << lighting.key_color[0] << " " << lighting.key_color[1] << " " << lighting.key_color[2] << " "
           << lighting.fill_dir[0] << " " << lighting.fill_dir[1] << " " << lighting.fill_dir[2] << " "
           << lighting.fill_color[0] << " " << lighting.fill_color[1] << " " << lighting.fill_color[2] << " "
           << lighting.background[0] << " " << lighting.background[1] << " " << lighting.background[2];
        if (!send_command(ss.str())) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::RenderFailed,
                                               "failed to send SET_LIGHTING to hlmv_backend"));
        }
        return vmt::Ok();
    }

    vmt::Result<void> set_model(const std::filesystem::path& model_path) override {
        if (!send_command("LOAD_MODEL " + model_path.string())) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::RenderFailed,
                                               "failed to send LOAD_MODEL to hlmv_backend"));
        }
        return vmt::Ok();
    }

    vmt::Result<Thumbnail> render_material_thumbnail(const std::string& material_name, int size,
                                                     bool flip_vertical) override {
        if (!initialized_ || !is_backend_alive()) {
            return vmt::Err<Thumbnail>(engine_error(
                vmt::ErrorCode::RenderFailed, "hlmv_backend is not initialized or running"));
        }
        if (size <= 0) {
            return vmt::Err<Thumbnail>(engine_error(
                vmt::ErrorCode::InvalidArgument, "invalid thumbnail size"));
        }

        wchar_t temp_dir[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp_dir);
        wchar_t temp_file[MAX_PATH] = {};
        GetTempFileNameW(temp_dir, L"vmt_thmb_", 0, temp_file);
        std::filesystem::path thumb_path = temp_file;
        thumb_path.replace_extension(L".tga");

        std::string cmd = "CAPTURE_THUMBNAIL \"" + material_name + "\" " +
                          std::to_string(size) + " \"" + thumb_path.string() + "\"";
        if (!send_command(cmd)) {
            std::error_code ec;
            std::filesystem::remove(thumb_path, ec);
            std::filesystem::remove(temp_file, ec);
            return vmt::Err<Thumbnail>(engine_error(
                vmt::ErrorCode::RenderFailed, "failed to send CAPTURE_THUMBNAIL to hlmv_backend"));
        }

        FILE* f = _wfopen(thumb_path.c_str(), L"rb");
        if (!f) {
            std::error_code ec;
            std::filesystem::remove(thumb_path, ec);
            std::filesystem::remove(temp_file, ec);
            return vmt::Err<Thumbnail>(engine_error(
                vmt::ErrorCode::RenderFailed, "failed to open captured thumbnail file"));
        }

        unsigned char header[18] = {};
        if (fread(header, 1, 18, f) != 18) {
            fclose(f);
            std::error_code ec;
            std::filesystem::remove(thumb_path, ec);
            std::filesystem::remove(temp_file, ec);
            return vmt::Err<Thumbnail>(engine_error(
                vmt::ErrorCode::RenderFailed, "malformed thumbnail header"));
        }

        int w = header[12] | (header[13] << 8);
        int h = header[14] | (header[15] << 8);
        if (w <= 0 || h <= 0) {
            fclose(f);
            std::error_code ec;
            std::filesystem::remove(thumb_path, ec);
            std::filesystem::remove(temp_file, ec);
            return vmt::Err<Thumbnail>(engine_error(
                vmt::ErrorCode::RenderFailed, "invalid thumbnail dimensions"));
        }

        Thumbnail result;
        result.width = w;
        result.height = h;
        size_t total_bytes = (size_t)w * h * 4;
        result.bgra.resize(total_bytes);
        if (fread(result.bgra.data(), 1, total_bytes, f) != total_bytes) {
            fclose(f);
            std::error_code ec;
            std::filesystem::remove(thumb_path, ec);
            std::filesystem::remove(temp_file, ec);
            return vmt::Err<Thumbnail>(engine_error(
                vmt::ErrorCode::RenderFailed, "incomplete thumbnail pixel data"));
        }
        fclose(f);

        std::error_code ec;
        std::filesystem::remove(thumb_path, ec);
        std::filesystem::remove(temp_file, ec);

        if (flip_vertical) {
            std::vector<unsigned char> flipped(result.bgra.size());
            const size_t row_bytes = (size_t)w * 4;
            for (int y = 0; y < h; ++y) {
                std::memcpy(flipped.data() + (size_t)y * row_bytes,
                            result.bgra.data() + (size_t)(h - 1 - y) * row_bytes, row_bytes);
            }
            result.bgra.swap(flipped);
        }

        return vmt::Ok(std::move(result));
    }

    vmt::Result<void> set_render_mode(const std::string& mode) override {
        if (!send_command("SET_RENDER_MODE " + mode)) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::RenderFailed,
                                               "failed to send SET_RENDER_MODE to hlmv_backend"));
        }
        return vmt::Ok();
    }

    vmt::Result<void> set_animation(const std::string& state) override {
        if (!send_command("SET_ANIMATION " + state)) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::RenderFailed,
                                               "failed to send SET_ANIMATION to hlmv_backend"));
        }
        return vmt::Ok();
    }

    vmt::Result<void> set_speed(float speed) override {
        if (!send_command("SET_SPEED " + std::to_string(speed))) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::RenderFailed,
                                               "failed to send SET_SPEED to hlmv_backend"));
        }
        return vmt::Ok();
    }

    // --- Model viewer: queries ------------------------------------------------
    // The response-file plumbing of docs/IPC_PROTOCOL.md. Each query blocks the
    // calling thread while it polls for the backend's answer (up to 10 s), so
    // run them on a worker thread. Concurrent calls are safe: every job
    // generates its own token under a mutex.

    vmt::Result<ModelList> list_models(const std::string& prefix) override {
        if (!backend_running()) {
            return list_models_from_disk(prefix);
        }
        const vmt::Result<std::string> dump = request_dump("models", prefix);
        if (!dump.ok()) {
            return vmt::Err<ModelList>(dump.error());
        }
        return parse_models_dump(dump.value());
    }

    vmt::Result<ModelScene> query_model_scene() override {
        const vmt::Result<std::string> dump = request_dump("model_scene", std::string());
        if (!dump.ok()) {
            return vmt::Err<ModelScene>(dump.error());
        }
        return parse_model_scene_dump(dump.value());
    }

    vmt::Result<AnimState> query_anim_state() override {
        const vmt::Result<std::string> dump = request_dump("anim_state", std::string());
        if (!dump.ok()) {
            return vmt::Err<AnimState>(dump.error());
        }
        return parse_anim_state_dump(dump.value());
    }

    vmt::Result<std::vector<unsigned char>> read_game_binary(
        const std::string& game_path) override {
        if (!backend_running()) {
            return read_binary_file(game_path);
        }
        return request_raw(game_path);
    }

    // --- Model viewer: animation control --------------------------------------

    vmt::Result<void> set_sequence(int index) override {
        return send_simple("SET_SEQUENCE " + std::to_string(index));
    }

    vmt::Result<void> set_sequence_label(const std::string& label) override {
        return send_simple("SET_SEQUENCE_LABEL " + label);
    }

    vmt::Result<void> set_frame(int frame) override {
        return send_simple("SET_FRAME " + std::to_string(frame));
    }

    vmt::Result<void> step_frame(int delta) override {
        return send_simple("STEP_FRAME " + std::to_string(delta));
    }

    vmt::Result<void> set_loop(bool enabled) override {
        return send_simple(std::string("SET_LOOP ") + (enabled ? "1" : "0"));
    }

    vmt::Result<void> stop_animation() override {
        return send_simple("SET_STOP");
    }

    vmt::Result<void> set_blend_time(float seconds) override {
        return send_simple("SET_BLEND_TIME " + std::to_string(seconds));
    }

    vmt::Result<void> blend_now() override {
        return send_simple("BLEND_NOW");
    }

    vmt::Result<void> set_overlay(int layer, int sequence, float weight) override {
        return send_simple("SET_OVERLAY " + std::to_string(layer) + " " +
                           std::to_string(sequence) + " " + std::to_string(weight));
    }

    vmt::Result<void> set_pose_parameter(int index, float value) override {
        return send_simple("SET_POSEPARAM " + std::to_string(index) + " " +
                           std::to_string(value));
    }

    vmt::Result<void> set_bodygroup(int group, int value) override {
        return send_simple("SET_BODYGROUP " + std::to_string(group) + " " +
                           std::to_string(value));
    }

    vmt::Result<void> set_skin(int skin) override {
        return send_simple("SET_SKIN " + std::to_string(skin));
    }

    vmt::Result<void> set_view_flag(const std::string& flag, bool enabled) override {
        return send_simple("SET_VIEWFLAG " + flag + (enabled ? " 1" : " 0"));
    }

    vmt::Result<void> reload_material_by_name(const std::string& material_name) override {
        return send_simple("RELOAD_MATERIAL " + material_name);
    }

    vmt::Result<void> render() override {
        if (child_hwnd_ != nullptr && IsWindow(child_hwnd_)) {
            InvalidateRect(child_hwnd_, nullptr, FALSE);
        }
        return vmt::Ok();
    }

    vmt::Result<void> resize(int width, int height) override {
        if (host_hwnd_ != nullptr && IsWindow(host_hwnd_)) {
            RECT rc{};
            if (GetClientRect(host_hwnd_, &rc)) {
                int client_w = rc.right - rc.left;
                int client_h = rc.bottom - rc.top;
                if (client_w > 0 && client_h > 0) {
                    width = client_w;
                    height = client_h;
                }
            }
        }

        if (child_hwnd_ != nullptr && IsWindow(child_hwnd_)) {
            SetWindowPos(child_hwnd_, HWND_TOP, 0, 0, width, height,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        send_command("RESIZE " + std::to_string(width) + " " + std::to_string(height));
        return vmt::Ok();
    }

    void shutdown() override {
        if (!initialized_) return;
        terminate_backend();
#if !defined(_WIN64)
        (void)spike::shutdown_engine();
#endif
        initialized_ = false;
        material_name_.clear();
    }

    bool initialized() const noexcept override {
        return initialized_ && is_backend_alive();
    }

private:
    bool is_backend_alive() const {
        if (process_info_.hProcess == nullptr) return false;
        DWORD exit_code = 0;
        if (GetExitCodeProcess(process_info_.hProcess, &exit_code)) {
            return exit_code == STILL_ACTIVE;
        }
        return false;
    }

    // True when the backend process is up; the query methods fall back to the
    // local game folder when it is not.
    bool backend_running() const {
        return initialized_ && is_backend_alive();
    }

    bool send_command(const std::string& cmd) {
        return send_command_with_timeout(cmd, kCommandTimeoutMs);
    }

    // send_command() with an explicit WM_COPYDATA timeout; DUMP/RAW jobs run
    // longer than the 1 s round trip of the plain commands.
    bool send_command_with_timeout(const std::string& cmd, unsigned timeout_ms) {
        if (child_hwnd_ == nullptr || !IsWindow(child_hwnd_)) {
            return false;
        }
        COPYDATASTRUCT cds{};
        cds.dwData = 0;
        cds.cbData = static_cast<DWORD>(cmd.size());
        cds.lpData = const_cast<char*>(cmd.data());
        DWORD_PTR result = 0;
        if (SendMessageTimeoutA(child_hwnd_, WM_COPYDATA,
                                reinterpret_cast<WPARAM>(host_hwnd_),
                                reinterpret_cast<LPARAM>(&cds),
                                SMTO_ABORTIFHUNG, timeout_ms, &result) != 0) {
            return result != 0;
        }
        return false;
    }

    // send_command() with the standard error for a rejected command.
    vmt::Result<void> send_simple(const std::string& cmd) {
        if (!send_command(cmd)) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::RenderFailed,
                                               "hlmv_backend rejected: " + cmd));
        }
        return vmt::Ok();
    }

    // --- Response-file plumbing of the queries (docs/IPC_PROTOCOL.md) --------

    // Token of one query job: "ui_<pid>_<counter>_<tick>", matching the
    // [A-Za-z0-9_-]{1,64} rule. The counter is guarded so worker threads never
    // share a token.
    std::string generate_token() {
        std::lock_guard<std::mutex> lock(token_mutex_);
        ++token_counter_;
        return "ui_" + std::to_string(GetCurrentProcessId()) + "_" +
               std::to_string(token_counter_) + "_" + std::to_string(GetTickCount64());
    }

    // Sends a DUMP/RAW command and waits up to 10 s for `<token>.done` or
    // `<token>.err` (polled every 5 ms). On success the path of the
    // `<token>.<payload_extension>` file is returned; the response files are
    // removed on every failure path.
    vmt::Result<std::filesystem::path> run_dump_job(const std::string& command,
                                                    const std::string& token,
                                                    const char* payload_extension) {
        if (!backend_running()) {
            return vmt::Err<std::filesystem::path>(engine_error(
                vmt::ErrorCode::RenderFailed, "hlmv_backend is not initialized or running"));
        }

        const std::filesystem::path dir = dump_dir();
        if (!send_command_with_timeout(command, kDumpTimeoutMs)) {
            remove_dump_files(token);
            return vmt::Err<std::filesystem::path>(engine_error(
                vmt::ErrorCode::RenderFailed,
                "failed to send \"" + command + "\" to hlmv_backend"));
        }

        const std::filesystem::path done = dir / (token + ".done");
        const std::filesystem::path err = dir / (token + ".err");
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kDumpTimeoutMs);
        for (;;) {
            std::error_code ec;
            if (std::filesystem::exists(err, ec)) {
                std::ifstream file(err, std::ios::binary);
                std::string message;
                if (file) {
                    message.assign(std::istreambuf_iterator<char>(file),
                                   std::istreambuf_iterator<char>());
                }
                while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
                    message.pop_back();
                }
                remove_dump_files(token);
                if (message.empty()) {
                    message = "hlmv_backend rejected \"" + command + "\"";
                }
                return vmt::Err<std::filesystem::path>(
                    engine_error(vmt::ErrorCode::Internal, std::move(message)));
            }
            if (std::filesystem::exists(done, ec)) {
                return vmt::Ok(dir / (token + "." + payload_extension));
            }
            if (!is_backend_alive()) {
                remove_dump_files(token);
                return vmt::Err<std::filesystem::path>(engine_error(
                    vmt::ErrorCode::RenderFailed,
                    "hlmv_backend exited while \"" + command + "\" was in flight"));
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                remove_dump_files(token);
                return vmt::Err<std::filesystem::path>(engine_error(
                    vmt::ErrorCode::RenderFailed,
                    "timed out after " + std::to_string(kDumpTimeoutMs / 1000) +
                        " s waiting for hlmv_backend to answer \"" + command + "\""));
            }
            Sleep(5);
        }
    }

    // DUMP <kind> <token> [args] -> the UTF-8 payload of `<token>.dat`.
    vmt::Result<std::string> request_dump(const std::string& kind, const std::string& args) {
        const std::string token = generate_token();
        std::string command = "DUMP " + kind + " " + token;
        if (!args.empty()) {
            command += " " + args;
        }

        const vmt::Result<std::filesystem::path> payload = run_dump_job(command, token, "dat");
        if (!payload.ok()) {
            return vmt::Err<std::string>(payload.error());
        }

        std::ifstream file(payload.value(), std::ios::binary);
        std::string text;
        const bool read_ok = static_cast<bool>(file);
        if (file) {
            text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        remove_dump_files(token);
        if (!read_ok) {
            return vmt::Err<std::string>(engine_error(
                vmt::ErrorCode::IoError,
                "failed to read the hlmv_backend dump file " + payload.value().string()));
        }
        return vmt::Ok(std::move(text));
    }

    // RAW <token> <game path> -> the bytes of `<token>.bin`.
    vmt::Result<std::vector<unsigned char>> request_raw(const std::string& game_path) {
        const std::string token = generate_token();
        const std::string command = "RAW " + token + " " + game_path;

        const vmt::Result<std::filesystem::path> payload = run_dump_job(command, token, "bin");
        if (!payload.ok()) {
            return vmt::Err<std::vector<unsigned char>>(payload.error());
        }

        std::ifstream file(payload.value(), std::ios::binary);
        std::vector<unsigned char> bytes;
        const bool read_ok = static_cast<bool>(file);
        if (file) {
            bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        remove_dump_files(token);
        if (!read_ok) {
            return vmt::Err<std::vector<unsigned char>>(engine_error(
                vmt::ErrorCode::IoError,
                "failed to read the hlmv_backend binary file " + payload.value().string()));
        }
        return vmt::Ok(std::move(bytes));
    }

    // list_models() without a backend: walks the loose <game_dir>/models tree.
    // VPK content is only visible to the backend, so everything found here is
    // reported as loose.
    vmt::Result<ModelList> list_models_from_disk(const std::string& prefix) const {
        if (config_.game_dir.empty()) {
            return vmt::Err<ModelList>(engine_error(
                vmt::ErrorCode::EngineInitFailed,
                "hlmv_backend is not running and no game folder is set to scan for models"));
        }

        ModelList models;
        std::error_code dir_error;
        std::filesystem::recursive_directory_iterator it(config_.game_dir / L"models", dir_error);
        const std::filesystem::recursive_directory_iterator end;
        while (it != end) {
            const std::filesystem::directory_entry entry = *it;
            it.increment(dir_error);
            if (dir_error) break;

            std::error_code type_error;
            if (!entry.is_regular_file(type_error) || type_error) continue;
            if (entry.path().extension() != L".mdl") continue;

            std::error_code relative_error;
            const std::filesystem::path relative =
                std::filesystem::relative(entry.path(), config_.game_dir, relative_error);
            if (relative_error) continue;

            std::string name = relative.generic_string();
            if (!prefix.empty() && name.compare(0, prefix.size(), prefix) != 0) continue;

            ModelEntry model;
            model.name = std::move(name);
            model.loose = true;
            models.entries.push_back(std::move(model));
        }
        return vmt::Ok(std::move(models));
    }

    void terminate_backend() {
        if (child_hwnd_ != nullptr && IsWindow(child_hwnd_)) {
            send_command("QUIT");
        }
        if (process_info_.hProcess != nullptr) {
            if (WaitForSingleObject(process_info_.hProcess, 1000) != WAIT_OBJECT_0) {
                TerminateProcess(process_info_.hProcess, 0);
            }
            CloseHandle(process_info_.hProcess);
            process_info_.hProcess = nullptr;
        }
        if (process_info_.hThread != nullptr) {
            CloseHandle(process_info_.hThread);
            process_info_.hThread = nullptr;
        }
        child_hwnd_ = nullptr;
        host_hwnd_ = nullptr;
    }

    bool initialized_ = false;
    bool diagnostics_installed_ = false;
    std::string material_name_;
    EngineViewConfig config_;
    HWND host_hwnd_ = nullptr;
    HWND child_hwnd_ = nullptr;
    PROCESS_INFORMATION process_info_{};

    // Query job state, shared by the worker threads that call the four
    // blocking query methods.
    std::mutex token_mutex_;
    std::uint64_t token_counter_ = 0;
};

} // namespace

vmt::Result<std::unique_ptr<EngineView>> EngineView::create() {
    return vmt::Ok(std::unique_ptr<EngineView>(new HlmvEngineView()));
}

} // namespace bridge
