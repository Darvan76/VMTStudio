add_library(engine_bridge STATIC
    src/engine_bridge.cpp
    src/pe.cpp
    src/dll_loader.cpp
)

target_include_directories(engine_bridge PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# engine_bridge is the ONLY module that will include Valve SDK headers, and only
# in its .cpp / private headers. Its public header is Valve-free.
target_link_libraries(engine_bridge
    PUBLIC vmt_core renderer
    PRIVATE vmtviewer_warnings
)

# ---- Phase 3 engine spike ---------------------------------------------------

# Build-time location of the Source SDK 2013 public headers. Override with
# -DSOURCE_SDK_PUBLIC=... if your SDK lives elsewhere.
set(SOURCE_SDK_PUBLIC "C:/Users/Dario/Documents/source-sdk-2013/source-sdk-2013/src/public"
    CACHE PATH "Source SDK 2013 src/public directory")

# The SDK-dependent translation units, isolated so the Valve headers (which are
# not /W4 /WX clean) compile with relaxed flags and don't leak into other TUs.
# engine_view.cpp is the Valve-free adapter implementing bridge::EngineView.
add_library(engine_spike_sdk STATIC
    src/engine_spike_engine.cpp
    src/engine_view.cpp
)

target_include_directories(engine_spike_sdk PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/sdk_compat"
    "${SOURCE_SDK_PUBLIC}"
    "${SOURCE_SDK_PUBLIC}/tier0"
    "${SOURCE_SDK_PUBLIC}/tier1"
    "${SOURCE_SDK_PUBLIC}/tier2"
)

target_link_libraries(engine_spike_sdk PRIVATE vmt_core)

target_compile_definitions(engine_spike_sdk PRIVATE
    COMPILER_MSVC
    COMPILER_MSVC32
    PLATFORM_WINDOWS
    PLATFORM_WINDOWS_PC
    WIN32
    _WIN32
)

target_compile_options(engine_spike_sdk PRIVATE
    /W0
    /permissive
)

# Standalone spike executable (Phase 3).
add_executable(engine_spike
    src/engine_spike_main.cpp
)

target_include_directories(engine_spike PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(engine_spike
    PRIVATE engine_bridge engine_spike_sdk vmtviewer_warnings
)
