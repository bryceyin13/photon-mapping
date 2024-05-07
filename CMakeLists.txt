cmake_minimum_required(VERSION 3.20)
project(pm LANGUAGES CXX)

# OpenMP
find_package(OpenMP)
if(NOT OpenMP_CXX_FOUND)
    message(WARNING "failed to find OpenMP")
endif()

# Embree3
find_package(embree 3.0 REQUIRED)
if (${embree_FOUND})
    message(STATUS "Found Embree")
else()
    message(FATAL_ERROR "Could not find Embree")
endif()

add_library(pm INTERFACE)
target_include_directories(pm INTERFACE "include")
target_compile_features(pm INTERFACE cxx_std_20)
set_target_properties(pm PROPERTIES CXX_EXTENSIONS OFF)

# tinyobjloader
add_library(tinyobjloader INTERFACE)
target_include_directories(tinyobjloader INTERFACE "tinyobjloader")
target_compile_definitions(tinyobjloader INTERFACE TINYOBJLOADER_IMPLEMENTATION)
target_include_directories(tinyobjloader SYSTEM INTERFACE tinyobjloader)

target_link_libraries(pm INTERFACE OpenMP::OpenMP_CXX)
target_link_libraries(pm INTERFACE tinyobjloader)
target_link_libraries(pm INTERFACE embree)

add_executable(main "main.cpp")
target_link_libraries(main PRIVATE pm)
