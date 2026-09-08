# native_sim host test for the wire format: batch header pack/unpack and COBS.
# It reuses the firmware's own src/codec.cpp -- that translation unit is
# deliberately free of Zephyr headers so it compiles unchanged for the host.
#
#   west build -b native_sim -p auto -d build_test tests/codec \
#     && ./build_test/zephyr/zephyr.exe
#   west twister -p native_sim -T tests

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(codec_test)

target_sources(app PRIVATE
  src/main.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../src/codec.cpp
)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src)
