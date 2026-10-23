cmake_minimum_required(VERSION 3.15)
project(DatacenterShift CXX)
set(CMAKE_CXX_STANDARD 17)

find_package(raylib 5.0 QUIET)
if (NOT raylib_FOUND)
  include(FetchContent)
  FetchContent_Declare(raylib GIT_REPOSITORY https://github.com/raysan5/raylib.git GIT_TAG 5.0)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(raylib)
endif()

add_executable(datacenter main.cpp)
target_link_libraries(datacenter raylib)
