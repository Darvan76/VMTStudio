#pragma once

// engine_view - hosts the game's own engine in a child window to render a
// preview of the material being edited.
//
// This is the Phase 3/6 bridge between the UI and the engine spike: the engine
// DLLs are loaded lazily by initialize(), render() draws one frame into the
// caller-provided child window, and the caller owns the message loop (the
// engine never pumps messages here).
//
// Phase 9 adds preview controls (shape, lights/background), engine-side
// material discovery for the browser, and offscreen thumbnail rendering. All
// of it stays best-effort: a game binary that refuses an operation returns an
// error and the viewer keeps working.
//
// This header is Valve-free and Windows-header-free (the window handle travels
// as void*, matching the engine's own SetMode convention).

#include "vmt/result.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bridge {

// Preview primitive drawn with the current material. Selected in
// View > Preview options.
enum class PreviewShape {
    Sphere,
    Cube,
    Cylinder,
    Plane,
    BrushWall,
    BrushFloor,
    BrushCube,
    BrushDisplacement,
};

// Light rig and clear color for the preview. Directions point along the light's
// travel (from the light towards the object) and do not need to be normalized;
// colors are linear 0..1 triples. Valve-free POD so the UI can own the values.
struct PreviewLighting {
    float ambient[3] = { 0.30f, 0.30f, 0.32f };
    float key_dir[3] = { -0.42f, -0.70f, -0.58f };
    float key_color[3] = { 1.0f, 1.0f, 1.0f };
    float fill_dir[3] = { 0.55f, -0.25f, 0.60f };
    float fill_color[3] = { 0.22f, 0.22f, 0.26f };
    float background[3] = { 0.16f, 0.16f, 0.17f };
};

// A material found by list_materials(): the game-relative name without the
// extension, using forward slashes ("brick/brickwall001a").
struct MaterialName {
    std::string name;
};

// Pixels read back from an engine render target: BGRA, row 0 is the top row
// (already flipped vertically if `flip_vertical` was requested).
struct Thumbnail {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> bgra;
};

// --- Model viewer data (IPC protocol v2, see docs/IPC_PROTOCOL.md) ------------
//
// Every type below is a plain value type filled by the query methods of
// EngineView. "Game-relative" always means a forward-slash path relative to the
// game folder ("models/props_c17/oildrum001.mdl"), which is how the engine file
// system addresses loose files and VPK entries alike.

// One .mdl found by list_models().
struct ModelEntry {
    std::string name;      // game-relative, ends in ".mdl"
    bool loose = false;    // true when it lives on disk (not inside a VPK)
};

struct ModelList {
    std::vector<ModelEntry> entries;
    bool truncated = false; // the backend stopped after its safety cap
};

// A material of the loaded model, indexed by the model's texture table.
struct ModelMaterial {
    int texture_index = -1;
    std::string name;    // game-relative, no extension
    std::string shader;  // empty when the material could not be resolved
};

// A bodypart/submodel that references a material (the same material can be
// shared by several submodels).
struct ModelMaterialUse {
    int texture_index = -1;
    int bodypart = -1;
    int submodel = -1;
    std::string bodypart_name;
};

struct ModelSequence {
    int index = 0;
    std::string label;
    std::string activity;  // empty when the sequence has no activity
    float fps = 0.0f;
    int frames = 0;
    float duration = 0.0f; // seconds
    bool looping = false;
    bool hidden = false;   // STUDIO_HIDDEN
};

struct ModelBodypart {
    int index = 0;
    std::string name;
    int models = 0;   // number of submodels
    int current = 0;  // currently selected submodel
};

struct ModelSkin {
    int index = 0;
    std::string name;
};

struct ModelBone {
    int index = 0;
    std::string name;
    int parent = -1;
};

struct ModelAttachment {
    int index = 0;
    std::string name;
};

struct ModelPoseParameter {
    int index = 0;
    std::string name;
    float min_value = 0.0f;
    float max_value = 1.0f;
};

struct ModelStats {
    int polys = 0;
    int batches = 0;
    int bones = 0;
    int lods = 0;
    int materials = 0;
    float bbox_min[3] = { 0.0f, 0.0f, 0.0f };
    float bbox_max[3] = { 0.0f, 0.0f, 0.0f };
};

