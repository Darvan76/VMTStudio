#pragma once

#include <string>
#include <vector>

// Plain, Valve-free interface for the Phase 3 engine spike. All engine calls
// happen on the calling (main) thread.
namespace spike {

enum class MountMode {
    All,        // full gameinfo.txt mount, then material system + render
    NoAddons,   // mount every entry except those whose location mentions "addons"
    AddonsOnly, // mount exactly one directory from the addons wildcard (one AddSearchPath call)
    Single,     // mount exactly --path once (path ID "GAME")
    SingleDup,  // mount exactly --path twice (path IDs "GAME" then "MOD")
    Probe,      // layout probe: mount game_dir through the detected vtable slots, then verify FileExists + read + find
};

struct EngineOptions {
    std::string bin_dir;    // game /bin folder (DLLs live here)
    std::string game_dir;   // directory containing gameinfo.txt
    std::string material;   // material name to render, e.g. "dev/dev_monotone_basetexture"
    int width = 800;
    int height = 600;

    MountMode mount_mode = MountMode::All;
    std::string single_path;   // used by MountMode::Single / SingleDup
    unsigned steam_appid = 0;  // when non-zero, sets SteamAppId before any DLL load

    // UI-phase integration: render into a caller-provided child window instead
    // of creating a top-level one, and let the caller own the message pump.
    void* child_hwnd = nullptr;
    bool pump_messages = true;
};

struct StepResult {
    bool ok = false;
    std::string message; // log line: success summary or exact error text
};

// Installs the vectored exception handler (SEH) and, in debug builds, the
// CRT/RTC wiring. Call this as the first statement of the program.
void install_diagnostics();

// Sets the SteamAppId environment variable. Call before loading any game DLL.
void apply_steam_appid(unsigned appid);

StepResult init_filesystem_only(const EngineOptions& opts); // filesystem only (no D3D/matsys)
StepResult init_engine(const EngineOptions& opts);   // steps 3-4: filesystem + material system + view
StepResult render_frame(int frame);                  // step 5: draw sphere + present
StepResult shutdown_engine();                        // step 6: ordered shutdown

// --- UI-phase extensions -----------------------------------------------------

// Mounts an extra search path (used to preview a VMT written to a temp dir).
// `to_head` inserts the path at the highest priority so files in it override
// the game's own copies; false appends it behind the game content.
StepResult mount_custom_path(const std::string& path_id, const std::string& full_path,
                             bool to_head = false);

// Loads a material by name from the mounted search paths and makes it the
// current preview material.
StepResult set_material(const std::string& material_name);

// Forces the material with this name (and materials it references) to be
// reloaded from disk. Pass the preview material's name.
StepResult reload_material(const std::string& material_name);

// Tracks the new host size (projection aspect). The device backbuffer is
// created once in init_engine and never reset: SetMode after shaders were used
// faults inside shaderapidx9 in the 2013 binaries. D3D presents the fixed
// backbuffer scaled to the window.
StepResult resize_view(int width, int height);

// --- Phase 8 validation -------------------------------------------------------

// Whether a game-relative path (e.g. "materials/foo/bar.vtf") exists in the
// mounted search paths, including VPKs. `ok` is false when the file system is
// not initialized.
struct ExistsResult {
    bool ok = false;
    bool exists = false;
};
ExistsResult file_exists(const std::string& game_path);

// --- Phase 9 preview options and material browser ----------------------------

// Primitive drawn with the preview material. Mirrors bridge::PreviewShape
// without leaking the public header into this TU.
enum class PreviewShape {
    Sphere,
    Cube,
    Cylinder,
    Plane,
};

// Plain light rig. Directions point along the light's travel and are
// normalized by the engine; colors are linear 0..1 triples.
struct Lighting {
    float ambient[3] = { 0.30f, 0.30f, 0.32f };
    float key_dir[3] = { -0.42f, -0.70f, -0.58f };
    float key_color[3] = { 1.0f, 1.0f, 1.0f };
    float fill_dir[3] = { 0.55f, -0.25f, 0.60f };
    float fill_color[3] = { 0.22f, 0.22f, 0.26f };
    float background[3] = { 0.16f, 0.16f, 0.17f };
};

StepResult set_preview_shape(PreviewShape shape);
StepResult set_lighting(const Lighting& lighting);

// Enumerates .vmt files in the mounted search paths. Names are game-relative
// without the extension ("materials/foo/bar.vmt" -> "foo/bar"). `ok` is false
// when the file system is not initialized.
struct MaterialList {
    bool ok = false;
    bool truncated = false;
    std::vector<std::string> names;
};
MaterialList list_materials();

// Reads a text file through the engine file system (VPK-aware).
struct TextFile {
    bool ok = false;      // the file system answered
    bool found = false;   // the file exists
    std::string text;
};
TextFile read_text_file(const std::string& game_path);

// Reads any file as raw bytes through IFileSystem, so assets inside VPKs
// (textures) reach the viewer's own VTF decoder without the engine's material
// system having to be usable.
struct BinaryFile {
    bool ok = false;    // the file system answered
    bool found = false; // the file exists
    std::string message;
    std::vector<unsigned char> bytes;
};
BinaryFile read_binary_file(const std::string& game_path);

// Renders one material offscreen and reads the pixels back. BGRA, top-down
// after the optional vertical flip. Never touches the on-screen preview
// material. `ok=false` carries a message.
struct ThumbnailResult {
    bool ok = false;
    std::string message;
    int width = 0;
    int height = 0;
    std::vector<unsigned char> bgra;
};
ThumbnailResult render_material_thumbnail(const std::string& material_name, int size,
                                          bool flip_vertical);

// Loads a .mdl model for the preview. Pending: the StudioRender headers are not
// wired yet, so this always reports that cleanly.
StepResult set_model(const std::string& model_path);

bool is_initialized();

} // namespace spike
