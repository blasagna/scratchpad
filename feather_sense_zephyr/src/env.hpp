/* Copyright (c) 2026 Bob DiMaiolo. SPDX-License-Identifier: Apache-2.0 */

#ifndef FEATHER_SENSE_ENV_HPP_
#define FEATHER_SENSE_ENV_HPP_

#include <stdint.h>

namespace env
{

int start();

/* Microseconds the last SHT30 fetch blocked for, and how many have failed.
 *
 * The CircuitPython port's most expensive lesson was that 1 Hz environmental
 * reads stalled its loop for ~152 ms of every second, and the design document
 * carried that forward as an upper bound to check. Microseconds rather than
 * milliseconds because the answer turned out to be three orders of magnitude
 * smaller: Zephyr's driver runs the SHT30 in periodic mode, so a fetch is a
 * command and a read rather than a conversion. `fs env` prints both.
 */
uint32_t last_fetch_us();
uint32_t fetch_failures();

} /* namespace env */

#endif /* FEATHER_SENSE_ENV_HPP_ */
