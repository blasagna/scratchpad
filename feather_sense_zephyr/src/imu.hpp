/* Copyright (c) 2026 Bob DiMaiolo. SPDX-License-Identifier: Apache-2.0 */

#ifndef FEATHER_SENSE_IMU_HPP_
#define FEATHER_SENSE_IMU_HPP_

#include <stdint.h>

namespace imu
{

int start();

/* Diagnostics for the `fs imu` shell command. */
struct Stats {
	uint32_t samples;
	uint32_t batches;
	uint32_t overruns;
	uint32_t stall_flushes;
	uint8_t who_am_i;
};

Stats stats();

/*
 * Hold the next drain off for `ms` milliseconds, once.
 *
 * A test hook, and the only way to reach the stall clamp: it needs a backlog
 * past 96 samples, which at 208 Hz is 460 ms of nothing draining the FIFO, and
 * the worst gap a healthy board has ever shown is 6.7 ms. Nothing in normal
 * operation calls this.
 */
void stall(uint32_t ms);

} /* namespace imu */

#endif /* FEATHER_SENSE_IMU_HPP_ */
