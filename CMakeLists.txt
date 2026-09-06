cmake_minimum_required(VERSION 3.10)
project(zlgcan_bridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(zlgcan_bridge
  src/bridge_main.cpp
  src/zlgcan_api.cpp
  src/socketcan_port.cpp
)

target_include_directories(zlgcan_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(zlgcan_bridge PRIVATE -Wall -Wextra)

find_package(Threads REQUIRED)
find_library(DL_LIB dl)
target_link_libraries(zlgcan_bridge PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
