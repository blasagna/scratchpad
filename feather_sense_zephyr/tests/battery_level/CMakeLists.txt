# native_sim host test for the pure battery arithmetic: divider millivolts to
# percent, and the status-LED band with hysteresis. Reuses the firmware's own
# src/battery_level.cpp, which carries no Zephyr headers.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(battery_level_test)

target_sources(app PRIVATE
  src/main.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../src/battery_level.cpp
)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src)
