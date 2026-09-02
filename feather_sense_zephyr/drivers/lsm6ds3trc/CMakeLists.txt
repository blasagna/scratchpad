# The register-level work is ST's own vendored stmemsc driver, at
# ~/zephyrproject/modules/hal/st/sensor/stmemsc/lsm6ds3tr-c_STdC/driver/, which
# the hal_st module compiles and puts on the include path once
# CONFIG_USE_STDC_LSM6DS3TR_C is set (this module's Kconfig `select`s it, since
# that symbol has no prompt and cannot come from prj.conf). Nothing from ST is
# copied into this repo.

if(CONFIG_LSM6DS3TRC)
  zephyr_library()
  zephyr_library_sources(lsm6ds3trc.c)
  zephyr_library_sources_ifdef(CONFIG_LSM6DS3TRC_TRIGGER lsm6ds3trc_trigger.c)

  # stmemsc.h -- the bus shim that adapts ST's stmdev_ctx_t to Zephyr's
  # i2c_dt_spec. In-tree ST drivers reach it as `../stmemsc`; from out of tree
  # it needs the absolute path.
  zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/sensor/st/stmemsc)

  # The app-facing FIFO API in lsm6ds3trc.h, for src/imu.cpp.
  zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR})
endif()
