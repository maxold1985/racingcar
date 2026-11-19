cmake_minimum_required(VERSION 3.20)
project(LuauRaycastVehicle VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(RV_WITH_LUAU "Build Luau scripting demo (downloads Luau unless LUAU_SOURCE_DIR set)" ON)
option(RV_WITH_GRAPHICS "Build interactive raylib 3D racing game (requires RV_WITH_LUAU)" OFF)
if(RV_WITH_GRAPHICS AND NOT RV_WITH_LUAU)
    message(FATAL_ERROR "RV_WITH_GRAPHICS requires RV_WITH_LUAU=ON")
endif()

add_library(raycast_vehicle SHARED src/rv_physics.cpp)
target_include_directories(raycast_vehicle PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(raycast_vehicle PRIVATE RV_BUILD_DLL)
set_target_properties(raycast_vehicle PROPERTIES OUTPUT_NAME "raycast_vehicle")

add_library(raycast_vehicle_static STATIC src/rv_physics.cpp)
target_include_directories(raycast_vehicle_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(NOT MSVC)
    target_link_libraries(raycast_vehicle PRIVATE m)
    target_link_libraries(raycast_vehicle_static PUBLIC m)
endif()

enable_testing()
add_executable(rv_tests tests/test_vehicle.c)
target_link_libraries(rv_tests PRIVATE raycast_vehicle_static)
add_test(NAME vehicle_core COMMAND rv_tests)
add_executable(rv_vector_tests tests/test_vec3.cpp)
target_link_libraries(rv_vector_tests PRIVATE raycast_vehicle_static)
add_test(NAME vector_operators COMMAND rv_vector_tests)

if(RV_WITH_LUAU)
    set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
    set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
    include(FetchContent)
    if(LUAU_SOURCE_DIR)
        FetchContent_Declare(luau SOURCE_DIR "${LUAU_SOURCE_DIR}")
    else()
        FetchContent_Declare(luau GIT_REPOSITORY https://github.com/luau-lang/luau.git GIT_TAG master GIT_SHALLOW TRUE)
    endif()
    FetchContent_MakeAvailable(luau)
    add_executable(raycast_luau_demo demo/main.cpp src/rv_luau.cpp)
    target_include_directories(raycast_luau_demo PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${luau_SOURCE_DIR}/VM/include
        ${luau_SOURCE_DIR}/Compiler/include)
    target_link_libraries(raycast_luau_demo PRIVATE raycast_vehicle_static Luau.VM Luau.Compiler)
    target_compile_definitions(raycast_luau_demo PRIVATE RV_DEFAULT_SCRIPT_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/scripts/race.luau\")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/scripts" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
endif()

if(RV_WITH_GRAPHICS)
    # raylib 5.5 uses OpenGL 3.3 on desktop by default.
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    include(FetchContent)
    if(RAYLIB_SOURCE_DIR)
        FetchContent_Declare(raylib SOURCE_DIR "${RAYLIB_SOURCE_DIR}")
    else()
        FetchContent_Declare(raylib
            GIT_REPOSITORY https://github.com/raysan5/raylib.git
            GIT_TAG 5.5
            GIT_SHALLOW TRUE)
    endif()
    FetchContent_MakeAvailable(raylib)
    add_executable(racingcar_game graphics/main.cpp graphics/rv_model.cpp src/rv_luau.cpp)
    target_include_directories(racingcar_game PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/graphics"
        "${luau_SOURCE_DIR}/VM/include"
        "${luau_SOURCE_DIR}/Compiler/include")
    target_link_libraries(racingcar_game PRIVATE raycast_vehicle_static raylib Luau.VM Luau.Compiler)
    target_compile_definitions(racingcar_game PRIVATE
        RV_DEFAULT_GAME_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/scripts/racing_game.luau")
endif()
