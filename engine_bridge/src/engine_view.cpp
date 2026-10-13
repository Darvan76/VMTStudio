#include "bridge/engine_view.h"

#include <fstream>
#if !defined(_WIN64)
#include "engine_spike_engine.h"
#endif

#include <windows.h>
#include <chrono>
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

        // 3. Build command line
        std::wstring cmd = L"\"" + backend_exe.wstring() + L"\"";
        if (config.child_window != nullptr) {
            cmd += L" -embed " + std::to_wstring(reinterpret_cast<uintptr_t>(config.child_window));
        }
        cmd += L" -game \"" + config.game_dir.wstring() + L"\"";
        cmd += L" -bin \"" + config.bin_dir.wstring() + L"\"";

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

    vmt::Result<void> render() override {
        if (child_hwnd_ != nullptr && IsWindow(child_hwnd_)) {
            InvalidateRect(child_hwnd_, nullptr, FALSE);
        }
        return vmt::Ok();
    }

    vmt::Result<void> resize(int width, int height) override {
        if (child_hwnd_ != nullptr && IsWindow(child_hwnd_)) {
            SetWindowPos(child_hwnd_, HWND_TOP, 0, 0, width, height,
                         SWP_NOZORDER | SWP_NOACTIVATE);
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

    bool send_command(const std::string& cmd) {
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
                                SMTO_ABORTIFHUNG, 1000, &result) != 0) {
            return result != 0;
        }
        return false;
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
};

} // namespace

vmt::Result<std::unique_ptr<EngineView>> EngineView::create() {
    return vmt::Ok(std::unique_ptr<EngineView>(new HlmvEngineView()));
}

} // namespace bridge
