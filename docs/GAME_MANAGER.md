# Game Manager design

## Goal
Centralize discovery, presentation and connection of installed Source Engine
games so users never have to manually browse for `<game>/bin` or the folder
containing `gameinfo.txt` unless they want to.

This owns the Crowbar-style auto-detection: the static helpers
`GameManager::detect_bin_dir`, `GameManager::detect_game_dir` and
`GameManager::looks_like_bin_dir` are the single source of truth, and the UI
wrappers (`ui::detect_bin_dir`, `ui::detect_game_dir`) delegate to them so the
connection dialog and the catalogue scan behave identically.

## Core concepts

- `GameProfile` (in `game_profiles`): immutable-ish description of one game
  installation: `id`, display `name`, `game_dir`, `bin_dir`, `engine_branch`,
  `is_valid`. It also carries an `InstallReport install` (see *Install
  validation* below) that says **why** the install is (not) usable.
- `GameManager` (in `game_profiles`): owns a vector of `GameProfile` and can
  `discover(root, depth)` by walking a filesystem root. It also exposes the
  filesystem detection helpers as public static methods:
  - `looks_like_bin_dir(path)`: true when the folder contains both
    `tier0.dll` and `vstdlib.dll` (the minimal marker set of a Source `/bin`).
  - `detect_bin_dir(selected)`: resolves a Source `/bin` folder from a direct
    `/bin`, a mod folder (gameinfo.txt), a game/SDK root or a Steam library
    root (immediate-children scan, sorted, first match).
  - `detect_game_dir(bin_dir)`: resolves the folder with `gameinfo.txt` from a
    `/bin` folder: the parent when it directly contains one, otherwise the
    first immediate subdirectory (sorted) that does.
- `ConnectionDialog` (UI): consumes `GameManager` to populate a combo box of
  discovered games; selecting one fills the bin/game fields and runs
  `ui::test_connection`.
- `UISettings`: stores `last_game_id` in addition to the existing paths.

## Data flow

1. On startup, `MainWindow` creates a `GameManager`, points it at the last
   known root (or a set of Steam library roots) and calls `discover`.
2. The resulting profiles feed:
   - Game menu (recent + discovered)
   - Connection dialog's "Known games" combo
   - Material browser roots (optional future work)
3. Selecting a profile sets `last_bin_dir` / `last_game_dir`, saves settings
   and (re)launches the embedded `hlmv_backend`.

## Discovery rules

- A directory containing `gameinfo.txt` becomes a candidate `game_dir`
  (`GameManager::detect_game_dir`).
- `bin` is resolved by `GameManager::detect_bin_dir` and requires **both**
  `tier0.dll` and `vstdlib.dll` inside the folder; the scan order is: the
  selected folder itself (when it already is a `/bin`), `<mod>/bin`, then
  `<parent>/bin` for mod folders, `<root>/bin` for game/SDK roots, and finally
  the immediate children of the selected folder (Steam library roots), sorted,
  first match wins.
- Engine branch is inferred from `gameinfo.txt` (`SteamAppId` and the `game`
  value), defaulting to `source_2013` for valid 32-bit installations and
  `gmod` for Garry's Mod.
- Duplicates by `game_dir` are suppressed.
- Recursion depth is bounded (default 4) so scanning a Steam library root is
  cheap.

## Install validation

Every discovered profile is run through `profiles::check_install(profile)`
(`game_profiles/include/profiles/install_check.h`), a read-only, best-effort,
never-throwing check of the real files on disk. Its result is stored in
`GameProfile::install` and `GameProfile::is_valid` is set to
`install.usable`, so "valid" now means "the game can actually be driven by
this application", not merely "a bin folder exists".

`InstallReport` fields:

- `usable`: true only when `problem == InstallProblem::None`.
- `problem`: one of the failure categories below.
- `detail`: human-readable, actionable sentence naming the offending path
  (and DLLs) and the suggested fix; shown in the UI.
