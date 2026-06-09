cmake_minimum_required(VERSION 3.22.1)
project(ns_mobile_protocol LANGUAGES C CXX)

# SDL3 Android static integration.
# GitHub Actions installs one SDL3 prefix per ABI at:
#   <repo>/_cache/sdl3-android/<abi>
# The Gradle build passes -DSDL3_ANDROID_ROOT=<repo>/_cache/sdl3-android.
if(NOT SDL3_ANDROID_ROOT)
    if(DEFINED ENV{SDL3_ANDROID_ROOT})
        set(SDL3_ANDROID_ROOT "$ENV{SDL3_ANDROID_ROOT}")
    else()
        set(SDL3_ANDROID_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../_cache/sdl3-android")
    endif()
endif()

set(SDL3_ABI_PREFIX "${SDL3_ANDROID_ROOT}/${ANDROID_ABI}")
set(SDL3_DIR "${SDL3_ABI_PREFIX}/lib/cmake/SDL3" CACHE PATH "SDL3 CMake package directory")

find_package(SDL3 CONFIG REQUIRED)

if(TARGET SDL3::SDL3-static)
    set(NS_SDL3_TARGET SDL3::SDL3-static)
elseif(TARGET SDL3::SDL3)
    set(NS_SDL3_TARGET SDL3::SDL3)
elseif(TARGET SDL3::SDL3-shared)
    set(NS_SDL3_TARGET SDL3::SDL3-shared)
else()
    message(FATAL_ERROR "SDL3 package was found, but no usable SDL3 CMake target exists in ${SDL3_DIR}")
endif()

add_library(nsprotocol SHARED
    ns_protocol_jni.cpp
    sdl_controller_jni.cpp
    ../../../../../shared/ns_protocol.c
    ../../../../../shared/sdl_controller.c
)

target_include_directories(nsprotocol PRIVATE
    ../../../../../shared
)

target_compile_features(nsprotocol PRIVATE cxx_std_17)

target_link_libraries(nsprotocol PRIVATE
    ${NS_SDL3_TARGET}
    android
    log
)
