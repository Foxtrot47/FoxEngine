cmake_minimum_required(VERSION 3.20)
project(FoxEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(Engine)
add_subdirectory(Game)
add_subdirectory(Editor)

include(CTest)
if(BUILD_TESTING)
    add_executable(LevelAssetTests Tests/LevelAssetTests.cpp)
    target_link_libraries(LevelAssetTests PRIVATE FoxEngine)
    target_compile_options(LevelAssetTests PRIVATE /W4 /WX /MP)
    add_test(NAME LevelAssetTests COMMAND LevelAssetTests)
    set_tests_properties(LevelAssetTests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    add_executable(EditorCameraTests Tests/EditorCameraTests.cpp Editor/src/EditorCamera.cpp)
    target_include_directories(EditorCameraTests PRIVATE Editor/src)
    target_link_libraries(EditorCameraTests PRIVATE FoxEngine)
    target_compile_options(EditorCameraTests PRIVATE /W4 /WX /MP)
    add_test(NAME EditorCameraTests COMMAND EditorCameraTests)
    set_tests_properties(EditorCameraTests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    add_executable(EditorUndoTests Tests/EditorUndoTests.cpp
        Editor/src/EditorEntityOps.cpp Editor/src/EditorUndo.cpp)
    target_include_directories(EditorUndoTests PRIVATE Editor/src)
    target_link_libraries(EditorUndoTests PRIVATE FoxEngine)
    target_compile_options(EditorUndoTests PRIVATE /W4 /WX /MP)
    add_test(NAME EditorUndoTests COMMAND EditorUndoTests)
    set_tests_properties(EditorUndoTests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
