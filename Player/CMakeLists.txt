file(GLOB_RECURSE PLAYER_SOURCES "src/*.cpp")

# The generic game player: reads project.json and runs the project's scripts.
# `FoxProject publish` copies it next to the game content as <ProjectName>.exe.
add_executable(Player WIN32 ${PLAYER_SOURCES})

target_link_libraries(Player PRIVATE FoxEngine shell32)

target_compile_definitions(Player PRIVATE
    UNICODE
    _UNICODE
)

target_compile_options(Player PRIVATE
    /W4
    /WX
    /MP
)

set_target_properties(Player PROPERTIES
    OUTPUT_NAME "FoxPlayer"
    VS_DEBUGGER_WORKING_DIRECTORY "${FOX_DEFAULT_PROJECT}"
)
