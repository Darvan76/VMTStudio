# Game Manager design

## Goal
Centralize discovery, presentation and connection of installed Source Engine
games so users never have to manually browse for `<game>/bin` or the folder
containing `gameinfo.txt` unless they want to.

This builds on the existing Crowbar-style auto-detection (`ui::detect_bin_dir`,
`ui::detect_game_dir`) and turns it into a reusable catalogue.

## Core concepts

- `GameProfile` (in `game_profiles`): immutable-ish description of one game
  installation: `id`, display `name`, `game_dir`, `bin_dir`, `engine_branch`,
  `is_valid`.
- `GameManager` (in `game_profiles`): owns a vector of `GameProfile` and can
  `discover(root, depth)` by walking a filesystem root.
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

- A directory containing `gameinfo.txt` becomes a candidate `game_dir`.
- `bin` is resolved as `<game>/bin` when it contains `tier0.dll`, falling back
  to `<parent>/bin`.
- Engine branch is inferred from `gameinfo.txt` (`SteamAppId` and the `game`
  value), defaulting to `source_2013` for valid 32-bit installations and
  `gmod` for Garry's Mod.
- Duplicates by `game_dir` are suppressed.
- Recursion depth is bounded (default 4) so scanning a Steam library root is
  cheap.

## Non-goals (for now)

- No automatic Steam registry parsing yet; callers can pass library roots.
- No launching logic inside `GameManager`: connection/lifecycle stays in
  `bridge::EngineView` / `MainWindow`.
- No network or cloud game profiles.

## Testing plan

- Unit tests for `GameManager::discover` on a temp tree with:
  - valid game (`gameinfo.txt` + `../bin/tier0.dll`)
  - nested game inside a Steam library
  - invalid game missing `tier0.dll`
  - duplicate suppression
- Integration tests reuse `ui::test_connection` against detected `bin_dir`.

## Next steps

1. Add Steam library root detection (`libraryfolders.vdf`).
2. Load richer metadata from `game_profiles` JSON definitions (DLL set,
   interface versions).
3. Wire `GameManager` into `ConnectionDialog` and the Game menu.
4. Persist `last_game_id` in `UISettings` and auto-reconnect on startup.