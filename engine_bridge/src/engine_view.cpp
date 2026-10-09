#include "bridge/engine_view.h"

#include "engine_spike_engine.h"

#include <utility>

namespace bridge {
namespace {

vmt::Error engine_error(vmt::ErrorCode code, std::string message) {
    return vmt::make_error(std::move(code), std::move(message),
                           "Check the game folders in Game > Connection and try again");
}

spike::PreviewShape to_spike_shape(PreviewShape shape) {
    switch (shape) {
    case PreviewShape::Sphere:
        return spike::PreviewShape::Sphere;
    case PreviewShape::Cube:
        return spike::PreviewShape::Cube;
    case PreviewShape::Cylinder:
        return spike::PreviewShape::Cylinder;
    case PreviewShape::Plane:
        break;
    }
    return spike::PreviewShape::Plane;
}

spike::Lighting to_spike_lighting(const PreviewLighting& lighting) {
    spike::Lighting out;
    for (int i = 0; i < 3; ++i) {
        out.ambient[i] = lighting.ambient[i];
        out.key_dir[i] = lighting.key_dir[i];
        out.key_color[i] = lighting.key_color[i];
        out.fill_dir[i] = lighting.fill_dir[i];
        out.fill_color[i] = lighting.fill_color[i];
        out.background[i] = lighting.background[i];
    }
    return out;
}

class SpikeEngineView final : public EngineView {
public:
    SpikeEngineView() = default;
    ~SpikeEngineView() override { shutdown(); }

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

        if (!diagnostics_installed_) {
            spike::install_diagnostics();
            diagnostics_installed_ = true;
        }
        spike::apply_steam_appid(config.steam_appid);

        spike::EngineOptions options;
        options.bin_dir = config.bin_dir.string();
        options.game_dir = config.game_dir.string();
        options.width = config.width;
        options.height = config.height;
        options.child_hwnd = config.child_window;
        options.pump_messages = false; // the UI host owns the message loop
        options.mount_mode = spike::MountMode::All;

        const spike::StepResult result = spike::init_engine(options);
        if (!result.ok) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::EngineInitFailed,
                                               "the engine could not be initialized: " +
                                                   result.message));
        }
        initialized_ = true;
        return vmt::Ok();
    }

    vmt::Result<void> mount_directory(const std::filesystem::path& dir,
                                      bool search_first) override {
        const spike::StepResult result =
            spike::mount_custom_path("GAME", dir.string(), search_first);
        if (!result.ok) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::MountFailed, result.message));
        }
        return vmt::Ok();
    }

    vmt::Result<void> set_material(const std::string& material_name) override {
        const spike::StepResult result = spike::set_material(material_name);
        if (!result.ok) {
            material_name_.clear();
            vmt::ErrorCode code = result.message.find("not found") != std::string::npos
                                      ? vmt::ErrorCode::NotFound
                                      : vmt::ErrorCode::RenderFailed;
            return vmt::Err<void>(engine_error(code, result.message));
        }
        material_name_ = material_name;
        return vmt::Ok();
    }

    vmt::Result<void> reload_material() override {
        if (material_name_.empty()) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::InvalidArgument,
                                               "no preview material is set"));
        }
        const spike::StepResult result = spike::reload_material(material_name_);
        if (!result.ok) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::RenderFailed, result.message));
        }
        return vmt::Ok();
    }

    vmt::Result<bool> file_exists(const std::string& game_path) override {
        const spike::ExistsResult result = spike::file_exists(game_path);
        if (!result.ok) {
            return vmt::Err<bool>(engine_error(vmt::ErrorCode::InvalidArgument,
                                               "the engine file system is not initialized"));
        }
        return vmt::Ok(result.exists);
    }

    vmt::Result<std::vector<std::string>> list_materials() override {
        const spike::MaterialList result = spike::list_materials();
        if (!result.ok) {
            return vmt::Err<std::vector<std::string>>(engine_error(
                vmt::ErrorCode::InvalidArgument, "the engine file system is not initialized"));
        }
        return vmt::Ok(result.names);
    }

    vmt::Result<std::string> read_text_file(const std::string& game_path) override {
        const spike::TextFile result = spike::read_text_file(game_path);
        if (!result.ok) {
            return vmt::Err<std::string>(engine_error(vmt::ErrorCode::InvalidArgument,
                                                      "the engine file system is not initialized"));
        }
        if (!result.found) {
            vmt::Error error = vmt::make_error(vmt::ErrorCode::NotFound,
                                               "file not found in the mounted game paths: " +
                                                   game_path,
                                               "Check the game connection and the file name");
            return vmt::Err<std::string>(std::move(error));
        }
        return vmt::Ok(result.text);
    }

    vmt::Result<void> set_preview_shape(PreviewShape shape) override {
        const spike::StepResult result = spike::set_preview_shape(to_spike_shape(shape));
        if (!result.ok) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::InvalidArgument, result.message));
        }
        return vmt::Ok();
    }

    vmt::Result<void> set_lighting(const PreviewLighting& lighting) override {
        const spike::StepResult result = spike::set_lighting(to_spike_lighting(lighting));
        if (!result.ok) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::InvalidArgument, result.message));
        }
        return vmt::Ok();
    }

    vmt::Result<void> set_model(const std::filesystem::path& model_path) override {
        const spike::StepResult result = spike::set_model(model_path.string());
        if (!result.ok) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::InvalidArgument, result.message));
        }
        return vmt::Ok();
    }

    vmt::Result<Thumbnail> render_material_thumbnail(const std::string& material_name, int size,
                                                     bool flip_vertical) override {
        const spike::ThumbnailResult result =
            spike::render_material_thumbnail(material_name, size, flip_vertical);
        if (!result.ok) {
            return vmt::Err<Thumbnail>(
                engine_error(vmt::ErrorCode::RenderFailed, result.message));
        }
        Thumbnail thumbnail;
        thumbnail.width = result.width;
        thumbnail.height = result.height;
        thumbnail.bgra = result.bgra;
        return vmt::Ok(std::move(thumbnail));
    }

    vmt::Result<void> render() override {
        const spike::StepResult result = spike::render_frame(frame_++);
        if (!result.ok) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::RenderFailed, result.message));
        }
        return vmt::Ok();
    }

    vmt::Result<void> resize(int width, int height) override {
        const spike::StepResult result = spike::resize_view(width, height);
        if (!result.ok) {
            return vmt::Err<void>(engine_error(vmt::ErrorCode::RenderFailed, result.message));
        }
        return vmt::Ok();
    }

    void shutdown() override {
        if (!initialized_) {
            return;
        }
        (void)spike::shutdown_engine();
        initialized_ = false;
        material_name_.clear();
    }

    bool initialized() const noexcept override { return initialized_; }

private:
    bool initialized_ = false;
    int frame_ = 0;
    std::string material_name_;
    bool diagnostics_installed_ = false;
};

} // namespace

vmt::Result<std::unique_ptr<EngineView>> EngineView::create() {
    return vmt::Ok(std::unique_ptr<EngineView>(new SpikeEngineView()));
}

} // namespace bridge
