cmake_minimum_required(VERSION 3.24)

# Independent downstream consumer of the installed Optical Fabric package.
# This project is deliberately NOT part of the Optical Fabric build: it exists to
# prove that the installed artifact alone (headers, library, exported target and
# package config) is enough to compile, link and run against.
project(OpticalFabricConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(OpticalFabric 1.0 CONFIG REQUIRED)

if(NOT TARGET SummonSoftwareLabs::OpticalFabric)
  message(FATAL_ERROR "the installed package did not export SummonSoftwareLabs::OpticalFabric")
endif()

message(STATUS "consumer resolved OpticalFabric ${OpticalFabric_VERSION}")

add_executable(optical_fabric_consumer main.cpp)
target_link_libraries(optical_fabric_consumer PRIVATE SummonSoftwareLabs::OpticalFabric)

if(MSVC)
  target_compile_options(optical_fabric_consumer PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
else()
  target_compile_options(optical_fabric_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()
add_test(NAME optical_fabric_consumer COMMAND optical_fabric_consumer)
