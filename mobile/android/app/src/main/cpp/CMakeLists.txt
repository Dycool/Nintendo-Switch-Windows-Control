cmake_minimum_required(VERSION 3.22.1)
project(ns_mobile_protocol LANGUAGES C CXX)

add_library(nsprotocol SHARED
    ns_protocol_jni.cpp
    ../../../../../shared/ns_protocol.c
)

target_include_directories(nsprotocol PRIVATE
    ../../../../../shared
)

target_compile_features(nsprotocol PRIVATE cxx_std_17)

target_link_libraries(nsprotocol PRIVATE log)
