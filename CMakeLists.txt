cmake_minimum_required(VERSION 3.15)
project(vs_placebo C)
project(p2p CXX)

set(CMAKE_C_STANDARD 99)

add_library(p2p STATIC libp2p/p2p_api.cpp libp2p/v210.cpp)
add_library(vs_placebo SHARED vs-placebo.c vs-placebo.h deband.c deband.h tonemap.c tonemap.h)
target_compile_options(vs_placebo PRIVATE -Wno-discarded-qualifiers)
target_link_libraries(vs_placebo p2p)
target_link_libraries(vs_placebo -lplacebo)
