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

## Command grammar (v1, unchanged)

```
PING
MOUNT_DIR <directory>
SET_MATERIAL <game-relative material name>      # empty name clears the override
RELOAD_MATERIAL [<game-relative material name>] # no argument: the current override
LOAD_MODEL <absolute path or game-relative .mdl>
SET_SHAPE <sphere|cube|cylinder|plane|brush_wall|brush_floor|brush_cube|brush_displacement>
CAPTURE_THUMBNAIL <material|-> <size> <output file>
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
DUMP <kind> <token> [args]      # kind: models | model_scene | anim_state
RAW <token> <game-relative path>  # binary payload (VPK-aware)
```

* `token` matches `[A-Za-z0-9_-]{1,64}`; the UI generates it (job id).
* The backend is launched with `-ipc-dir <dir>` (the UI passes
  `%TEMP%\VMTStudio\ipc`). Unknown directory: the backend creates it.
* Response files, all inside `<ipc_dir>`:
  * `<token>.dat` - UTF-8 text result (schema below), written atomically
    (`<token>.part` then rename).
  * `<token>.bin` - raw bytes, only for `RAW`, written the same way.
  * `<token>.done` - empty marker created **after** the payload; the UI treats
    its presence as "result complete".
  * `<token>.err` - UTF-8 error message; mutually exclusive with `.done`.
* The backend deletes stale `<token>.*` files before starting a job.
* The UI gives up after 10 s (timeout) and reports an error.

`DUMP models [prefix]`: every `*.mdl` reachable through the engine file system
(loose folders and VPKs), including VPK-packed content. `prefix` is a
game-relative path prefix such as `models/props_c17/`.

`DUMP model_scene` (no arguments): everything the UI needs about the loaded
model in one round trip.

`DUMP anim_state`: the playback state of the loaded model.

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

`kind=models`:

```
model\t<game-relative path ending in .mdl>\t<loose|vpk>
```

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

`kind=anim_state`:

```
state\t<sequence index>\t<frame>\t<cycle 0..1>\t<speed scale>\t<ground speed>\t<paused 0|1>
```

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
