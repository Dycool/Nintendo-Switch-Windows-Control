cmake_minimum_required(VERSION 3.16)
project(ns-switch-control CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-O3 -Wall -Wextra)

include_directories(${CMAKE_SOURCE_DIR}/include)

option(USE_UPNP "Enable UPnP port forwarding via miniupnpc" ON)

if(USE_UPNP)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(MINIUPNPC miniupnpc)
    endif()
    if(MINIUPNPC_FOUND)
        add_definitions(-DUSE_UPNP)
        message(STATUS "UPnP support enabled (miniupnpc found)")
    else()
        message(STATUS "UPnP support disabled (miniupnpc not found)")
    endif()
else()
    message(STATUS "UPnP support disabled by user")
endif()

add_executable(ns-backend src/backend/main.cpp)
target_link_libraries(ns-backend PRIVATE pthread)
if(MINIUPNPC_FOUND)
    target_link_libraries(ns-backend PRIVATE ${MINIUPNPC_LIBRARIES})
    target_include_directories(ns-backend PRIVATE ${MINIUPNPC_INCLUDE_DIRS})
endif()