// Everything the UI knows about the loaded model. `model_name` is empty when no
// model is loaded.
struct ModelScene {
    std::string model_name;
    ModelStats stats;
    std::vector<ModelMaterial> materials;
    std::vector<ModelMaterialUse> material_uses;
    std::vector<ModelSequence> sequences;
    std::vector<ModelBodypart> bodyparts;
    std::vector<ModelSkin> skins;
    std::vector<ModelBone> bones;
    std::vector<ModelAttachment> attachments;
    std::vector<ModelPoseParameter> pose_parameters;
};

// Playback state polled for the animation UI. `frame` is 0-based. The defaults
// describe "no data": running at normal speed, which keeps the formatted status
// neutral until the first real dump arrives.
struct AnimState {
    int sequence = 0;
    float frame = 0.0f;
    float cycle = 0.0f;
    float speed = 1.0f;
    float ground_speed = 0.0f;
    bool paused = false;
};

struct EngineViewConfig {
    std::filesystem::path bin_dir;    // folder with tier0.dll, materialsystem.dll, ...
    std::filesystem::path game_dir;   // folder containing gameinfo.txt
    void* child_window = nullptr;     // HWND of the host child window
    unsigned steam_appid = 0;         // optional; set before loading DLLs
    int width = 800;                  // initial device/backbuffer size
    int height = 600;
};

class EngineView {
public:
    static vmt::Result<std::unique_ptr<EngineView>> create();

    virtual ~EngineView() = default;

    // Loads the engine DLLs, mounts the game's search paths and creates the
    // device on the child window. Lazy: call it when the user asks for a
    // preview, not at startup.
    virtual vmt::Result<void> initialize(const EngineViewConfig& config) = 0;

    // Mounts an extra directory (game-style layout: <dir>/materials/...) with
    // the "GAME" path id. When `search_first` is true the directory takes
    // priority over the game's own content, which is how the edited material
    // overrides the original file.
    virtual vmt::Result<void> mount_directory(const std::filesystem::path& dir,
                                              bool search_first) = 0;

    // Loads a material by name from the mounted search paths.
    virtual vmt::Result<void> set_material(const std::string& material_name) = 0;

    // Forces the current material to be reloaded from disk after it changed.
    virtual vmt::Result<void> reload_material() = 0;

    // Checks whether a game-relative file (for example
    // "materials/foo/bar.vtf") exists in the mounted search paths, including
    // VPKs. Used by validation for texture lookups.
    virtual vmt::Result<bool> file_exists(const std::string& game_path) = 0;

    // Enumerates every .vmt in the mounted search paths (game folders + VPKs).
    // Names are game-relative without extension. Best-effort; the engine file
    // system must be initialized.
    virtual vmt::Result<std::vector<std::string>> list_materials() = 0;

    // Reads a text file (a VMT, for example) through the engine file system,
    // so materials that live inside VPKs can be opened for editing.
    virtual vmt::Result<std::string> read_text_file(const std::string& game_path) = 0;

    // Reads an arbitrary file as raw bytes through the engine file system, so
    // binary assets that live inside VPKs (textures above all) can be handed to
    // the in-process VTF decoder. Needed because the viewer decodes textures
    // itself: the engine's material system is not required to view them.
    //
    // Returns NotFound when the path does not resolve in the mounted search
    // paths. Oversized files (> 256 MiB) are refused rather than buffered.
    virtual vmt::Result<std::vector<unsigned char>> read_binary_file(
        const std::string& game_path) = 0;

    // Selects the primitive used for the preview.
    virtual vmt::Result<void> set_preview_shape(PreviewShape shape) = 0;

    // Applies a light rig and viewport clear color.
    virtual vmt::Result<void> set_lighting(const PreviewLighting& lighting) = 0;

    // Loads a .mdl model to preview. Pending: the StudioRender headers this
    // path needs are not wired yet; the call reports that cleanly.
    virtual vmt::Result<void> set_model(const std::filesystem::path& model_path) = 0;

