file(GLOB FOXPROJECT_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

add_executable(FoxProject ${FOXPROJECT_SOURCES})

target_link_libraries(FoxProject PRIVATE FoxEngine)

# `FoxProject publish` ships the generic player built for the same configuration.
add_dependencies(FoxProject Player)

target_compile_definitions(FoxProject PRIVATE
    UNICODE
    _UNICODE
    FOX_PLAYER_EXE="$<TARGET_FILE:Player>"
)

target_compile_options(FoxProject PRIVATE
    /W4
    /WX
    /MP
)

set_target_properties(FoxProject PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
