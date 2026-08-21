# native_sim host test for the pure raw_to_millicelsius() conversion. It reuses
# the firmware's own src/temp_convert.cpp -- that TU is deliberately free of
# Zephyr headers so it compiles unchanged for the host. This is the C++/Zephyr
# analog of `cargo test -p rp2040_temp` in the Rust sibling.
#
# Run it with either:
#   west build -b native_sim -p auto tests/temp_convert && ./build/zephyr/zephyr.exe
#   west twister -p native_sim -T tests

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(temp_convert_test)

target_sources(app PRIVATE
  src/main.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../src/temp_convert.cpp
)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src)
