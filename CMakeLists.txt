cmake_minimum_required(VERSION 3.25)

project(vw VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(Vulkan REQUIRED)
find_package(nlohmann_json REQUIRED)
find_path(PAM_INCLUDE_DIR security/pam_appl.h REQUIRED)
find_library(PAM_LIBRARY pam REQUIRED)
find_path(QUICKJS_INCLUDE_DIR quickjs.h PATH_SUFFIXES quickjs REQUIRED)
find_library(QUICKJS_LIBRARY quickjs PATH_SUFFIXES quickjs REQUIRED)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(GIO_UNIX REQUIRED IMPORTED_TARGET gio-unix-2.0)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(RSVG REQUIRED IMPORTED_TARGET librsvg-2.0)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp)
find_path(STB_INCLUDE_DIR stb_image.h PATH_SUFFIXES stb REQUIRED)
find_program(GLSLC glslc REQUIRED)

set(ZWAYLAND_ROOT "$ENV{ZWAYLAND_ROOT}" CACHE PATH
    "Root containing zwayland-scanner, zwayland, and zwwm-protocols")
if(NOT ZWAYLAND_ROOT)
  message(FATAL_ERROR "Set ZWAYLAND_ROOT or run CMake inside nix develop")
endif()
foreach(component zwayland-scanner zwayland zwwm-protocols)
  if(NOT EXISTS "${ZWAYLAND_ROOT}/${component}/CMakeLists.txt")
    message(FATAL_ERROR "ZWAYLAND_ROOT is missing ${component}: ${ZWAYLAND_ROOT}")
  endif()
endforeach()

# libvw is shared, so statically built dependencies must be PIC.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Disable dependency tests without clobbering this project's BUILD_TESTING.
set(VW_BUILD_TESTING ${BUILD_TESTING})
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_TESTING OFF)
add_subdirectory("${ZWAYLAND_ROOT}/zwayland-scanner" "deps/zwayland-scanner")
add_subdirectory("${ZWAYLAND_ROOT}/zwayland" "deps/zwayland")
add_subdirectory("${ZWAYLAND_ROOT}/zwwm-protocols" "deps/zwwm-protocols")
set(BUILD_TESTING ${VW_BUILD_TESTING})

