#pragma once

// renderer - preview scene: primitive meshes, camera, lights, and drawing
// through the engine material system.
//
// Phase 1 skeleton: placeholder public header. Renderer never includes Valve
// headers; it talks to engine_bridge through its own abstractions.

namespace render {

// The concrete declarations arrive in a later phase. For now the module exposes
// a single symbol so the static library has a well-defined translation unit.
const char* renderer_version_string() noexcept;

} // namespace render
