# the minimum version of CMake.
cmake_minimum_required(VERSION 3.5.0)
project(MyApplication10)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

# Package the versioned OHOS scene by default. Import a Mikan-exported scene
# explicitly with -DMIKAN_SCENE_FILE=<path>; never override local additions
# based on the presence of a workstation-specific project directory.
set(MIKAN_SCENE_FILE "" CACHE FILEPATH "Optional Mikan SceneSerializer v1 main.json")
set(_mikan_scene_fallback "${NATIVERENDER_ROOT_PATH}/application/scene/default_main.json")
if(MIKAN_SCENE_FILE)
    if(NOT EXISTS "${MIKAN_SCENE_FILE}")
        message(FATAL_ERROR "MIKAN_SCENE_FILE does not exist: ${MIKAN_SCENE_FILE}")
    endif()
    set(_mikan_scene_source "${MIKAN_SCENE_FILE}")
else()
    set(_mikan_scene_source "${_mikan_scene_fallback}")
endif()
if(NOT EXISTS "${_mikan_scene_source}")
    message(FATAL_ERROR "No Mikan scene source is available: ${_mikan_scene_source}")
endif()
set(_mikan_scene_package_dir "${NATIVERENDER_ROOT_PATH}/../resources/rawfile/scenes")
file(MAKE_DIRECTORY "${_mikan_scene_package_dir}")
configure_file("${_mikan_scene_source}" "${_mikan_scene_package_dir}/main.json" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_mikan_scene_source}")
message(STATUS "SDL3 scene source: ${_mikan_scene_source}")

add_subdirectory(../../../../.. external/sdlbin)

link_directories(${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
add_definitions(-DVK_USE_PLATFORM_OHOS=1)
include_directories(${NATIVERENDER_ROOT_PATH}
                    ${NATIVERENDER_ROOT_PATH}/include
                    ${SDL3_SOURCE_DIR}/include
                    ${SDL3_SOURCE_DIR}/src/video)

set(JOLT_ROOT ${NATIVERENDER_ROOT_PATH}/third_party/JoltPhysics)
file(GLOB_RECURSE JOLT_SOURCES ${JOLT_ROOT}/Jolt/*.cpp)

find_library(HILOG_LIBRARY NAMES hilog_ndk.z hilog)
if(NOT HILOG_LIBRARY)
    message(FATAL_ERROR "OpenHarmony hilog library was not found in the selected SDK")
endif()

add_library(entry SHARED
    application/app_main.cpp
    application/audio/audio_manager.cpp
    application/scene/scene_definition.cpp
    application/terrain/heightmap_terrain.cpp
    application/stb_image_impl.cpp
    application/model_loader.cpp
    application/touch_controller.cpp
    application/physics/jolt_gameplay_physics.cpp
    application/rhi/rhi.cpp
    application/rhi/rhi_gles.cpp
    application/vulkan_sdl.cpp
    ${JOLT_SOURCES})
add_dependencies(entry SDL3::SDL3-shared)
target_compile_definitions(entry PRIVATE
    JPH_DEBUG_RENDERER=0
    JPH_PROFILE_ENABLED=0
    JPH_DISABLE_TEMP_ALLOCATOR_NAMES=1
    JPH_USE_CUSTOM_ALLOCATOR=0)
target_include_directories(entry PRIVATE ${JOLT_ROOT})
# libGLESv3.so, not just libGLESv2.so: the OHOS NDK splits the entry points the
# way Android does, so every ES 3.0 call this backend uses (glGenVertexArrays,
# glBindBufferBase, glGetUniformBlockIndex, ...) only resolves against the v3
# library.  libGLESv2.so stays for the ES 2.0 subset, and libEGL.so is needed
# alongside them for the platform EGL pair.
target_link_libraries(entry PUBLIC libace_napi.z.so libSDL3.so libvulkan.so ${HILOG_LIBRARY}
                                  libGLESv3.so libGLESv2.so libEGL.so libohaudio.so libohsensor.so)
