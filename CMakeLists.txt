cmake_minimum_required(VERSION 3.16)
project(fsbench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

file(GLOB FSBENCH_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/*.cpp
  ${CMAKE_SOURCE_DIR}/src/tests/*.cpp
  ${CMAKE_SOURCE_DIR}/src/bench/*.cpp)

add_executable(fsbench ${FSBENCH_SOURCES})
target_include_directories(fsbench PRIVATE ${CMAKE_SOURCE_DIR}/src)

find_package(Threads REQUIRED)
target_link_libraries(fsbench PRIVATE Threads::Threads)

if(MSVC)
  target_compile_options(fsbench PRIVATE /W4 /permissive- /utf-8 /EHsc /O2)
  target_compile_definitions(fsbench PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
else()
  target_compile_options(fsbench PRIVATE -Wall -Wextra -O2)
endif()

if(WIN32)
  target_link_libraries(fsbench PRIVATE advapi32)
  if(MINGW)
    target_link_options(fsbench PRIVATE -static)
  endif()
endif()
