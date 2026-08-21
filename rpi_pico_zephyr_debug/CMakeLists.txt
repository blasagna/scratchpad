# Freestanding Zephyr application: ZEPHYR_BASE must point at the Zephyr repo
# inside ~/zephyrproject.

cmake_minimum_required(VERSION 3.20.0)

# `west debug` / `west attach` fix. With CONFIG_DEBUG_THREAD_INFO=y the OpenOCD
# runner appends `$_TARGETNAME configure -rtos Zephyr` for thread-aware
# debugging, but RP2040's upstream `target/rp2040.cfg` names its core target
# `_TARGETNAME_0` (it's a dual-core part) and never defines the singular
# `_TARGETNAME` the runner assumes. The undefined Tcl variable aborts OpenOCD
# init, so the GDB server never comes up and `west debug` dies with a
# connection timeout. Point the runner at the real handle. (`west flash` is
# unaffected -- it doesn't emit the RTOS command.)
#
# `board_finalize_runner_args` invokes this macro, but only if it's defined
# before find_package(Zephyr) -- the finalize runs while the board is processed,
# which find_package triggers -- so it must sit above that line, not below.
#
# The macro fires once per runner the board registers (rpi_pico has six), so a
# GLOBAL-property guard keeps it from appending the same arg six times.
macro(app_set_runner_args)
  get_property(_app_runner_args_done GLOBAL PROPERTY app_runner_args_done)
  if(NOT _app_runner_args_done)
    set_property(GLOBAL PROPERTY app_runner_args_done TRUE)
    board_runner_args(openocd "--target-handle=_TARGETNAME_0")
  endif()
endmacro()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rpi_pico_zephyr_debug)

target_sources(app PRIVATE
  src/main.cpp
  src/temp_convert.cpp
)
