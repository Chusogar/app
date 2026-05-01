cmake_minimum_required(VERSION 3.10)
project(lynx C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall -Wextra -Wno-unused-parameter")

find_package(SDL2 REQUIRED)

include_directories(${SDL2_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR})

add_executable(lynx
    lynx.c
    m6502/m6502.c
)

target_link_libraries(lynx ${SDL2_LIBRARIES})
