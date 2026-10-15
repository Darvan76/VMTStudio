#include "bridge/ipc_dump.h"

// Scaffold: the real implementation lands with the IPC protocol v2 work.
// Returning a clean error keeps the build green while the parser is written.

namespace bridge {
namespace {

vmt::Error not_implemented(const char* what) {
    return vmt::make_error(vmt::ErrorCode::Internal,
                           std::string("ipc_dump: ") + what + " is not implemented yet",
                           "see docs/IPC_PROTOCOL.md");
}

} // namespace

int dump_protocol_version(const std::string&) {
    return 0;
}

vmt::Result<ModelList> parse_models_dump(const std::string&) {
    return vmt::Err<ModelList>(not_implemented("parse_models_dump"));
}

vmt::Result<ModelScene> parse_model_scene_dump(const std::string&) {
    return vmt::Err<ModelScene>(not_implemented("parse_model_scene_dump"));
}

vmt::Result<AnimState> parse_anim_state_dump(const std::string&) {
    return vmt::Err<AnimState>(not_implemented("parse_anim_state_dump"));
}

} // namespace bridge
