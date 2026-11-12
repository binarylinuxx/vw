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
  add_custom_command(
    OUTPUT "${FACET_OUTPUT}"
    COMMAND vw::facetc "${source}" -o "${FACET_OUTPUT}" --namespace "${FACET_NAMESPACE}" ${module}
    DEPENDS vw::facetc "${source}"
    COMMENT "facetc ${FACET_SOURCE}"
    VERBATIM)
endfunction()
