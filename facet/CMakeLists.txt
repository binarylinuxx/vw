# Facet: a declarative view language compiled to C++ on vw. Built only with
# -DVW_WITH_FACET=ON; vw itself does not depend on it.

# facetc needs nothing but the standard library.
add_executable(vw-facetc src/main.cpp src/lexer.cpp src/parser.cpp src/generator.cpp)
add_executable(vw::facetc ALIAS vw-facetc)
set_target_properties(vw-facetc PROPERTIES EXPORT_NAME facetc)
target_compile_features(vw-facetc PRIVATE cxx_std_23)
target_compile_options(vw-facetc PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS vw-facetc EXPORT vwTargets RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

include("${PROJECT_SOURCE_DIR}/cmake/VwFacet.cmake")

if(VW_BUILD_EXAMPLES)
  set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
  file(MAKE_DIRECTORY "${generated}")
  foreach(example launcher cpp-blocks)
    set(view launcher)
    set(program main)
    if(example STREQUAL "cpp-blocks")
      set(view inbox)
      set(program check)
    endif()
    facet_compile(OUTPUT "${generated}/${view}.facet.hpp" SOURCE examples/${example}/${view}.facet)
    add_executable(vw-facet-${example} examples/${example}/${program}.cpp "${generated}/${view}.facet.hpp")
    target_include_directories(vw-facet-${example} PRIVATE examples/${example} "${generated}")
    target_compile_options(vw-facet-${example} PRIVATE -Wall -Wextra)
    target_link_libraries(vw-facet-${example} PRIVATE vw::vw)
  endforeach()
endif()
