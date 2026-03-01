cmake_minimum_required(VERSION 3.16)
project(wirelesshubd VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-O2 -Wall -Wextra)

file(GLOB_RECURSE SOURCES src/*.cpp)

add_executable(wirelesshubd ${SOURCES})

target_include_directories(wirelesshubd PRIVATE src)

target_link_libraries(wirelesshubd PRIVATE pthread)

install(TARGETS wirelesshubd DESTINATION /usr/local/bin)

# ── WASD HID mouse test (fake ESP32) ──────────────────────────────────────
add_executable(wasd_mouse_test tests/WasdMouseTest.cpp)
target_compile_options(wasd_mouse_test PRIVATE -O2 -Wall -Wextra)
