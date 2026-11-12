# vw_compile_shader(OUTPUT <file.vws> SOURCE <file.comp>
#                   [INCLUDE_DIRECTORIES dir...] [DEFINES NAME[=VALUE]...])
#
# Compiles a GLSL compute shader into vw's .vws format with vw-shaderc.
# Includes are tracked, so editing an included file recompiles the shader.
# Add the OUTPUT to a target's sources or a custom target to build it.
function(vw_compile_shader)
  cmake_parse_arguments(VWS "" "OUTPUT;SOURCE" "INCLUDE_DIRECTORIES;DEFINES" ${ARGN})
  if(NOT VWS_OUTPUT OR NOT VWS_SOURCE)
    message(FATAL_ERROR "vw_compile_shader needs OUTPUT and SOURCE")
  endif()
  get_filename_component(source "${VWS_SOURCE}" ABSOLUTE)
  get_filename_component(output "${VWS_OUTPUT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
  get_filename_component(output_directory "${output}" DIRECTORY)
  set(arguments)
  foreach(directory IN LISTS VWS_INCLUDE_DIRECTORIES)
    list(APPEND arguments -I "${directory}")
  endforeach()
  foreach(define IN LISTS VWS_DEFINES)
    list(APPEND arguments -D "${define}")
  endforeach()
  if(GLSLC)
    list(APPEND arguments --glslc "${GLSLC}")
  endif()
  add_custom_command(
    OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_directory}"
    COMMAND vw::shaderc ${arguments} --depfile "${output}.d" "${source}" -o "${output}"
    DEPENDS "${source}" vw::shaderc
    DEPFILE "${output}.d"
    COMMENT "Compiling shader ${VWS_SOURCE}"
    VERBATIM)
endfunction()
