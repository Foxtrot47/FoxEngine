option(BUILD_SHARED_LIBS "Build shared libraries" ON)

file(GLOB_RECURSE ENGINE_SOURCES "src/*.cpp")

add_library(FoxEngine STATIC ${ENGINE_SOURCES})

target_include_directories(FoxEngine
    PUBLIC  include
    PUBLIC  ${CMAKE_SOURCE_DIR}/ThirdParty/fsr3/include
    PRIVATE src
)

find_package(assimp CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(directxtex CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(Jolt CONFIG REQUIRED)
find_package(Lua REQUIRED)
find_package(sol2 CONFIG REQUIRED)
find_package(mikktspace CONFIG REQUIRED)
find_path(CGLTF_INCLUDE_DIR cgltf.h REQUIRED)

target_link_libraries(FoxEngine PUBLIC
    d3d11.lib
    dxgi.lib
    d3dcompiler.lib
    dxguid.lib
    assimp::assimp
    imgui::imgui
    Microsoft::DirectXTex
    nlohmann_json::nlohmann_json
    debug     ${CMAKE_SOURCE_DIR}/ThirdParty/fsr3/lib/ffx_fsr3upscaler_x64d.lib
    optimized ${CMAKE_SOURCE_DIR}/ThirdParty/fsr3/lib/ffx_fsr3upscaler_x64.lib
    debug     ${CMAKE_SOURCE_DIR}/ThirdParty/fsr3/lib/ffx_backend_dx11_x64d.lib
    optimized ${CMAKE_SOURCE_DIR}/ThirdParty/fsr3/lib/ffx_backend_dx11_x64.lib
)

target_link_libraries(FoxEngine PRIVATE zstd::libzstd)
# glTF import (cgltf, header-only) and MikkTSpace tangents stay private to Renderer/GltfImport.cpp.
target_include_directories(FoxEngine PRIVATE ${CGLTF_INCLUDE_DIR})
target_link_libraries(FoxEngine PRIVATE mikktspace::mikktspace)
# Jolt stays private: only Engine/src/Physics includes its headers.
target_link_libraries(FoxEngine PRIVATE Jolt::Jolt)
# Lua and sol2 stay private as well: only Engine/src/Scripting includes them.
target_include_directories(FoxEngine PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(FoxEngine PRIVATE ${LUA_LIBRARIES} sol2::sol2)
target_compile_definitions(FoxEngine PRIVATE SOL_ALL_SAFETIES_ON=1)
# vcpkg's Jolt is built with floating-point-exception support, but its
# exported config only defines that flag for Debug/Release; RegisterTypes()
# rejects feature-define mismatches, so define it for the other configs.
target_compile_definitions(FoxEngine PRIVATE
    FOX_ENGINE_ROOT="${CMAKE_SOURCE_DIR}"
    FOX_DEFAULT_PROJECT="${FOX_DEFAULT_PROJECT}"
)
target_compile_definitions(FoxEngine PRIVATE
    $<$<CONFIG:RelWithDebInfo,MinSizeRel>:JPH_FLOATING_POINT_EXCEPTIONS_ENABLED>
)
# The library is also built with AVX2; compile the files that include Jolt
# headers the same way so inlined math matches the library's code paths.
file(GLOB PHYSICS_SOURCES "src/Physics/*.cpp")
set_source_files_properties(${PHYSICS_SOURCES} PROPERTIES COMPILE_OPTIONS "/arch:AVX2")

target_compile_definitions(FoxEngine PUBLIC
    UNICODE
    _UNICODE
    $<$<CONFIG:Debug>:SE_DEBUG>
    $<$<CONFIG:Release>:SE_RELEASE>
    $<$<CONFIG:RelWithDebInfo>:SE_PROFILE>
)

target_compile_options(FoxEngine PRIVATE
    /W4
    /WX
    /MP
    /bigobj # sol2 binding units exceed the default COFF section limit
)
