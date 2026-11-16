# Left 4 Dead 2 compatibility preparation

Date: 2026-11-16. Development branch: `codex/l4d2-support`.

This branch prepares detection, diagnostics and the implementation plan for
Left 4 Dead 2. **It does not yet provide native L4D2 rendering.** No game files
are changed or redistributed. Existing uncommitted work was preserved when
creating the branch; this document describes only the L4D2 preparation.

## Engine choice

Left 4 Dead 2 was released in 2009, and uses a distinct Source branch. Valve's
[Source SDK 2013 repository](https://github.com/ValveSoftware/source-sdk-2013)
publishes game code for Half-Life 2, Half-Life 2: Deathmatch and Team Fortress 2;
it is not a drop-in L4D2 renderer SDK. See the
[L4D2 Steam page](https://store.steampowered.com/app/550/Left_4_Dead_2/) and
[Valve's original tools announcement](https://store.steampowered.com/oldnews/?enddate=1262332800&feed=l4d_blog&headlines=0)
(23 December 2009, “All I want for Christmas is an SDK”).

Keep the existing SDK 2013 backend. Add a separate Win32 backend for L4D2,
behind the existing Valve-free `bridge::EngineView` and IPC contracts. The Qt
frontend, parser, undo history, asset index and VTF decoder can be reused.
Select the backend using both the game profile and validated runtime evidence.
The current preparation recognizes the installed runtime and refuses it before
calling the legacy engine interfaces, rather than applying the GMod adapters.

## Installed evidence

Inspected read-only on this machine:

- Installation paths are omitted; use `-GameRoot` to select a local installation.
- Game: Steam app 550, build 23990068; Authoring Tools: app 563, build 14602762.
- Runtime DLLs and `bin/hlmv.exe` are real x86 PE files.
- `left4dead2/pak01_dir.vpk` is a real VPK v1 directory.
- `gameinfo.txt` specifies app 550 and tools app 563.

Interface names below were observed as strings in the installed DLLs. The SDK
column comes from the headers configured in this workspace, including local
modifications, not an assertion about every SDK 2013 revision.

| Contract | Installed L4D2 evidence | Current configured backend headers |
| --- | --- | --- |
| Filesystem | `VFileSystem018` | `VFileSystem022` |
| Material system | `VMaterialSystem080` | `VMaterialSystem082` (existing fallback 080) |
| Studio renderer | `VStudioRender026` | `VStudioRender025` |
| Material configuration | `VMaterialSystemConfig004` | `VMaterialSystemConfig002` |
| Console variables | `VEngineCvar007` | `VEngineCvar004` |
| Model cache | `MDLCache004` | `MDLCache004` |

Strings can describe requested interfaces as well as provided ones. Even an
equal version name does not prove equal vtables, calling conventions, callback
signatures or structure layouts. Do not alias interface versions to work around
these differences.

The separate x86 `engine_abi_probe` was also compiled and run against the
installed runtime. `CreateInterface` accepted filesystem 018, material 080,
studio 026, cvar 007, model cache 004 and data cache 003. It rejected filesystem
017/022, material 082, studio 025 and cvar 004. The process exited with code 0;
transcript: `scratch/l4d2/factory-probe.tsv`. This confirms factory availability
for those names, without calling interface methods or verifying structure ABI.

Build and run this optional diagnostic in its own process:

```powershell
cmake --build build --config Release --target engine_abi_probe
& .\build\bin\Release\engine_abi_probe.exe `
  '<L4D2_INSTALL>/bin'
```

It loads executable game DLLs in the diagnostic process and calls only their
factories. Run it against the selected trusted game install, with a timeout
when automating it. The static PowerShell inventory below does not load
game code. Neither diagnostic initializes the renderer.

A bounded hidden run of the pre-change Release `hlmv_backend.exe`, using the
installed L4D2 `/bin`, exited with code 1 during
`CSteamApplication::Create`, before `CHLModelViewerApp::Create` or renderer
initialization. Log: `scratch/l4d2/legacy-backend/hlmv_debug.log`.

Reproduce the static inventory without loading game code:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/probe_l4d2.ps1 `
  -IncludeHashes -OutputPath scratch/l4d2/probe.json
```

The script auto-discovers Steam libraries, or accepts `-GameRoot`, and reads
`SOURCE_SDK_PUBLIC` / CMake cache settings, or accepts `-SdkPublic`. Its JSON
contains PE architectures, interface strings, optional SHA-256 hashes, Steam
build IDs and ordered search paths. `native_preview_verified` remains false:
this is an inventory, not a renderer test. Reports stay in ignored `scratch/`.

## Content mounting

The actual base `gameinfo.txt` mounts these paths in this order:

1. `update`
2. `left4dead2_dlc3`
3. `left4dead2_dlc2`
4. `left4dead2_dlc1`
5. `|gameinfo_path|.` (the base `left4dead2` directory)
6. `hl2`

Use the L4D2 filesystem to mount its gameinfo paths and archives, preserving
priority, localized content and addon behavior. An alphabetical recursive scan
or mounting only the base folder produces incorrect effective materials. Valve
documents VPK use for custom content in its
[L4D blog](https://www.l4d.com/blog/post.php?id=4771).

The prepared game-profile parser uses the existing KeyValues document parser
for quoted and unquoted keys, nested `FileSystem/SteamAppId`, and comments.
AppID 550 identifies an L4D2-based mod even if its folder and display name differ.
The existing discovery traversal may also list DLC/update gameinfo directories;
choose the principal `left4dead2` module for the full retail mount order.

Read-only archive inspection found VPK v1 packages in `update`, all three DLCs
and the base game. Useful real acceptance fixtures include:

| Source | Asset | Observed metadata |
| --- | --- | --- |
| Base | `materials/ads/burger_off.vmt` | `LightmappedGeneric`, `ads/burger.vtf` is VTF 7.4, 512 x 512 |
| DLC2 | `materials/models/deadbodies/common_male_head_01.vmt` | `VertexLitGeneric`, matching VTF 7.4, 512 x 512 |
| Base | `materials/models/infected/common/l4d2_test/common_male_suit_body.vmt` | `Infected`, matching VTF 7.4, 512 x 512 |
| Update | `materials/models/infected/common/common_infected_shared.vmt` | `Infected`, blood, burn-detail and wound-cutout parameters |
| Base | `models/c1_chargerexit/car_camjoint.mdl` | `IDST`, studio version 49 |

The existing SDK header uses studio version 48, so the model path also needs
explicit review. Some legacy model entries are version 44; a single sample is
not a complete format inventory. Local metadata report:
`scratch/l4d2/vpk-inventory.json`; no proprietary asset data is included in
tracked files.

## Backend implementation sequence

1. **Verify the ABI before rendering.** The isolated x86 factory probe is now
   implemented and has identified the provided versions. Next establish
   `IAppSystem`, `ICommandLine`, filesystem and cvar layouts against the installed
   runtime. Do not carry over the GMod offset tables or infer a command-line
   layout solely from the `CommandLine` export's name.
2. **Bring up the filesystem.** Initialize a coherent set of L4D2 DLLs and mount
   the original gameinfo without rewriting Steam files. Prove loose-file and
   VPK enumeration/read/prefix/exists operations through the existing IPC
   contracts. Compare precedence with the native Authoring Tools.
3. **Bring up the material system in isolation.** Establish the L4D2 material
   config layout and interfaces required by `Connect`, shader API selection,
   device creation and shutdown. Validate rendering context and mesh layouts
   before reusing `brush_render` or studio drawing. Keep all Valve-facing code
   private to this backend.
4. **Validate native materials and models.** Load a real VMT/VTF pair from a
   mounted archive and render a brush, then an L4D2-compiled preview model.
   Validate the studio header/cache/draw structures before loading game models;
   do not reuse SDK 2013 model assumptions without checking them.
5. **Integrate and gate features.** Select the dedicated executable in
   `EngineView`, preserve IPC query cancellation/readiness/shutdown semantics,
   and run editing, reload, export and reconnect through the frontend. Mark
   the game supported only after these end-to-end checks pass.

Public reference candidates include the legitimate Alien Swarm / Reactive Drop
SDK's [IAppSystem](https://github.com/ReactiveDrop/reactivedrop_public_src/blob/reactivedrop_beta/src/public/appframework/IAppSystem.h),
[ICommandLine](https://github.com/ReactiveDrop/reactivedrop_public_src/blob/reactivedrop_beta/src/public/tier0/icommandline.h),
[interfaces](https://github.com/ReactiveDrop/reactivedrop_public_src/blob/reactivedrop_beta/src/public/interfaces/interfaces.h),
[IMaterialSystem](https://github.com/ReactiveDrop/reactivedrop_public_src/blob/reactivedrop_beta/src/public/materialsystem/imaterialsystem.h)
and [material config](https://github.com/ReactiveDrop/reactivedrop_public_src/blob/reactivedrop_beta/src/public/materialsystem/materialsystem_config.h).
These are comparison material, not verified L4D2 ABI declarations. They do not
provide a ready-to-build HLMV backend. The older render headers in
[AlliedModders' L4D2 branch](https://github.com/alliedmodders/hl2sdk/tree/l4d2)
also require independent validation against the current runtime.

## Acceptance criteria before claiming native support

- Main game and AppID-550 mods are detected separately from SDK 2013 and GMod.
- DLLs are sourced from the selected install; incompatible runtimes fail with
  useful diagnostics and no version-name aliases.
- VPK reads resolve update/DLC/base conflicts in native search-path order.
- Real `LightmappedGeneric`, `VertexLitGeneric` and L4D2-specific `Infected`
  materials render using the L4D2 shader DLLs, with a native-tool comparison.
- Model/brush preview, texture lookup, edit/reload, cancellation, resize,
  reconnect and close work without crashes or leftover backend processes.
- The existing SDK 2013 backend still passes its tests and native smoke checks.
- A distributable build contains project binaries and required notices, with
  the game's proprietary runtime and assets supplied by the user's install.

The native-renderer gates above remain open. A passing CTest run only validates
the implemented detection and guard behavior; it does not establish L4D2
rendering compatibility.

## Verification of this preparation

- Win32 HLMV backend and x64 application compiled in Release and Debug.
- Eight CTest suites passed in each configuration, including nine new cases
  for profile detection, bounded interface inspection and startup diagnostics.
- Static inventory validated in PowerShell 7 and Windows PowerShell 5.1;
  optional hashes, missing paths and scan-size limits exercised.
- The real pre-change backend exit was reproduced. The prepared backend exits
  with code 2 and an explicit L4D2-backend requirement before command-line or
  app-system initialization; it does not attempt to render.
- Source/resource wiring and diff whitespace checked. Native L4D2 rendering,
  a graphical application session, packaging and a clean-machine install were
  not verified as part of this preparation.

The initial restricted test run could not replace temporary files. The full
suites passed with normal temporary-directory access. The x64 build also needed
normal MSBuild process access; builds used `/nodeReuse:false` and one worker.
Existing HLMV link warnings and the existing Qt deployment warning about
`VCINSTALLDIR` remain; this preparation did not change their owning code.
