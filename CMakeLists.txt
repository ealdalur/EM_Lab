cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW) # MSVC runtime library selection via MSVC_RUNTIME_LIBRARY

project(EM_Lab
  VERSION 1.0.0
  DESCRIPTION "GPU FDTD (Yee lattice) electromagnetic wave simulator with volumetric raymarching"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ------------------------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------------------------
find_package(SDL3 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(Lua 5.4 REQUIRED)   # scene scripting (Lua 5.4 or newer)
find_package(imgui CONFIG REQUIRED)   # on-screen instrument panels (vcpkg: imgui[sdl3-binding,opengl3-binding])
find_package(implot CONFIG REQUIRED)  # plots (S11, radiation pattern)

# GLAD: OpenGL 4.6 Core loader (generated with glad 0.1.36, vendored in external/glad)
add_library(glad STATIC external/glad/src/glad.c)
target_include_directories(glad PUBLIC external/glad/include)
set_target_properties(glad PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ------------------------------------------------------------------------------
# Application
# ------------------------------------------------------------------------------
set(EMLAB_SOURCES
  src/Main.cpp
  src/Camera.cpp
  src/Shader.cpp
  src/SimulationGrid.cpp
  src/VolumeRenderer.cpp
  src/Environment.cpp
  src/FieldAnalyzer.cpp
  src/GpuSelection.cpp
  src/FarField.cpp
  src/Instruments.cpp
  src/ScriptEngine.cpp
  src/Ports.cpp
  src/Ui.cpp
  src/Radar.cpp
  src/InstrumentsUi.cpp
)

set(EMLAB_HEADERS
  src/GLUtils.hpp
  src/Camera.hpp
  src/Shader.hpp
  src/SimulationGrid.hpp
  src/VolumeRenderer.hpp
  src/Environment.hpp
  src/FieldAnalyzer.hpp
  src/GpuSelection.hpp
  src/FarField.hpp
  src/Instruments.hpp
  src/ScriptEngine.hpp
  src/Ports.hpp
  src/Ui.hpp
  src/Radar.hpp
)

set(EMLAB_SHADERS
  shaders/common.glsl
  shaders/update_E.comp
  shaders/update_B.comp
  shaders/apply_sources.comp
  shaders/clear_fields.comp
  shaders/poynting_flux.comp
  shaders/probe_gather.comp
  shaders/field_sampling.glsl
  shaders/ntff_common.glsl
  shaders/ntff_accumulate.comp
  shaders/ntff_pattern.comp
  shaders/ntff_reduce.comp
  shaders/port_accumulate.comp
  shaders/fullscreen.vert
  shaders/raymarch.frag
  shaders/lines.vert
  shaders/lines.frag
)

file(GLOB EMLAB_SCENES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scenes/*.lua ${CMAKE_CURRENT_SOURCE_DIR}/scenes/validation/*.lua)


add_executable(${PROJECT_NAME} ${EMLAB_SOURCES} ${EMLAB_HEADERS} ${EMLAB_SHADERS} ${EMLAB_SCENES})
source_group("Shaders" FILES ${EMLAB_SHADERS})
source_group("Scenes" FILES ${EMLAB_SCENES})

target_include_directories(${PROJECT_NAME} PRIVATE src ${LUA_INCLUDE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE
  glad
  SDL3::SDL3
  OpenGL::GL
  glm::glm
  imgui::imgui
  implot::implot
  ${LUA_LIBRARIES}
)

if(WIN32)
  # DXGI is only used to report which adapter is the high-performance one; the actual
  # discrete-GPU selection is done by symbols exported from the executable (GpuSelection.cpp).
  target_link_libraries(${PROJECT_NAME} PRIVATE dxgi)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
  GLM_FORCE_RADIANS
  # Fallback shader location so the executable also runs straight from the build tree / IDE.
  EMLAB_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders"
  EMLAB_SCENE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scenes"
)

if(MSVC)
  set_target_properties(${PROJECT_NAME} glad PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL") # match the vcpkg x64-windows libraries
  target_compile_options(${PROJECT_NAME} PRIVATE
    /W4 /permissive- /utf-8 /MP
    $<$<CONFIG:Release>:/O2 /fp:fast>
    $<$<CONFIG:Debug>:/Od>
  )
  target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME})
else()
  target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Debug>:-O0 -g>
  )
endif()

# Ship shaders and scene scripts next to the executable (searched there first,
# then in the source tree, so edits to scenes/*.lua hot-reload when run from an IDE).
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/shaders
          $<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/scenes
          $<TARGET_FILE_DIR:${PROJECT_NAME}>/scenes
  COMMENT "Copying shaders and scene scripts to output directory")
