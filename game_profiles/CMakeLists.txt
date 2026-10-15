add_library(game_profiles STATIC
    src/game_profiles.cpp
    src/game_manager.cpp
    src/install_check.cpp
    src/steam_library.cpp
)

target_include_directories(game_profiles PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(game_profiles
    PUBLIC vmt_core
    PRIVATE vmtviewer_warnings
)

# steam_library.cpp reads HKCU\Software\Valve\Steam via RegGetValueW (Advapi32).
if(WIN32)
    target_link_libraries(game_profiles PRIVATE advapi32)
endif()
