/*
 * Battery sampling: the ADC read, the percent, and the status LED band.
 * Copyright (c) 2026 Bob DiMaiolo. SPDX-License-Identifier: Apache-2.0
 */

#ifndef FEATHER_SENSE_BATTERY_HPP_
#define FEATHER_SENSE_BATTERY_HPP_

#include <stdint.h>

namespace battery
{

struct Reading {
	uint16_t millivolts;
	uint8_t percent;
	uint8_t flags;
};

int start();

/* The most recent reading. RPC opcode 0x01 answers from this rather than
 * touching the ADC, which is what lets RPC run on the caller's thread.
 */
Reading last();

} /* namespace battery */

#endif /* FEATHER_SENSE_BATTERY_HPP_ */
