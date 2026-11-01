add_executable(FoxCook src/main.cpp)

target_link_libraries(FoxCook PRIVATE FoxEngine)

target_compile_definitions(FoxCook PRIVATE
    UNICODE
    _UNICODE
)

target_compile_options(FoxCook PRIVATE
    /W4
    /WX
    /MP
)

set_target_properties(FoxCook PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
