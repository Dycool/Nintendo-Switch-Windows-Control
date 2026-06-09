cmake_minimum_required(VERSION 3.22.1)
project(ns_mobile_protocol LANGUAGES C CXX)

# SDL3 static library integration.
# Set SDL3_DIR to the SDL3 source root, or it defaults to a path relative to the project.
# SDL3 must be pre-built for each Android ABI; the library is found under SDL3_DIR/build/<abi>.
if(NOT SDL3_DIR)
    set(SDL3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../SDL3")
endif()

set(SDL3_INCLUDE_DIR "${SDL3_DIR}/include")

# Find the SDL3 static library for the current Android ABI.
set(SDL3_LIB_DIR "${SDL3_DIR}/build/${ANDROID_ABI}")
set(SDL3_LIB "${SDL3_LIB_DIR}/libSDL3.a")

if(NOT EXISTS "${SDL3_LIB}")
    message(WARNING "SDL3 static library not found at ${SDL3_LIB}. "
                    "Build SDL3 first: cd ${SDL3_DIR} && cmake -S . -B build/${ANDROID_ABI} "
                    "-DANDROID_ABI=${ANDROID_ABI} && cmake --build build/${ANDROID_ABI}")
endif()

add_library(nsprotocol SHARED
    ns_protocol_jni.cpp
    sdl_controller_jni.cpp
    ../../../../../shared/ns_protocol.c
    ../../../../../shared/sdl_controller.c
)

target_include_directories(nsprotocol PRIVATE
    ../../../../../shared
    ${SDL3_INCLUDE_DIR}
)

target_compile_features(nsprotocol PRIVATE cxx_std_17)

target_link_libraries(nsprotocol PRIVATE
    log
    ${SDL3_LIB}
)
