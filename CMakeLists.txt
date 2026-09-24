cmake_minimum_required(VERSION 3.24)

project(OpticalFabric
  VERSION 1.0.0
  DESCRIPTION "Optical Fabric - governed optical connectivity and path authority runtime"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Policy
# ---------------------------------------------------------------------------
if(POLICY CMP0155)
  cmake_policy(SET CMP0155 NEW)
endif()
if(POLICY CMP0156)
  cmake_policy(SET CMP0156 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(OPTICAL_FABRIC_BUILD_TESTS "Build the test suite" ON)
option(OPTICAL_FABRIC_BUILD_EXAMPLES "Build runnable examples" ON)
option(OPTICAL_FABRIC_BUILD_BENCHMARKS "Build benchmarks" ON)
option(OPTICAL_FABRIC_BUILD_APPS "Build the CLI and the control-plane node" ON)
option(OPTICAL_FABRIC_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(OPTICAL_FABRIC_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)
option(OPTICAL_FABRIC_BUILD_SHARED "Build a shared library instead of a static library" OFF)

if(OPTICAL_FABRIC_BUILD_SHARED)
  set(OPTICAL_FABRIC_LIB_TYPE SHARED)
else()
  set(OPTICAL_FABRIC_LIB_TYPE STATIC)
endif()

# ---------------------------------------------------------------------------
# Warnings: first-party targets only, never third-party or system headers.
# ---------------------------------------------------------------------------
function(optical_fabric_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)
    if(OPTICAL_FABRIC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(OPTICAL_FABRIC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(OPTICAL_FABRIC_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(OPTICAL_FABRIC_SOURCES
  src/version.cpp
  src/error.cpp
  src/digest.cpp
  src/crc_stream.cpp
  src/identity.cpp
  src/limits.cpp
  src/generation.cpp
  src/evidence.cpp
  src/topology.cpp
  src/lifecycle.cpp
  src/path.cpp
  src/text_codec.cpp
  src/state_codec.cpp
  src/store.cpp
  src/persistence.cpp
  src/inspection.cpp
  src/report.cpp
  src/interfaces.cpp
  src/fabric_core.cpp
  src/fabric_route.cpp
  src/fabric_authority.cpp
  src/fabric_connectivity.cpp
  src/fabric_query.cpp
  src/protocol.cpp
  src/socket.cpp
  src/server.cpp
  src/client.cpp)

add_library(OpticalFabric ${OPTICAL_FABRIC_LIB_TYPE} ${OPTICAL_FABRIC_SOURCES})
add_library(SummonSoftwareLabs::OpticalFabric ALIAS OpticalFabric)

set_target_properties(OpticalFabric PROPERTIES
  EXPORT_NAME OpticalFabric
  POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  WINDOWS_EXPORT_ALL_SYMBOLS ON)

target_include_directories(OpticalFabric
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

optical_fabric_apply_warnings(OpticalFabric)

if(WIN32)
  # ws2_32: loopback control plane. bcrypt: entropy for incarnation identity.
  target_link_libraries(OpticalFabric PRIVATE ws2_32 bcrypt)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(OpticalFabric PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# Install + package export
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS OpticalFabric
  EXPORT OpticalFabricTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT OpticalFabricTargets
  FILE OpticalFabricTargets.cmake
  NAMESPACE SummonSoftwareLabs::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OpticalFabric)

configure_package_config_file(
  cmake/OpticalFabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/OpticalFabricConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OpticalFabric)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/OpticalFabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/OpticalFabricConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/OpticalFabricConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OpticalFabric)

install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/optical_fabric)

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------
if(OPTICAL_FABRIC_BUILD_APPS)
  foreach(app of_cli of_node)
    add_executable(${app} apps/${app}.cpp)
    target_link_libraries(${app} PRIVATE OpticalFabric)
    optical_fabric_apply_warnings(${app})
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Tests, examples and benchmarks
# ---------------------------------------------------------------------------
if(OPTICAL_FABRIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(OPTICAL_FABRIC_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(OPTICAL_FABRIC_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
