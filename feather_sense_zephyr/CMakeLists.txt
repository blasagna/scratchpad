# Freestanding Zephyr application: ZEPHYR_BASE must point at the Zephyr repo
# inside ~/zephyrproject.

cmake_minimum_required(VERSION 3.20.0)

# The out-of-tree LSM6DS3TR-C/LSM6DS33 driver. This must be set before
# find_package(Zephyr), because the Zephyr package walks ZEPHYR_EXTRA_MODULES
# while it processes Kconfig and devicetree bindings -- adding it afterwards
# leaves `scratchpad,lsm6ds3trc` an unknown compatible and the node unbound.
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/drivers/lsm6ds3trc)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(feather_sense_zephyr)

target_sources(app PRIVATE
  src/main.cpp
  src/battery.cpp
  src/battery_level.cpp
  src/ble.cpp
  src/buttons.cpp
  src/codec.cpp
  src/env.cpp
  src/imu.cpp
  src/led.cpp
  src/magn.cpp
  src/rpc.cpp
  src/shell.cpp
  src/streams.cpp
  src/usb.cpp
)

# The `get build id` RPC (opcode 0x05) reports this. `git describe` is resolved
# at configure time, so a rebuild after a commit picks up the new value only on
# a cmake re-run -- which `west build` does whenever CMakeLists.txt changes and
# `-p auto` forces anyway.
execute_process(
  COMMAND git describe --always --dirty --tags
  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
  OUTPUT_VARIABLE APP_GIT_DESCRIBE
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT APP_GIT_DESCRIBE)
  set(APP_GIT_DESCRIBE "unknown")
endif()
target_compile_definitions(app PRIVATE APP_GIT_DESCRIBE="${APP_GIT_DESCRIBE}")
