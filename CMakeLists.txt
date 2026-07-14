cmake_minimum_required(VERSION 3.20)
project(FoxEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(Engine)
add_subdirectory(Game)

include(CTest)
if(BUILD_TESTING)
    add_executable(LevelAssetTests Tests/LevelAssetTests.cpp)
    target_link_libraries(LevelAssetTests PRIVATE FoxEngine)
    target_compile_options(LevelAssetTests PRIVATE /W4 /WX /MP)
    add_test(NAME LevelAssetTests COMMAND LevelAssetTests)
    set_tests_properties(LevelAssetTests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
