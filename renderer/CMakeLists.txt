add_library(renderer STATIC
    src/renderer.cpp
)

target_include_directories(renderer PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(renderer
    PUBLIC vmt_core
    PRIVATE vmtviewer_warnings
)
