cmake_minimum_required(VERSION 3.25)
project(my-widget LANGUAGES CXX)

find_package(vw REQUIRED)

add_executable(my-widget main.cpp)
target_compile_features(my-widget PRIVATE cxx_std_23)
target_link_libraries(my-widget PRIVATE vw::vw)
install(TARGETS my-widget)