    // Offscreen render of one material into a BGRA thumbnail. Does not change
    // the current preview material; returns a clear error when the game
    // binaries cannot provide a readable render target.
    virtual vmt::Result<Thumbnail> render_material_thumbnail(const std::string& material_name,
                                                             int size,
                                                             bool flip_vertical) = 0;

    // Sets render debug mode ("shaded", "wireframe", "albedo", "normals").
    virtual vmt::Result<void> set_render_mode(const std::string& mode) = 0;

    // Controls animation playback ("play", "pause", "toggle").
    virtual vmt::Result<void> set_animation(const std::string& state) = 0;

    // Sets animation playback speed scale (e.g. 1.0f is normal speed).
    virtual vmt::Result<void> set_speed(float speed) = 0;

    // --- Model viewer: queries (protocol v2) ---------------------------------
    //
    // Query methods talk to the backend through response files and wait for
    // them, so they block the calling thread for a few milliseconds up to the
    // timeout. Call them from a worker thread, never from the UI thread.

    // Every .mdl in the game file system (loose folders and VPKs). `prefix` is
    // an optional game-relative filter ("models/props_c17/"); empty lists all.
    virtual vmt::Result<ModelList> list_models(const std::string& prefix) = 0;

    // Everything the UI needs about the currently loaded model in one round
    // trip. Fails when no model is loaded.
    virtual vmt::Result<ModelScene> query_model_scene() = 0;

    // Playback state of the loaded model (frame, cycle, speed, ground speed).
    virtual vmt::Result<AnimState> query_anim_state() = 0;

    // Reads a game file as raw bytes through the engine file system, so assets
    // inside VPKs are reachable (textures above all), same contract as
    // read_binary_file().
    virtual vmt::Result<std::vector<unsigned char>> read_game_binary(
        const std::string& game_path) = 0;

    // --- Model viewer: animation control (protocol v2) -----------------------

    virtual vmt::Result<void> set_sequence(int index) = 0;
    virtual vmt::Result<void> set_sequence_label(const std::string& label) = 0;
    virtual vmt::Result<void> set_frame(int frame) = 0;
    virtual vmt::Result<void> step_frame(int delta) = 0;
    virtual vmt::Result<void> set_loop(bool enabled) = 0;
    virtual vmt::Result<void> stop_animation() = 0;
    virtual vmt::Result<void> set_blend_time(float seconds) = 0;
    virtual vmt::Result<void> blend_now() = 0;

    // Overlay layers are 1-based (1..4); sequence -1 clears the layer.
    virtual vmt::Result<void> set_overlay(int layer, int sequence, float weight) = 0;
    virtual vmt::Result<void> set_pose_parameter(int index, float value) = 0;
    virtual vmt::Result<void> set_bodygroup(int group, int value) = 0;
    virtual vmt::Result<void> set_skin(int skin) = 0;

    // View flags: "bones", "hitboxes", "attachments", "ground", "movement",
    // "overlay_wireframe", "normals", "tangents", "shadow", "sequence_boxes".
    virtual vmt::Result<void> set_view_flag(const std::string& flag, bool enabled) = 0;

    // Forces the named material to be reloaded from the mounted search paths
    // after the file changed on disk, without setting it as the global
    // override. Used by the "edit the material in place" workflow.
    virtual vmt::Result<void> reload_material_by_name(const std::string& material_name) = 0;

    // Draws and presents one frame. Call from the host's message loop/timer.
    virtual vmt::Result<void> render() = 0;

    // Tracks the new host size for the projection aspect. The device backbuffer
    // is created once at initialize() and never reset here: the 2013 game
    // binaries fault inside shaderapidx9 when SetMode resets the device after
    // shaders have been used. The engine presents the fixed backbuffer scaled
    // to the window, so a large initial size keeps the image sharp.
    virtual vmt::Result<void> resize(int width, int height) = 0;

    // Ordered engine shutdown. Safe to call more than once and when the engine
    // was never initialized. The host window is never destroyed by the engine.
    virtual void shutdown() = 0;

    virtual bool initialized() const noexcept = 0;
};

} // namespace bridge
