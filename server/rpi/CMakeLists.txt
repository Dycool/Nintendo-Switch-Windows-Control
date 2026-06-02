cmake_minimum_required(VERSION 3.16)
project(ns-switch-control CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-O2 -Wall -Wextra)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_executable(ns-backend src/backend/main.cpp)
target_link_libraries(ns-backend PRIVATE pthread)
