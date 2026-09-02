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

} /* namespace imu */

#endif /* FEATHER_SENSE_IMU_HPP_ */
