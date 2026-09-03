/*
 * Battery sampling: the ADC read, the percent, and the status LED band.
 * Copyright (c) 2026 Bob DiMaiolo. SPDX-License-Identifier: Apache-2.0
 */

#ifndef FEATHER_SENSE_BATTERY_HPP_
#define FEATHER_SENSE_BATTERY_HPP_

#include <stdint.h>

#include "codec.hpp"

namespace battery
{

/*
 * The battery sample, which is also this module's public type.
 *
 * One type rather than two that must agree: the bytes `streams::emit()` puts on
 * the wire and the bytes RPC opcode 0x01 answers with are the same bytes, so a
 * separate API struct would be a copy of the wire layout with a copy's failure
 * mode. The layout itself lives in codec.hpp with the other four, where the
 * host-side parity test can compile it.
 */
using Reading = codec::BatterySample;

int start();

/* The most recent reading. RPC opcode 0x01 answers from this rather than
 * touching the ADC, which is what lets RPC run on the caller's thread.
 */
Reading last();

} /* namespace battery */

#endif /* FEATHER_SENSE_BATTERY_HPP_ */