- `missing_dlls`: the required DLLs that are absent (filled for `MissingDll`).

Failure taxonomy (`InstallProblem`, checked in this order):

| Problem | Meaning | Typical real-world cause |
|---|---|---|
| `None` | All checks passed; `usable == true`. | — |
| `EmptyFolder` | Game folder missing or contains no files. | Install moved/uninstalled; stale library entry. |
| `NoGameInfo` | Folder has content but no `gameinfo.txt`. | User picked the wrong folder (not a Source mod). |
| `NoBin` | No `/bin` with engine DLLs in or near the folder. | Incomplete download / dedicated-server-only install. |
| `MissingDll` | `/bin` exists but required DLLs are absent (`missing_dlls` lists them). | Partial/corrupted install; verify files in Steam. |
| `WrongArch` | DLLs exist but are not 32-bit x86 PE images (or are corrupt). | 64-bit engine builds; this app drives the 32-bit `hlmv_backend`. |
| `NoExports` | DLLs present with the right arch but lack the expected exports (`CreateInterface` / `CommandLine_Tier0`). | Unsupported engine build; interface layout changed. |

Required DLL set (same order/names as `ui::test_connection`):
`tier0.dll`, `vstdlib.dll`, `filesystem_stdio.dll`, `materialsystem.dll`,
`shaderapidx9.dll`.

Details of the check, in order:

1. `game_dir` exists and is non-empty, else `EmptyFolder`.
2. `gameinfo.txt` exists, else `NoGameInfo`.
3. A bin folder is resolved: the profile's own `bin_dir`, then
   `GameManager::detect_bin_dir`, then a plain `<game>/bin` (or `<parent>/bin`)
   even if it lacks the marker DLLs — so the report can name what is missing.
   Nothing resolves → `NoBin`.
4. All required DLLs exist, else `MissingDll`.
5. Every DLL is a valid PE image and 32-bit (x86), else `WrongArch`
   (corrupt/non-PE files also land here).
6. Best-effort export check: a DLL whose export table clearly lacks the
   expected export is flagged `NoExports`; an unreadable/unparsable export
   table is skipped rather than failed.

`profiles::to_string(problem)` returns a short tag ("empty folder",
"missing DLLs", ...) used by the connection dialog, which renders unusable
profiles as `Name (invalid: <tag>)` in the "Known games" combo.

## Non-goals (for now)

- No automatic Steam registry parsing yet; callers can pass library roots.
- No launching logic inside `GameManager`: connection/lifecycle stays in
  `bridge::EngineView` / `MainWindow`.
- No network or cloud game profiles.

## Testing plan

- Unit tests for `GameManager::discover` on a temp tree with:
  - valid game (`gameinfo.txt` + `../bin/tier0.dll` + `../bin/vstdlib.dll`)
  - nested game inside a Steam library
  - invalid game missing `tier0.dll` / `vstdlib.dll`
  - duplicate suppression
- Unit tests for the static helpers through the `ui::detect_bin_dir` /
  `ui::detect_game_dir` wrappers (mods, game roots, Steam libraries,
  tier0-only folders are rejected).
- `install_check_tests.cpp` builds the fixtures from synthesized minimal PE
  images (`tests/framework/pe_stub.h`) and covers the whole taxonomy: empty
  folder, missing `gameinfo.txt`, no bin, missing required DLLs, 64-bit /
  corrupt DLLs, missing exports, a fully valid install, and the
  `GameManager::scan` wiring (`is_valid` == `install.usable`).
- Integration tests reuse `ui::test_connection` against detected `bin_dir`.

## Next steps

1. Add Steam library root detection (`libraryfolders.vdf`).
2. Load richer metadata from `game_profiles` JSON definitions (DLL set,
   interface versions).
3. Wire `GameManager` into `ConnectionDialog` and the Game menu.
4. Persist `last_game_id` in `UISettings` and auto-reconnect on startup.