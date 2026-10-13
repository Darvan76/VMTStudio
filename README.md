# VMTViewer

Native Windows (x86) viewer/editor for Source Engine VMT materials and VTF
textures. Built with C++17, CMake and Visual Studio 2022 (`-A Win32`, static
CRT `/MT`). Engine previews are rendered by the game's own `/bin` DLLs; the
application itself only reads files and metadata.

Modules:

- `vmt_core` - lossless VMT parser/document model/serializer (no engine, UI or
  Windows dependencies).
- `vtf_reader` - VTF metadata reading and RGBA8 preview decoding (wraps
  VTFLib++).
- `renderer`, `engine_bridge`, `game_profiles`, `ui` - engine preview wiring,
  game install/profile handling, VMT validation and the application shell.
- `tests` - `vmt_tests` (vmt_core) and `vtf_tests` (vtf_reader, fixtures and
  fuzz checks).

No Valve files are redistributed with this application. It contains no game
content: engine binaries, materials, models and textures are read from a game
installation on this machine, selected by the user in `Game > Connection`
(the About dialog states this as well).

## Third-party: VTFLib++ (LGPL-2.1)

`vtf_reader` wraps [VTFLib++](https://github.com/ficool2/VTFLib), pinned as a
git submodule at `thirdparty/vtflib` (master commit `b27ecd3`, which includes
the BC4/BC5/BC6H_UNSIGNED image format id corrections). VTFLib++ is licensed
under the **GNU Lesser General Public License v2.1**; the license text is at
`thirdparty/vtflib/LGPL.txt`.

Linking mode: **DLL**. The viewer links VTFLib++'s import library and ships
the runtime DLL next to its executables.

Build the DLL from the submodule (32-bit, no Qt: VTFCmd and VTFEdit are not
used):

```
cmake -B thirdparty/vtflib/build_vtflib -A Win32 -DVTFLIB_BUILD_VTFCMD=OFF -DVTFLIB_BUILD_VTFEDIT=OFF
cmake --build thirdparty/vtflib/build_vtflib --config Release
cmake --build thirdparty/vtflib/build_vtflib --config Debug
```

Runtime locations:

- `thirdparty/vtflib/build_vtflib/bin/VTFLib++.dll` (Release)
- `thirdparty/vtflib/build_vtflib/bin/VTFLib++_d.dll` (Debug)

The viewer's build copies the matching DLL next to every executable that links
`vtf_reader` (for example `build/bin/Release/VTFLib++.dll`) through
`vtf_deploy_vtflib_runtime()` in `vtf_reader/CMakeLists.txt`. Replacing that
DLL replaces the VTFLib++ runtime used by the viewer, as the LGPL allows.

### Patch to VTFLib++ (source offer)

VTFLib++'s `VTFLib/Resource.rc` includes `afxres.h`, a header that only ships
with the optional MFC component of the Visual Studio build tools. The resource
script contains no MFC resources, so VMTViewer carries exactly one build-only
patch:

- `thirdparty/patches/vtflib-resource-winres.patch` - replaces the
  `afxres.h` include with `winres.h` (Windows SDK).

It changes only how the resource compiler parses the DLL version resource;
library behavior is unchanged. The patch is applied automatically and
idempotently:

- when the viewer is configured with CMake, and
- by the `vtflib_apply_patch` build target:
  `cmake --build build --target vtflib_apply_patch`.

Equivalently: `cmake -P thirdparty/patches/apply_vtflib_patches.cmake`.

The modified library source is offered in full: the pinned submodule plus this
patch reproduces the exact VTFLib++ sources used by VMTViewer, satisfying the
LGPL-2.1 source-availability requirement for the modified library.

## Build

```
git submodule update --init --recursive

# 1. Apply the VTFLib++ patch (step 3 does this automatically as well)
cmake -P thirdparty/patches/apply_vtflib_patches.cmake

# 2. Build VTFLib++ (see the commands above)

# 3. Configure, build and test the viewer
cmake -S . -B build -A Win32
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Engine preview

`Game > Connection...` picks the game's `bin` folder and the game folder that
contains `gameinfo.txt` (auto-detected from `/bin` when possible). The user
chooses their own installed game; no game or path is hardcoded.
Opening a material then loads the engine automatically and renders it in the
center viewport; `Preview material` forces a manual start/refresh (useful
before the connection is configured). The edited VMT is written to a temporary
search path (mounted with the `GAME` path id) and reloaded into the running
engine after a 250 ms debounce, so the sphere follows edits from the parameter
panel or the text editor without restarting; saving forces an immediate
refresh. While the text has syntax errors the last good render stays on screen
and the error is shown in the status bar.

The engine device is created once, at preview start, with a backbuffer of the
viewport size (minimum 1280x720, capped to the desktop size). Resizing the
window does **not** reset the device: the 2013 game binaries fault inside
`shaderapidx9.dll` when `SetMode` resets a device after shaders were used
(`stdshader` restore reads an unbound vertex shader). The host size only
re-aims the projection aspect and D3D presents the fixed backbuffer scaled to
the window, so circles stay round, the preview never crashes on resize, and
large windows still get a sharp image from the oversized backbuffer.

The engine is embedded with the tool sequence the engine's own launcher uses:
filesystem init from `gameinfo.txt`, `ICvar::Connect` (initializes vstdlib's
internal cvar query), a composite factory for `IMaterialSystem::Connect`
(shader hardware config + cvar interfaces; engine-only interfaces stay null),
then `IMaterialSystem::Init` and `SetMode` on the viewport child window. Game
DLLs are loaded with `LOAD_WITH_ALTERED_SEARCH_PATH` so bin-local dependencies
(steam_api.dll and friends) resolve.

Before initializing the engine in-process, the viewer runs a one-shot
compatibility check in a disposable child process (`vmtviewer --engine-probe`).
A faulty game install then cannot take the viewer down: the user gets an
actionable message instead of a crash. The engine log with the crash dump is
written to `%TEMP%\VMTViewer\engine.log`.

Verified working with Half-Life 2 (current build) and Source SDK Base 2013
Singleplayer. Garry's Mod's file system is supported through a runtime vtable
layout shim: its `VFileSystem022` inserts four methods before `AddSearchPath`
(every later slot shifts by +4, so the stock SDK calls corrupted the stack
there). The viewer detects the layout at startup and resolves the right slots,
so material listing and reading files through the engine file system —
including from VPKs — work on Garry's Mod. Its engine renderer, however, stays
unavailable: the newer material-system branch requires engine-only interfaces
(QueuedLoader, texture streaming, ...) during `IMaterialSystem::Connect` and
terminates the process when they are missing. The compatibility check detects
the branch and declines the engine preview with a clear message, so the viewer
stays usable.

## Material browser (Phase 9)

`View > Material browser` (or `F3`) opens a modeless window with a search box,
a tree grouped by folder and a thumbnail pane.

- **Scan game** enumerates every `.vmt` in the connected game's mounted file
  system, including VPKs, through the same engine interface the preview uses.
- **Scan folder...** walks a directory on disk for `.vmt` files; this works
  without a game connection. The last scanned folder is restored on the next
  start.
- **Thumbnails** are rendered by the engine (the material on a lit sphere) and
  cached as 24-bit BMP files under
  `%LOCALAPPDATA%\VMTViewer\thumbnails\<hash>.bmp`; selecting a cached
  material again re-renders it, and browsing without an engine falls back to
  the disk cache. A thumbnail renders into the device backbuffer and is read
  back before it is presented, so the visible preview is not disturbed. If a
  game's binary returns the pixels upside down, the "Flip thumbnails
  vertically" checkbox (persisted in the settings) corrects it.
- **Double-click** (or Open) loads the material into the editor. Materials
  inside VPKs are read through the engine file system, so no extraction step
  is needed; `Save` on such a material opens Save As, while `Save to game
  folder...` writes the loose override described below.

Editing a game material live: the edited copy is written to the temporary
preview root at the material's own relative path
(`materials/<name>.vmt`), and that root is mounted with the highest search
priority, so the engine resolves the material name to the edited copy while
its textures still come from the game. The debounced update/edit loop from
Phase 7 is unchanged.

## Editing and saving (Phase 10)

- **Undo / redo** (`Ctrl+Z` / `Ctrl+Y`) covers both the parameter panel and the
  raw text editor. Consecutive typing coalesces into a single step; panel edits
  are always their own step. The history is cleared when another material is
  opened.
- **Edit > Revert to saved/opened** restores the last saved revision (or the
  material as it was opened, before the first save). The revert itself can be
  undone.
- **Unsaved changes are guarded**: opening another material (dialog, recent
  list or browser) asks Save / Discard / Cancel, exactly like closing the app.
  A cancelled Save As keeps the current material.
- **Save / Save As write atomically**: the bytes go to a sibling `.tmp` file
  which then replaces the target, so a crash or a full disk cannot truncate an
  existing VMT.
- **File > Save to game folder...** writes the edited material into the
  connected game's own `materials/` folder as a loose override (loose files win
  over VPK entries in the engine search order). This is the workflow for
  materials opened from VPKs; the confirmation dialog shows the exact target
  and warns before overwriting. `Save` on a VPK material still opens Save As.

## Preview options (Phase 9)

`Preview > Preview options...` controls the preview scene:

- **Shape**: sphere, cube, cylinder, plane (also as radio items directly in the
  Preview menu). Flat shapes render double-sided.
- **Environment**: Studio dark, Studio light, Outdoor and Flat presets; moving
  any slider switches to a custom rig.
- **Light rig**: ambient level, key light azimuth/elevation/intensity and the
  background level. Changes apply to the running engine immediately.
- **Load model (.mdl)...** is present and wired to the engine call, but the
  model path itself is pending the StudioRender headers for this engine; the
  viewer reports that cleanly instead of failing.

## Settings and logging (Phase 9)

- All options above, the browser source and the game connection are persisted
  in `%APPDATA%\VMTViewer\settings.ini`. Saves are atomic (write to `.tmp`
  then replace), so a crash mid-save cannot corrupt the settings.
- The viewer log is `%LOCALAPPDATA%\VMTViewer\vmtviewer.log`: append-only,
  flushed per line, with a `session.lock` marker that distinguishes a clean
  exit from a crash and a vectored exception handler that writes `CRASH`
  records (module + offset + stack). The engine log stays at
  `%TEMP%\VMTViewer\engine.log`.
- `Help > About` states the no-Valve-files position and the VTFLib++ (LGPL-2.1)
  attribution.

## Validation

`Tools > Validate` (or `F7`, or the `Validate` button) checks the open material
and opens a results window; double-clicking a finding moves the caret to its
line. Severities: **error** (recovered parse diagnostics, missing textures),
**warning** (unknown shader, unknown parameter, type/range mismatch, missing
dependency parameter such as `$phongboost` without `$phong`, backslashes,
`.vtf` extension, duplicate parameter) and **info** (uppercase path style,
checks that could not run).

Texture existence is resolved through the engine file system of the connected
game - `IFileSystem::FileExists`, which walks every mounted search path
including VPKs - so the check only runs once a preview engine is initialized;
otherwise validation reports that texture checks were skipped. `_rt_*` render
targets and `$envmap`'s `env_cubemap` are exempt. Rules that need schema
knowledge (parameter names, types, ranges, dependencies) use the same
`data/schema` files as the parameter panel; shaders without a schema get a
single unknown-shader warning and only generic checks run.

## Tests

- `vmt_tests` - vmt_core parser/editor suites.
- `ui_tests` - session/editing core, panel model, settings (including the
  Phase 9 keys and the atomic save), preview sync, preview options, the
  material index/search/thumbnail-cache helpers and one validation test per
  rule against `tests/data/validate_*.vmt` fixtures.
- `vtf_tests` - metadata, decode, error handling and fuzz checks. The fixture
  checks run against `tests/data/vtf/expected.json` and the VTF files it
  references (schema documented at the top of `tests/vtf_tests.cpp`); the
  committed set must cover DXT1, DXT5, RGBA8888, mipmaps, alpha and
  multi-frame decoding. Regenerate it with the maintenance tool:

  ```
  cmake --build build --target make_vtf_fixtures --config Release
  build\bin\Release\make_vtf_fixtures.exe tests\data\vtf
  ```
