# VMT-Studio IPC protocol

The 64-bit Qt frontend (`vmtstudio.exe`) drives the 32-bit native renderer
(`hlmv_backend.exe`) over Win32 messages. This document is the contract between
the two processes: `hlmv_backend/src/backend_ipc.cpp` implements it, and
`engine_bridge/src/engine_view.cpp` consumes it. The text parsing lives in
`engine_bridge/include/bridge/ipc_dump.h` (`engine_bridge/src/ipc_dump.cpp`) so
it can be unit tested without Windows or an engine connection.

## Channel

* UI -> backend: `WM_COPYDATA` (id 0) to the backend window. The payload is a
  UTF-8 command line **without** a trailing NUL. The message returns `1` when
  the backend accepted the command and `0` otherwise.
* The host window handle travels in `wParam` (already the case today).
* Backend -> UI: one-way notifications posted to the host HWND (see *Live
  tick*). Data queries never travel in messages; they use response files.

## Command grammar (v1)

```
PING
MOUNT_DIR <directory>
SET_MATERIAL <game-relative material name>      # empty name clears the override
RELOAD_MATERIAL [<game-relative material name>] # no argument: the current override
LOAD_MODEL <absolute path or game-relative .mdl>
SET_SHAPE <sphere|cube|cylinder|plane|brush_wall|brush_floor|brush_cube|brush_displacement>
SET_RENDER_MODE <shaded|wireframe|albedo|normals>
SET_ANIMATION <play|pause|toggle>
SET_SPEED <scale>
SET_LIGHTING <18 floats>
SET_CAM <pitch> <yaw> <dist> <tx> <ty> <tz>
RESIZE <w> <h>
QUIT
```

`SET_MATERIAL` forces one material onto everything drawn (the classic material
"ball" preview). When the UI is editing a material that belongs to the loaded
model it should clear the override (`SET_MATERIAL` with an empty name) and rely
on the loose override + `RELOAD_MATERIAL <name>` instead, so the engine paints
the model with its own materials and only the edited slot changes.

## Command grammar (v2, model viewer)

```
SET_SEQUENCE <index>
SET_SEQUENCE_LABEL <label>            # lookup by sequence label
SET_FRAME <frame>                     # 0-based
STEP_FRAME <delta>                    # signed
SET_LOOP <0|1>                        # force/forbid wrapping at the sequence end
SET_STOP                              # pause and rewind to frame 0
SET_SPEED <scale>                     # clamped to 0.01 .. 8.0
SET_BLEND_TIME <seconds>
BLEND_NOW
SET_OVERLAY <layer> <sequence> <weight>   # layer 1..4; sequence -1 clears the layer
SET_POSEPARAM <index> <value>
SET_BODYGROUP <group index> <value>
SET_SKIN <index>
SET_VIEWFLAG <flag> <0|1>             # see the flag list below
```

View flags: `bones`, `hitboxes`, `attachments`, `ground`, `movement`,
`overlay_wireframe`, `normals`, `tangents`, `shadow`, `sequence_boxes`.

Unknown verbs and malformed arguments must return `0` and leave the backend
untouched. Every accepted command ends with a redraw.

## Data queries (v2): response files

The UI sends a dump request and the backend writes the answer to a file. This
keeps the message handler fast (the `WM_COPYDATA` round trip has a 1 s timeout)
and lets the UI wait without blocking its own message loop.

```
DUMP <kind> <token> [args]      # kind: assets | model_scene | exists
RAW <token> <game-relative path>  # binary payload (VPK-aware)
RAW_PREFIX <token> <max-bytes> <game-relative path> # bounded prefix + original size
```

* `token` matches `[A-Za-z0-9_-]{1,64}`; the UI generates it (job id).
* The backend is launched with `-ipc-dir <dir>` (the UI passes
  `%TEMP%\VMTStudio\ipc`). Unknown directory: the backend creates it.
* Response files, all inside `<ipc_dir>`:
  * `<token>.dat` - UTF-8 text result (schema below), written atomically
    (`<token>.part` then rename).
  * `<token>.bin` - binary result for `RAW` or `RAW_PREFIX`, written the same way.
  * `<token>.done` - empty marker created **after** the payload; the UI treats
    its presence as "result complete".
  * `<token>.err` - UTF-8 error message; mutually exclusive with `.done`.
* The backend deletes stale `<token>.*` response files before starting a job,
  preserving `<token>.cancel`.
* The UI gives up after 10 s (timeout) and reports an error. `assets` is the
  exception: a whole-game index legitimately takes longer, so the UI waits 60 s
  for it.
* Native directory notifications wake response waits when files change. A 5 ms
  timeout also checks cancellation, process exit and the job deadline; polling
  remains the fallback if notifications are unavailable. Error text is published
  through `.part` followed by an atomic rename to `.err`.
* Cancellation creates `<token>.cancel` and releases the frontend waiter. The
  backend preserves that marker when accepting a queued request, rejects a job
  cancelled before execution, and checks it during model/asset scans and output
  construction at intervals of at most 5 ms between engine calls. RAW checks it
  before execution and after its bounded read. Cancellation produces `.err` with
  `query cancelled`; it never publishes a successful partial listing.
* The frontend retains cancelled tokens until completion: subsequent requests
  reap acknowledged responses, and teardown removes leftovers after the backend
  exits, including `.part` and `.cancel`. A Windows error response is closed
  before removal so the frontend does not keep its own response locked.
