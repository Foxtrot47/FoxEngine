file(GLOB_RECURSE EDITOR_SOURCES "src/*.cpp")

add_executable(Editor WIN32 ${EDITOR_SOURCES})

find_package(imguizmo CONFIG REQUIRED)

target_link_libraries(Editor PRIVATE FoxEngine imguizmo::imguizmo shell32)

target_compile_definitions(Editor PRIVATE
    UNICODE
    _UNICODE
)

target_compile_options(Editor PRIVATE
    /W4
    /WX
    /MP
)

set_target_properties(Editor PROPERTIES
    OUTPUT_NAME "FoxEditor"
    VS_DEBUGGER_WORKING_DIRECTORY "${FOX_DEFAULT_PROJECT}"
)
