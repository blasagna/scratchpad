# SPDX-License-Identifier: Apache-2.0
#
# Freestanding Zephyr application: ZEPHYR_BASE must point at the Zephyr repo
# inside ~/zephyrproject.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(microbit_v2_zephyr)

target_sources(app PRIVATE
  src/main.c
  src/accel.c
  src/audio.c
  src/ble.c
  src/buttons.c
  src/display.c
  src/temp.c
)
