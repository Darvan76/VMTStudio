# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Model Viewer (Studio `.mdl` inspection)**:
  - Model browser (**Model > Open Model... (`F4`)**) that delegates the scan to the engine file system (loose folders and mounted `.vpk` archives) with search, source filters and inline loading, so the window never blocks.
  - Model loading via drag & drop of a `.mdl` onto the viewport, the **Model > Recent Models** menu, or the command line: `vmtstudio --model <path>` (a positional `.mdl` path is accepted too).
  - **Materials dock**: every material of the loaded model grouped by bodypart, with search and previous/next walk-through (`Alt+Left` / `Alt+Right`).
  - **Animation dock**: sequence list with activity filters, timeline scrubbing fed by the live engine tick, playback speed, blending, overlay layers, pose parameters, bodygroups and skins.
  - **Play / Pause (`Space`)** now also drives the loaded model; **Halve / Double Playback Speed (`[` / `]`)**.
  - **In-place material editing**: selecting a model material opens it in the VMT editor; edits are written as a loose override at the material's own game-relative path and the engine re-resolves only that slot, so the rest of the model keeps rendering. **Save (`Ctrl+S`)** commits the override atomically.
  - **Export Model Materials... (`Ctrl+Shift+E`)**: exports every material of the loaded model as `.vmt` files, flat or mirroring `materials/`.
  - **Isolate (experimental)**: transparent substitute overrides hide every other material of the model; every written substitute is tracked and removed when isolation is turned off, a model is closed/replaced, the connection changes or the app exits.
- **Persistent model state**: `last_model` and repeated `recent_models` entries in the settings store (additive keys; existing settings files remain compatible).

## [0.1.0] - 2026-10-15

### Added
- **Steam Library Auto-Detection (`SteamDetector`)**:
  - Automatic detection of local Steam installation roots by querying the Windows Registry (`HKCU\Software\Valve\Steam`) and the `STEAMPATH` environment variable.
  - Multi-drive Steam library discovery through recursive parsing of Valve KeyValues `libraryfolders.vdf`.
  - Automatic discovery of installed Source Engine games (Half-Life 2, Episode One, Episode Two, Portal, Counter-Strike: Source, Team Fortress 2, Source SDK Base 2013 Singleplayer/Multiplayer, Garry's Mod) and custom mods containing `gameinfo.txt`.
- **Source Engine Installation Validator (`check_install`)**:
  - Robust multi-stage verification pipeline evaluating game directories and engine `/bin` paths prior to spawning the renderer backend.
  - Actionable failure taxonomy identifying exact installation issues:
    - `EmptyFolder`: Directory does not exist or contains no files.
    - `NoGameInfo`: Missing `gameinfo.txt` game descriptor.
    - `NoBin`: Source engine `/bin` directory cannot be located.
    - `WrongArch`: Binary architecture validation enforcing 32-bit x86 PE images required by the HLMV backend.
    - `MissingDll`: Required engine DLLs absent (`tier0.dll`, `vstdlib.dll`, `filesystem_stdio.dll`, `materialsystem.dll`, `shaderapidx9.dll`).
    - `NoExports`: Required engine interface exports missing (`CreateInterface`, `CommandLine_Tier0`).
- **Interactive Game Picker in Connection Dialog**:
  - Pre-populated dropdown list with all auto-discovered Source Engine games and custom mods.
  - Real-time install status indicators (displaying diagnostic error tags for unusable configurations).
  - Single-click path configuration and live "Test Connection" validation.
- **Interactive Parameter Inspector**:
  - Schema-driven visual property editor supporting real-time manipulation of numeric parameters, toggles, color pickers, and texture selectors.
  - Bi-directional debounced synchronization between the visual inspector, syntax-highlighted code editor, and the 3D viewport.
  - Unified undo/redo history stack (`Ctrl+Z` / `Ctrl+Y`) across both inspector and text editor.
- **Shader Inspector Autocomplete Catalog**:
  - Shader dropdown and autocomplete catalog dynamically populated via `ui::shader_picker_names` from loaded JSON schemas (`VertexLitGeneric`, `LightmappedGeneric`, `UnlitGeneric`, `WorldVertexTransition`, `Water`, `Refract`, `Sprite`), enabling quick shader switching without manual typing.
- **Lossless VMT Syntax Preservation**:
  - Abstract Syntax Tree (AST) document model and serializer preserving original comments, formatting, exact whitespace, indentation, key capitalization, and quoting styles during round-trips.
- **Modern Dark UI Theme**:
  - High-contrast dark theme optimized for 3D previewing, material inspection, and shader parameter fine-tuning.

### Changed
- **Modernized C++ Standards Compliance**:
  - Codebase modernized to adhere to ISO C++ Core Guidelines and Trail of Bits Modern C++ standards.
  - Comprehensive C++17 adoption: widespread usage of `std::string_view` across parser and lookup paths to avoid heap allocations.
  - Systematic `noexcept` annotations on move operations, queries, and predicates.
  - Modern `std::filesystem` usage for platform-independent path operations.
  - Enforced RAII memory and process management using smart pointers (`std::unique_ptr`, `std::shared_ptr`).

### Performance
- **Instant UI Startup via Asynchronous Engine Initialization (`ensureEngineAsync`)**:
  - Background thread engine spawning and Direct3D 9 initialization, eliminating UI thread blocking and delivering near-instant application startup (~1.2s cold start).
  - Non-blocking viewport status notifications while the engine backend connects in the background.
- **High-Throughput VMT Parsing (`vmt_core`)**:
  - `LineTable` binary search delivering O(log L) source location mapping (1-based line/column) from byte offsets instead of full sequential buffer rescans.
  - Table-driven ASCII delimiter token scanning minimizing heap allocations during lexical analysis.
  - Zero-heap float and boolean parsing utilizing stack-allocated buffers (`char[64]`) and length-switched branch matching.
  - Fast-path string unquoting bypassing decoding passes when no escape sequences are present.
- **Optimized VTF Texture Decoding (`vtf_reader`)**:
  - Direct fast-path downscaling for uncompressed RGBA8888 textures resizing straight into the target buffer without intermediate allocations.
  - Immediate deallocation of intermediate buffers (`std::vector::swap`) to significantly reduce peak memory footprint during preview rendering.
  - Thread-safe VTFLib execution using RAII-scoped mutex locks (`std::scoped_lock`).

### Fixed
- Addressed memory spikes during large texture preview generation by immediately freeing intermediate RGBA conversion buffers.
- Fixed potential concurrency hazards and race conditions in multi-threaded VTF decoding through scoped RAII locking.
- Prevented editor desynchronization and race conditions during rapid typing via a 250 ms debounced preview timer.

### Security
- **Hardened PE Header Parser**:
  - Bounds-checked PE image inspection via `ByteView` with strict integer overflow prevention (`offset > size || size - offset < N`).
  - Enforced file size scan caps (`kMaxExportScanBytes = 8 MB`) to prevent denial-of-service and memory exhaustion on maliciously oversized or malformed binary files.
  - Length-bounded string scanners preventing buffer overflows or infinite loops on truncated or non-NUL-terminated PE export directory names.
