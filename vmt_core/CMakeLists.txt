add_library(vmt_core STATIC
    src/vmt_core.cpp
    src/value.cpp
    src/document.cpp
    src/json.cpp
    src/schema.cpp
)

target_include_directories(vmt_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(vmt_core PRIVATE vmtviewer_warnings)
