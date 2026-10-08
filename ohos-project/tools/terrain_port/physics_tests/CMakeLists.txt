cmake_minimum_required(VERSION 3.20)
project(character_gravity_tests LANGUAGES CXX)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(APP "${ROOT}/ohos-project/entry/src/main/cpp/application")
set(JOLT "${APP}/../third_party/JoltPhysics")
file(GLOB_RECURSE JOLT_SOURCES "${JOLT}/Jolt/*.cpp")
add_executable(physics_tests ../physics_tests.cpp
    "${APP}/physics/jolt_gameplay_physics.cpp"
    "${APP}/terrain/heightmap_terrain.cpp"
    "${APP}/scene/scene_definition.cpp" ${JOLT_SOURCES})
target_compile_features(physics_tests PRIVATE cxx_std_17)
target_include_directories(physics_tests PRIVATE "${ROOT}/include" "${ROOT}/src/video" "${APP}" "${JOLT}")
target_compile_definitions(physics_tests PRIVATE JPH_DEBUG_RENDERER=0 JPH_PROFILE_ENABLED=0
    JPH_DISABLE_TEMP_ALLOCATOR_NAMES=1 JPH_USE_CUSTOM_ALLOCATOR=0)
