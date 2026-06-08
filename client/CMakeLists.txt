cmake_minimum_required(VERSION 3.20)
project(ns_clients LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(SDL3 REQUIRED CONFIG)

set(NS_CLIENT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/ns-client.cpp")

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(ns-client WIN32 MACOSX_BUNDLE "${NS_CLIENT_SOURCE}")
target_compile_definitions(ns-client PRIVATE
    SDL_MAIN_HANDLED
    NS_CLIENT_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}"
)
target_link_libraries(ns-client PRIVATE Qt6::Widgets SDL3::SDL3 Threads::Threads)

if(WIN32 AND TARGET Qt6::EntryPoint)
    target_link_libraries(ns-client PRIVATE Qt6::EntryPoint)
endif()

if(WIN32)
    target_sources(ns-client PRIVATE "${CMAKE_CURRENT_LIST_DIR}/ns-gui.rc")
    target_compile_definitions(ns-client PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        UNICODE
        _UNICODE
    )
    target_link_libraries(ns-client PRIVATE ws2_32 winmm)
endif()

if(APPLE)
    find_library(COREGRAPHICS_FRAMEWORK CoreGraphics)
    target_link_libraries(ns-client PRIVATE ${COREGRAPHICS_FRAMEWORK})

    set_target_properties(ns-client PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "NS PC Control"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.nspccontrol.client"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
        MACOSX_BUNDLE_INFO_STRING "NS PC Control"
    )
endif()

if(UNIX AND NOT APPLE)
    set_target_properties(ns-client PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

if(MSVC)
    target_compile_options(ns-client PRIVATE /W3 /permissive- /EHsc)
else()
    target_compile_options(ns-client PRIVATE -Wall -Wextra -Wpedantic)
endif()