* Clearing an engine connection cancels active waits before draining workers;
  startup checks the owner's shared cancellation flag. An export may cancel its
  own text read without cancelling unrelated queries. The synchronous
  `WM_COPYDATA` acknowledgement remains bounded to 1 s. Teardown services sent
  native messages while waiting for the embedded child and allows 100 ms for
  normal DLL cleanup before the existing forced-exit fallback. The renderer owns
  no edited documents; frontend writes and workers are finished first.

`RAW_PREFIX <token> <max-bytes> <game-relative path>` opens the mounted game
file (loose or VPK) and reads only its first `min(file-size, max-bytes)` bytes.
The decimal limit must be between 1 and 4096. Absolute paths, `..` components
and control characters are rejected. Its `.bin` response is:

```
uint64 little-endian original-file-size
exactly min(original-file-size, max-bytes) payload bytes
```

The frontend rejects malformed or oversized responses before decoding them.
Prefix reads may inspect metadata of files larger than the existing 256 MiB
full-`RAW` cap; they never allocate or transfer the full file. Without a running
backend the bridge only provides bounded reads of loose files in the configured
game folder; it does not emulate VPK access or fall back to a full read.

`DUMP assets <roots> <extensions>`: every file reachable through the engine
file system whose extension is one of `extensions`, under any of `roots`:

```
DUMP assets <token> materials;models .vmt;.vtf;.mdl
```

* `roots` and `extensions` are `;`-separated lists; the first space in the
  argument separates the two lists.
* Roots are game-relative, with forward slashes and no trailing slash
  (`materials`, `models/props_c17`).
* Extensions include the dot and are folded to lower case by the backend
  (`.vmt;.vtf;.mdl`); a missing leading dot is accepted.
* Empty elements are ignored. Missing roots or extensions fail the job through
  `<token>.err`.
* Paths are deduplicated case-insensitively; the first spelling seen is the one
  reported. Every row is resolved to `loose` or `vpk` with the engine file
  system, so a loose copy and a VPK entry for the same path produce one row.
* The scan stops after `150000` unique rows, `1200000` visited entries, or a
  clock budget of about 20 s per root (with a global scan cap that keeps the
  answer inside the 60 s query timeout). A cut scan is marked with
  `note\ttruncated`; the rows already collected remain valid.

`DUMP model_scene` (no arguments): everything the UI needs about the loaded
model in one round trip.

`DUMP exists <token> <game-relative path>`: checks `IFileSystem::FileExists`
against the mounted `GAME` search paths, including VPK entries and loose
overrides. The path keeps its extension and may contain spaces; absolute paths,
`..` components and control characters are rejected. This query does not open
or copy the file payload. An unavailable file system produces `<token>.err`,
not an `exists=0` result. Older backends reject this new dump kind through the
same error response, allowing the UI to report lookup availability accurately.

### `.dat` text schema

```
VMTStudio-IPC\t2
kind\t<kind>
<row>...
```

* Lines end with `LF`; encoding is UTF-8.
* Fields are separated by a single `TAB`. Field values coming from the engine
  must have `TAB`, `LF` and `CR` replaced by a space.
* Rows are `field<TAB>...`; unknown `field`s must be ignored by the parser so
  the backend can add information without breaking older UIs.

`kind=assets`:

```
asset\t<game-relative path, extension included>\t<loose|vpk>
note\ttruncated
```

* One row per unique path (case-insensitive dedup in the backend), sorted by
  the lower-case path.
* `note\ttruncated` is written when the asset scan hit the row cap, the entry
  cap or the clock budget; the rows before it are still valid.
* The dump carries no size: loose sizes come from `std::filesystem` in the UI
  and VPK sizes are known through `RAW_PREFIX` or a full `RAW` read.

`kind=model_scene`:

```
info\tname\t<game-relative .mdl path>
stats\tpolys\tbatches\tbones\tlods\tmaterials\tminx\tminy\tminz\tmaxx\tmaxy\tmaxz
material\t<texture index>\t<game-relative material name, no extension>\t<shader or empty>
use\t<texture index>\t<bodypart index>\t<submodel index>\t<bodypart name>
sequence\t<index>\t<label>\t<activity or empty>\t<fps>\t<frames>\t<duration seconds>\t<looping 0|1>\t<hidden 0|1>
bodypart\t<index>\t<name>\t<submodel count>\t<current submodel>
skin\t<index>\t<name>
bone\t<index>\t<name>\t<parent index, -1 for the root>
attachment\t<index>\t<name>
pose\t<index>\t<name>\t<min value>\t<max value>
```

`material` rows are indexed by the model's texture table
(`studiohdr_t::textureindex`), which is what `use` rows reference. Skin
families remap texture indices per skin; the dump reports the material table of
the current skin.

`kind=exists`:

```
exists\t<found 0|1>
```

Exactly one `exists` row is required. `0` confirms that the engine file system
could not find the file; a missing or malformed result is a protocol error and
must never be interpreted as a missing texture by validation.

## Live tick (backend -> UI)

While a model is loaded the backend posts `WM_USER+422` to the host HWND every
rendered frame:

* `wParam`: current frame (integer, rounded).
* `lParam`: `round(cycle * 10000)` (so 0..10000).

The Qt viewport catches it in `nativeEvent()` and re-emits it as a signal. The
frontend may ignore ticks entirely (they are best-effort and carry no
guarantee), for example when the animation dock is closed.

## Versioning

The header line of every `.dat` file carries the protocol version (`2`). The UI
refuses to parse a higher version and reports "backend too new"; the backend
reports `DUMP` for an unknown kind through `<token>.err`.

The backend also answers `WM_USER+420` (fast ping) with `1`; the UI uses it to
detect that the embedded window is alive and ready.