set(VW_WLR_LAYER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/wlr-layer-shell-unstable-v1-zwayland-client.h")
add_custom_command(
  OUTPUT "${VW_WLR_LAYER_HEADER}"
  COMMAND "$<TARGET_FILE:zwayland-scanner>" zwayland-client-header
          "${ZWAYLAND_ROOT}/zwwm-protocols/protocols/wlr-layer-shell-unstable-v1.xml"
          "${VW_WLR_LAYER_HEADER}"
  DEPENDS zwayland-scanner
          "${ZWAYLAND_ROOT}/zwwm-protocols/protocols/wlr-layer-shell-unstable-v1.xml"
  VERBATIM)
add_custom_target(vw-protocol-headers DEPENDS "${VW_WLR_LAYER_HEADER}")

# vw-shaderc compiles GLSL into .vws, vw's shader format (src/vws.hpp). It is
# a host tool with no dependencies, exported as vw::shaderc.
add_executable(vw-shaderc tools/vw-shaderc.cpp src/vws.cpp)
add_executable(vw::shaderc ALIAS vw-shaderc)
set_target_properties(vw-shaderc PROPERTIES EXPORT_NAME shaderc)
target_compile_features(vw-shaderc PRIVATE cxx_std_23)
target_compile_options(vw-shaderc PRIVATE -Wall -Wextra -Wpedantic)
# Where `#include <vw/shader.glsl>` resolves: the source tree, then the install.
target_compile_definitions(vw-shaderc PRIVATE
  VW_SHADER_INCLUDE_DIRS="${CMAKE_CURRENT_SOURCE_DIR}/include:${CMAKE_INSTALL_FULL_INCLUDEDIR}")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/VwShaders.cmake")

# The built-in shaders, compiled to .vws and embedded into libvw.
set(VW_SHADER_NAMES rounded_rect raster)
set(VW_SHADER_OUTPUTS)
foreach(shader IN LISTS VW_SHADER_NAMES)
  vw_compile_shader(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.vws"
                    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.comp")
  list(APPEND VW_SHADER_OUTPUTS "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.vws")
endforeach()
set(VW_BUILTIN_SHADERS_HEADER "${CMAKE_CURRENT_BINARY_DIR}/vw_builtin_shaders.hpp")
add_custom_command(
  OUTPUT "${VW_BUILTIN_SHADERS_HEADER}"
  COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${VW_BUILTIN_SHADERS_HEADER}" "-DNAMES=${VW_SHADER_NAMES}"
          "-DINPUTS=${VW_SHADER_OUTPUTS}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake"
  DEPENDS ${VW_SHADER_OUTPUTS} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake"
  COMMENT "Embedding vw's built-in shaders"
  VERBATIM)
add_custom_target(vw-shaders DEPENDS "${VW_BUILTIN_SHADERS_HEADER}")

# libvw: the C++ API (vw/vw.hpp). Wayland, Vulkan and the toolkit libraries
# are private, so consumers only need nlohmann_json.
add_library(vw_core SHARED
  src/animation.cpp
  src/app.cpp
  src/applications.cpp
  src/auth.cpp
  src/bluetooth.cpp
  src/brightness.cpp
  src/canvas.cpp
  src/capture.cpp
  src/dbus.cpp
  src/host.cpp
  src/hyprland.cpp
  src/icon.cpp
  src/image.cpp
  src/ipc.cpp
  src/network.cpp
  src/mpris.cpp
  src/notifications.cpp
  src/pipewire.cpp
  src/process.cpp
  src/renderer.cpp
  src/scene.cpp
  src/text.cpp
  src/tray.cpp
  src/ui.cpp
  src/upower.cpp
  src/vws.cpp
  src/roles/layer.cpp
  src/roles/lock.cpp
  src/roles/role.cpp
  src/roles/toplevel.cpp
  src/wayland.cpp
  src/zwwm.cpp)
add_library(vw::vw ALIAS vw_core)
set_target_properties(vw_core PROPERTIES
  OUTPUT_NAME vw
  EXPORT_NAME vw
  VERSION "${PROJECT_VERSION}"
  SOVERSION "${PROJECT_VERSION_MAJOR}")
add_dependencies(vw_core vw-shaders vw-protocol-headers zwwm_protocol_headers)
target_include_directories(vw_core
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
         "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" ${STB_INCLUDE_DIR} ${PAM_INCLUDE_DIR})
target_include_directories(vw_core SYSTEM PRIVATE ${QUICKJS_INCLUDE_DIR})
target_link_libraries(vw_core
  PUBLIC nlohmann_json::nlohmann_json
  PRIVATE
    Vulkan::Vulkan
    PkgConfig::FREETYPE
    PkgConfig::FONTCONFIG
    PkgConfig::HARFBUZZ
    PkgConfig::GIO_UNIX
    PkgConfig::PIPEWIRE
    PkgConfig::GTK3
    PkgConfig::RSVG
    PkgConfig::XKBCOMMON
    PkgConfig::PANGOCAIRO
    PkgConfig::WEBP
    ${PAM_LIBRARY}
    ${QUICKJS_LIBRARY}
    ${CMAKE_DL_LIBS}
    zwayland-client
    zwwm::protocol_metadata)
# Built-in shaders are embedded (vw_builtin_shaders.hpp); VW_SHADER_DIR can
# point at newer .vws files while working on them.
target_compile_definitions(vw_core PRIVATE VW_VERSION="${PROJECT_VERSION}")
target_compile_options(vw_core PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS vw_core vw-shaderc EXPORT vwTargets
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES
  include/vw/animation.hpp include/vw/app.hpp include/vw/applications.hpp include/vw/capture.hpp include/vw/host.hpp
  include/vw/bluetooth.hpp include/vw/brightness.hpp include/vw/network.hpp
  include/vw/tray.hpp include/vw/upower.hpp
  include/vw/hyprland.hpp include/vw/ipc.hpp include/vw/mpris.hpp include/vw/notifications.hpp
  include/vw/pipewire.hpp include/vw/process.hpp include/vw/scene.hpp include/vw/text.hpp include/vw/ui.hpp
  include/vw/vw.hpp include/vw/zwwm.hpp include/vw/shader.glsl
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/vw")

include(CMakePackageConfigHelpers)
install(EXPORT vwTargets NAMESPACE vw:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vw")
configure_package_config_file(cmake/vwConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/vwConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vw")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/vwConfigVersion.cmake"
  COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vwConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/vwConfigVersion.cmake"
              "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VwShaders.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vw")

option(VW_BUILD_EXAMPLES "Build the C++ examples" ON)
if(VW_BUILD_EXAMPLES)
  foreach(example bar launcher tasks status clock dock power calendar calculator)
    add_executable(vw-example-${example} examples/cpp/${example}.cpp)
    target_link_libraries(vw-example-${example} PRIVATE vw::vw)
    # Props are designated initializers that omit defaulted fields on purpose.
    target_compile_options(vw-example-${example} PRIVATE -Wall -Wextra -Wpedantic
      -Wno-missing-designated-field-initializers)
  endforeach()
endif()

# Deprecated: the Lua configuration frontend and its service bindings.
option(VW_WITH_LUA "Build the deprecated Lua frontend (the vw executable)" ON)
if(VW_WITH_LUA)
  find_package(Lua 5.4 REQUIRED)
  add_executable(vw
    src/main.cpp
    src/lua/animation.cpp
    src/lua/applications.cpp
    src/lua/hyprland.cpp
    src/lua/lua_config.cpp
    src/lua/lua_util.cpp
    src/lua/mpris.cpp
    src/lua/notifications.cpp
    src/lua/pipewire.cpp
    src/lua/socket.cpp
    src/lua/zwwm.cpp)
  target_include_directories(vw PRIVATE ${LUA_INCLUDE_DIR})
  target_link_libraries(vw PRIVATE vw::vw ${LUA_LIBRARIES})
  target_compile_definitions(vw PRIVATE VW_VERSION="${PROJECT_VERSION}")
  target_compile_options(vw PRIVATE -Wall -Wextra -Wpedantic)

  install(TARGETS vw RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  install(FILES
    examples/init.lua examples/toplevel.lua examples/slider.lua examples/material-slider.lua
    examples/notifications.lua examples/pipewire.lua examples/focus-grab.lua
    examples/canvas.lua examples/canvas.js examples/lock.lua
    DESTINATION "${CMAKE_INSTALL_DATADIR}/vw/examples")

  if(BUILD_TESTING)
    set(VW_TEST_ENV VW_NO_DEPRECATION_WARNING=1)
    add_test(NAME vw-help COMMAND vw --help)
    add_test(NAME vw-version COMMAND vw --version)
    add_test(NAME vw-check-init COMMAND vw --check -c "${CMAKE_CURRENT_SOURCE_DIR}/examples/init.lua")
    set_tests_properties(vw-help vw-version vw-check-init PROPERTIES ENVIRONMENT "${VW_TEST_ENV}")
  endif()
endif()
install(DIRECTORY examples/cpp/ DESTINATION "${CMAKE_INSTALL_DATADIR}/vw/examples/cpp" FILES_MATCHING PATTERN "*.cpp")
