/* Copyright (c) 2026 Bob DiMaiolo. SPDX-License-Identifier: Apache-2.0 */

#ifndef FEATHER_SENSE_MAGN_HPP_
#define FEATHER_SENSE_MAGN_HPP_

#include <stdint.h>

namespace magn
{

int start();

/* How many half-filled batches have been discarded because a fetch failed
 * between their two samples. Reported by `fs magn`.
 */
uint32_t dropped_batches();

/* How many ticks failed their fetch, whole batches and half ones together. */
uint32_t fetch_failures();

/*
 * Treat the next `count` fetches as failed without touching the chip.
 *
 * A test hook. The LIS3MDL's DRDY and INT pins are not routed and its I2C
 * interface cannot be disabled and re-enabled from the host side, so there is
 * no way to make a real fetch fail that this board can also recover from. What
 * this reaches is the discard branch and its host-visible consequence, which is
 * the part that had never been run; the driver's own error return is still
 * taken on trust. Nothing in normal operation calls this.
 */
void fail_next(uint32_t count);

} /* namespace magn */

#endif /* FEATHER_SENSE_MAGN_HPP_ */
