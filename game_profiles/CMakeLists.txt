add_library(game_profiles STATIC
    src/game_profiles.cpp
)

target_include_directories(game_profiles PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(game_profiles
    PUBLIC vmt_core
    PRIVATE vmtviewer_warnings
)
