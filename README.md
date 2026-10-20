<div align="center">

<img src="vmtstudio-icon.png" alt="VMT-Studio Logo" width="128" height="128" />

# VMT-Studio

**A native, modern material studio powered by Valve's Source Engine renderer — pixel-perfect fidelity, right on your desktop.**

![Release](https://img.shields.io/badge/release-v0.2.0-blue.svg)
![License](https://img.shields.io/badge/license-MIT-green.svg)
![C++](https://img.shields.io/badge/standard-C%2B%2B17-blue.svg)
![Platform](https://img.shields.io/badge/platform-Windows%20x64-lightgrey.svg)
![Qt](https://img.shields.io/badge/GUI-Qt%206.8-green.svg)
[![Engine: Source Engine](https://img.shields.io/badge/Renderer-Source%20Engine%20(Native%20HLMV)-F37021.svg)](https://developer.valvesoftware.com/wiki/Half-Life_Model_Viewer)

*Read this in other languages: [Español](README.es.md)*

</div>

---

**VMT-Studio** is a high-performance material studio, parameter inspector, and real-time 3D previewer for Valve's Source Engine materials (`.vmt`) and textures (`.vtf`). Written in C++17 with a Qt 6 interface, it embeds the actual Source Engine renderer to deliver **100% authentic, 1:1 in-game visual fidelity** — no approximations, no reimplementations.

Unlike generic material viewers that try to replicate Source shaders through custom OpenGL or Vulkan pipelines, VMT-Studio loads Valve's native `materialsystem.dll` and renders through an embedded HLMV (Half-Life Model Viewer) architecture. Every Phong highlight, every bumpmap, every cubemap reflection, every lightwarp — they all look exactly as they do in-game.

---

## Table of Contents

- [Key Features](#key-features)
- [Architecture](#architecture)
  - [Dual-Process Architecture (x64 UI + Win32 HLMV)](#dual-process-architecture-x64-ui--win32-hlmv)
  - [Native Brush & Studio Model Rendering](#native-brush--studio-model-rendering)
  - [Modules Overview](#modules-overview)
- [Supported Games & Engines](#supported-games--engines)
- [Portable Release (Quick Start)](#portable-release-quick-start)
- [Build Requirements](#build-requirements)
- [Building the Project](#building-the-project)
  - [Step 1: Clone and Initialize Submodules](#step-1-clone-and-initialize-submodules)
  - [Step 2: Build VTFLib++ (32-bit and 64-bit)](#step-2-build-vtflib-32-bit-and-64-bit)
  - [Step 3: Build the 32-bit HLMV Backend](#step-3-build-the-32-bit-hlmv-backend)
  - [Step 4: Build the 64-bit Qt 6 Frontend](#step-4-build-the-64-bit-qt-6-frontend)
- [Running and Configuring](#running-and-configuring)
- [Model Viewer](#model-viewer)
- [Testing](#testing)
- [Licenses and Legal Notices](#licenses-and-legal-notices)
- [AI Disclosure](#ai-disclosure)

---

## Key Features

### 🎨 Native Source Engine Rendering

- Loads the game's actual Direct3D 9 libraries (`materialsystem.dll`, `tier0.dll`, `vstdlib.dll`) from your local installation.
- Zero approximation — exact specular highlights, fresnel curves, half-lambert shading, lightwarps, blend modes, and cubemap reflections.

### 🧊 Dual-Surface Preview: Models & Brushes

- **Studio Models**: Sphere, Cube, Cylinder, Plane, or custom `.mdl` models with full animation playback and speed control.
- **Native Brush Geometry**: Real world surfaces (`brush_wall`, `brush_floor`, `brush_cube`, `brush_displacement`) built with Source's `CMeshBuilder` — authentic normals, lightmap UVs, and vertex colors for `LightmappedGeneric` and `WorldVertexTransition` shaders.

### ✏️ Two-Way Live Editing

- **Visual Parameter Inspector** — dynamic, schema-driven property editor with sliders, checkboxes, color pickers, and texture selectors.
- **Code Editor** — syntax-highlighted VMT text editor.
- **Instant Sync** — changes in either editor update the 3D viewport in real time (250 ms debounce).
- **Full Undo / Redo** — unified stack across both editors (`Ctrl+Z` / `Ctrl+Y`).

### 📂 Asset Browser (`F3` / `F4`)

- One modeless window for materials, textures and models (`.vmt`, `.vtf`, `.mdl`) across loose game folders, mounted `.vpk` archives and user-added folders, without unpacking.
- Token search, type/source filters, list and grid views, per-type previews and drag & drop onto the viewport.
- Opens instantly from a per-game index cache (`%LOCALAPPDATA%\VMTStudio\cache\<game-key>\assets-v1.tsv`); **Refresh** (`Ctrl+R`) rescans and rewrites it.
- Thumbnails are decoded off the UI thread and cached per game (`%LOCALAPPDATA%\VMTStudio\thumbnails\<game-key>\`).
- `F4` opens the same window filtered to models; the inspector's texture picker uses it in *pick* mode, so VPK textures are selectable.

### 🔍 Schema Validation & Diagnostics (`F7`)

- Linting against official Source shader schemas (`data/schema`).
- Flags unknown shaders, deprecated parameters, type mismatches, and missing dependencies (e.g., `$phongboost` without `$phong`).
- **Live VFS Texture Check** — queries the game's `IFileSystem` to verify whether referenced VTF textures exist on disk or inside mounted VPKs.

### 💡 Lighting & Scene Studio

- Ambient light, key light (direction, azimuth, elevation, color), fill light, and background clear color.
- Environment presets: *Studio Dark*, *Studio Light*, *Outdoor*, and *Flat*.

### 🔬 Shading & Debug Modes

- Textured / Shaded, Flatshaded, Wireframe, and Normal map / Bumpmap preview.

### 🎮 Automated Game Discovery

- Queries the Windows registry (`HKCU\Software\Valve\Steam`) and `STEAMPATH` to detect Steam installations.
- Parses multi-library manifests (`libraryfolders.vdf`) across all drives.
- Interactive **Known games** dropdown with one-click path configuration and live install status indicators.

### 🛡️ Engine Install Validation

- Multi-stage verification: folder structure, `gameinfo.txt`, PE headers, and binary architecture (enforcing 32-bit x86).
- Validates required DLLs (`tier0.dll`, `vstdlib.dll`, `filesystem_stdio.dll`, `materialsystem.dll`, `shaderapidx9.dll`).
- Export table validation (`CreateInterface`, `CommandLine_Tier0`) with actionable error tags (`empty folder`, `missing DLL`, `wrong arch`, `no exports`).

### ⚡ High-Performance Parsing

- **`vmt_core`** — zero-allocation token scanner with ASCII lookup tables, binary-searched `LineTable`, and byte-exact lossless AST round-tripping.
- **`vtf_reader`** — fast-path downscaling for uncompressed RGBA8888 without intermediate allocations, immediate buffer cleanup, and thread-safe scoped decoding.

### 💾 Safe, Non-Destructive Workflow

- Atomic file writes (`.tmp` swap) to prevent corruption on crashes.
- `File > Save to game folder...` writes overrides directly into the game's `materials/` folder to seamlessly override VPK contents.

---

## Architecture

### Dual-Process Architecture (x64 UI + Win32 HLMV)

Valve's Source Engine 2013 and retail games (Half-Life 2, CS:S, TF2, Garry's Mod) are legacy 32-bit (`x86`) binaries. Qt 6 targets 64-bit (`x64`). Loading 32-bit DLLs into a 64-bit process is impossible on Windows.

VMT-Studio solves this with an out-of-process client-server architecture:

```
┌─────────────────────────────────────────────────────────────┐
│                    vmtstudio.exe (x64)                      │
│                  Frontend UI (Qt 6 MSVC)                    │
│                                                             │
│  ┌───────────────────────┐       ┌───────────────────────┐  │
│  │   Visual Inspector    │       │   Syntax Code Editor  │  │
│  └───────────┬───────────┘       └───────────┬───────────┘  │
│              │                               │              │
│              └───────────────┬───────────────┘              │
│                              ▼                              │
│                    ┌──────────────────┐                     │
│                    │ Viewport Widget  │ ◄─── (Embeds HWND)  │
│                    └─────────┬────────┘                     │
└──────────────────────────────┼──────────────────────────────┘
                               │ Win32 IPC (WM_COPYDATA)
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                   hlmv_backend.exe (x86)                    │
│              Rendering Backend (Win32 / C++17)              │
│                                                             │
│  ┌───────────────────────────────────────────────────────┐  │
│  │   HLMV Core + MX Toolkit + Native Direct3D 9 View     │  │
│  └───────────────────────────┬───────────────────────────┘  │
│                              ▼                              │
│  ┌───────────────────────────────────────────────────────┐  │
│  │       Native Game Engine DLLs (materialsystem.dll,    │  │
│  │          tier0.dll, vstdlib.dll, filesystem)          │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

1. **`vmtstudio` (Qt 6 x64)** — runs the modern UI, handles file I/O, parses schemas, provides syntax highlighting, and hosts a native Win32 child container (`ViewportWidget`).
2. **`hlmv_backend` (Win32 x86)** — launched as a child process with `-embed <HWND> -game <dir> -bin <dir>`. Loads the game's native engine binaries, initializes Direct3D 9, and reparents its viewport into the Qt widget.
3. **High-Speed IPC** — the frontend dispatches commands (`SET_MATERIAL`, `RELOAD_MATERIAL`, `SET_SHAPE`, `SET_LIGHTING`, `CAPTURE_THUMBNAIL`, etc.) via `WM_COPYDATA` messages with zero perceptible latency.

### Native Brush & Studio Model Rendering

Most material viewers can only display textures on a sphere. Source Engine materials, however, fall into two distinct rendering categories:

- **Studio Models (`VertexLitGeneric`)** — require vertex bone weighting, ambient cube calculations, and studio lighting pipelines.
- **World Brushes (`LightmappedGeneric`, `WorldVertexTransition`)** — require surface lightmaps, flat brush planes, or displacement meshes.

`hlmv_backend` includes a custom `brush_render` module that interfaces directly with Valve's `IMesh` and `CMeshBuilder`. It procedurally constructs authentic brush surfaces (Wall, Floor, Cube, Displacements) with accurate normals, lightmap coordinates, and vertex colors — allowing brush shaders to render faithfully without graphical anomalies.

### Modules Overview

| Module | Description |
| --- | --- |
| **`vmt_core`** | Standalone, lossless KeyValues parser, AST model, and serializer. Zero-allocation scanning, binary-searched `LineTable`, byte-exact round-tripping. Preserves comments, whitespace, casing, and quoting. No Windows or Qt dependencies. |
| **`vtf_reader`** | Fast VTF decoder wrapping `VTFLib++`. Supports DXT1, DXT5, RGBA8888, BC4/BC5, cubemaps, and volume mipmaps. Optimized fast paths and minimal peak memory. |
| **`hlmv_backend`** | 32-bit out-of-process engine runner: HLMV core, MX Toolkit (`mxtk`), IPC listener, and brush mesh builder. |
| **`engine_bridge`** | IPC bridge, subprocess lifecycle, PE header inspection, and filesystem compatibility shims (runtime vtable fixups for Garry's Mod `VFileSystem022`). |
| **`game_profiles`** | Steam library auto-detection, `gameinfo.txt` parsing, and comprehensive PE binary validation (architecture, DLLs, exports). |
| **`ui`** | Qt 6 GUI: docks, property inspectors, syntax highlighter, and dialogs. |
| **`data/schema`** | JSON shader definitions: `VertexLitGeneric`, `LightmappedGeneric`, `UnlitGeneric`, `WorldVertexTransition`, `Water`, `Refract`, `Sprite`. |

---

## Supported Games & Engines

The engine bridge supports automatic connection to any Source Engine branch based on Source SDK 2013 / Alien Swarm, including:

| Game | Notes |
| --- | --- |
| **Half-Life 2** (+ Episodes 1 & 2) | |
| **Portal** | |
| **Counter-Strike: Source** | |
| **Team Fortress 2** | |
| **Source SDK Base 2013** (SP / MP) | |
| **Garry's Mod** | Currently not working (authoritative engine modifications; support in development) |
| **Custom Source Mods** | Requires valid `gameinfo.txt` + `/bin` folder (mods with authoritative engine modifications are not working at this time) |

> [!WARNING]
> **Games with authoritative modifications are currently not working:** Games and mods with heavily modified or authoritative engine builds (such as *Garry's Mod* or custom branches with altered filesystem and materialsystem vtables) are not functional at this time. Dedicated compatibility shims for these engines are currently under active development.

---

## Portable Release (Quick Start)

Don't want to build from source? A precompiled portable package is ready to use:

1. **Download** — grab `vmtstudio-v0.2.0-win64.zip` from the [Releases](https://github.com/your-username/VMT-Studio/releases) page.
2. **Extract** — unzip anywhere (e.g. `C:\Tools\VMT-Studio`). No installation or admin rights required.
3. **Run** — launch `vmtstudio.exe`.
4. **Connect to Source Engine**:
   - Open **Game > Connection Settings...**
   - Select your game from the **Known games** dropdown, or click **Discover...** to auto-scan your Steam libraries.
   - Alternatively, specify the game directory (folder with `gameinfo.txt`) and engine `/bin` directory manually.
   - Click **Test Connection** to validate the installation.
5. **Start Editing** — open any `.vmt` via **File > Open** or the **Asset Browser (`F3`)**.

> [!NOTE]
> The portable ZIP bundles `vmtstudio.exe` (64-bit Qt 6 frontend), `hlmv_backend.exe` (32-bit renderer), `VTFLib.dll`, and all Qt 6 runtime dependencies. **No proprietary Valve assets are distributed** — engine DLLs are loaded dynamically from your local game installation.

---

## Build Requirements

| Requirement | Version |
| --- | --- |
| **OS** | Windows 10 / 11 (64-bit) |
| **Compiler** | Visual Studio 2022 (MSVC v143) — *Desktop development with C++* workload |
| **Build System** | CMake 3.21+ |
| **Qt** | Qt 6.8+ (MSVC 2022 64-bit) |
| **Source SDK** | Source SDK 2013 public headers (`SOURCE_SDK_PUBLIC` → `src/public`) |
| **Git** | With submodule support |

---

## Building the Project

The rendering backend must be compiled for **Win32 (32-bit)** to load Valve's 32-bit DLLs, while the Qt 6 frontend targets **x64 (64-bit)**. This requires a two-phase build:

### Step 1: Clone and Initialize Submodules

```powershell
git clone --recursive https://github.com/your-username/VMT-Studio.git
cd VMT-Studio
```

If already cloned:
```powershell
git submodule update --init --recursive
```

### Step 2: Build VTFLib++ (32-bit and 64-bit)

1. Apply the build patch (replaces `afxres.h` with `winres.h` to remove the MFC dependency):
   ```powershell
   cmake -P thirdparty/patches/apply_vtflib_patches.cmake
   ```

2. Build 32-bit VTFLib++ (for `hlmv_backend`):
   ```powershell
   cmake -B thirdparty/vtflib/build_vtflib -A Win32 -DVTFLIB_BUILD_VTFCMD=OFF -DVTFLIB_BUILD_VTFEDIT=OFF
   cmake --build thirdparty/vtflib/build_vtflib --config Release
   ```

3. Build 64-bit VTFLib++ (for `vmtstudio`):
   ```powershell
   cmake -B thirdparty/vtflib/build_vtflib64 -A x64 -DVTFLIB_BUILD_VTFCMD=OFF -DVTFLIB_BUILD_VTFEDIT=OFF
   cmake --build thirdparty/vtflib/build_vtflib64 --config Release
   ```

### Step 3: Build the 32-bit HLMV Backend

```powershell
cmake -S . -B build -A Win32 -DSOURCE_SDK_PUBLIC="<path-to-source-sdk-2013>/src/public"
cmake --build build --config Release --target hlmv_backend
```

### Step 4: Build the 64-bit Qt 6 Frontend

CMake will automatically copy `hlmv_backend.exe` next to `vmtstudio.exe` and invoke `windeployqt`:

```powershell
cmake -S . -B build64 -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.2/msvc2022_64"
cmake --build build64 --config Release --target vmtstudio
```

The standalone executable will be at:
```
build64/bin/Release/vmtstudio.exe
```

---

## Running and Configuring

1. Launch `vmtstudio.exe` (or `build64/bin/Release/vmtstudio.exe` if built from source).
2. Open **Game > Connection Settings...**:
   - Select a game from the **Known games** dropdown, or click **Discover...** to scan Steam libraries.
   - Or manually set the **Game Directory** (containing `gameinfo.txt`) and **Bin Directory**.
   - Click **Test Connection** to validate the installation.
3. Open any `.vmt` via **File > Open** or the **Asset Browser (`F3`)**.
4. The 3D viewport will automatically spawn the embedded HLMV backend and render the material with full Source Engine fidelity.

---

## Model Viewer

VMT-Studio can load and inspect Source Engine studio models (`.mdl`) with the same native renderer used for materials:

- **Open a model** — via **Model > Open Model... (`F4`)** (Asset Browser filtered to models), by **dragging & dropping** a `.mdl` onto the viewport, from **Model > Recent Models**, or from the command line: `vmtstudio --model models/props_c17/oildrum001.mdl`.
- **Materials dock** — every material grouped by bodypart, with search and previous/next navigation.
- **Animation dock** — sequence list with activity filters, timeline scrubbing, playback speed, blending, overlay layers, pose parameters, bodygroups, and skins.
- **Edit materials in place** — selecting a model material opens its VMT in the editor; edits are saved as a loose override at the material's game-relative path. The engine re-resolves only that slot, keeping the rest of the model intact. **Ctrl+S** saves atomically.
- **Export Model Materials (`Ctrl+Shift+E`)** — writes all model materials to disk as `.vmt` files, flat or mirroring the `materials/` hierarchy.
- **Isolate (experimental)** — hides every material except the selected one via transparent substitute overrides. Substitutes are removed automatically when isolation is disabled, the model is closed, the game connection changes, or the app exits.

### Shortcuts

| Shortcut | Action |
| --- | --- |
| `F3` | Open the Asset Browser |
| `F4` | Open the Asset Browser filtered to models |
| `Space` | Play / pause animation |
| `[` / `]` | Halve / double playback speed |
| `Alt+Left` / `Alt+Right` | Select previous / next material |
| `Ctrl+Shift+E` | Export all model materials |
| `Ctrl+M` | Load model from file dialog |

---

## Testing

```powershell
# 64-bit tests (core parser, VTF decoder, UI)
ctest --test-dir build64 -C Release --output-on-failure

# 32-bit tests (engine bridge, system components)
ctest --test-dir build -C Release --output-on-failure
```

---

## Licenses and Legal Notices

### Valve Corporation Disclaimer

- **VMT-Studio does NOT distribute any proprietary Valve binaries, textures, models, or copyrighted game assets.**
- Engine DLLs, materials, and models are loaded dynamically at runtime from a user-supplied, legally acquired local Source Engine installation.
- *Valve*, the *Valve logo*, *Half-Life*, *Source*, *Counter-Strike*, and *Team Fortress* are trademarks and/or registered trademarks of **Valve Corporation**.

### Third-Party Licenses

| Library | License | Notes |
| --- | --- | --- |
| **VTFLib++** | LGPL-2.1 | Linked dynamically. Full source + patches in `thirdparty/`. Shout out to [@ficool2](https://github.com/nicknine) for this library and his contributions to the Source Engine community. |
| **Qt 6** | LGPLv3 / Commercial | © The Qt Company Ltd. |
| **Source SDK 2013** | Valve Source SDK License | |

### VMT-Studio License

This project is open-source software licensed under the **MIT License**. See [LICENSE](LICENSE) for details.

---

## AI Disclosure

Parts of this project were designed, written, refactored, and documented with the assistance of Artificial Intelligence (AI).

If you have an issue with AI-assisted software development: **we genuinely do not care about your opinion**. The tool works, it renders Source engine materials natively with 100% fidelity, solves real problems, and is free and open-source. Use it or don't.
