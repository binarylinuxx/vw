# Register an external Facet module in the current CMake directory.
function(facet_register_module)
  cmake_parse_arguments(REG "" "NAME;HEADER" "" ${ARGN})
  if(NOT REG_NAME OR NOT REG_HEADER OR REG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "facet_register_module needs NAME and HEADER")
  endif()
  get_property(previous DIRECTORY PROPERTY "VW_FACET_HEADER_${REG_NAME}")
  if(previous AND NOT previous STREQUAL REG_HEADER)
    message(FATAL_ERROR "Conflicting headers for Facet module ${REG_NAME}")
  endif()
  if(NOT previous)
    set_property(DIRECTORY APPEND PROPERTY VW_FACET_MODULES "${REG_NAME}")
    set_property(DIRECTORY PROPERTY "VW_FACET_HEADER_${REG_NAME}" "${REG_HEADER}")
  endif()
endfunction()

# facet_compile(OUTPUT <file.hpp> SOURCE <file.facet> [NAMESPACE <name>] [MODULE <path>])
#
# From vw built with -DVW_WITH_FACET=ON.
#
# Compiles a .facet file into a C++ header with vw::facetc. Add the OUTPUT to a
# target's sources so it is generated before the target builds, and link the
# target to vw::vw (the header includes <vw/facet.hpp>).
function(facet_compile)
  cmake_parse_arguments(FACET "" "OUTPUT;SOURCE;NAMESPACE;MODULE" "" ${ARGN})
  if(NOT FACET_OUTPUT OR NOT FACET_SOURCE)
    message(FATAL_ERROR "facet_compile needs OUTPUT and SOURCE")
  endif()
  if(NOT FACET_NAMESPACE)
    set(FACET_NAMESPACE views)
  endif()
  get_filename_component(source "${FACET_SOURCE}" ABSOLUTE)
  set(module)
  if(FACET_MODULE)
    set(module --module "${FACET_MODULE}")
  endif()
  get_property(registered DIRECTORY PROPERTY VW_FACET_MODULES)
  set(module_headers)
  foreach(name IN LISTS registered)
    get_property(header DIRECTORY PROPERTY "VW_FACET_HEADER_${name}")
    list(APPEND module_headers --module-header "${name}" "${header}")
  endforeach()
  add_custom_command(
    OUTPUT "${FACET_OUTPUT}"
    COMMAND vw::facetc "${source}" -o "${FACET_OUTPUT}" --namespace "${FACET_NAMESPACE}" ${module} ${module_headers}
    DEPENDS vw::facetc "${source}"
    COMMENT "facetc ${FACET_SOURCE}"
    VERBATIM)
endfunction()
