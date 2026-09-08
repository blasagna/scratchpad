/*
 * The five remote procedure calls, shared by both transports.
 * Copyright (c) 2026 Bob DiMaiolo. SPDX-License-Identifier: Apache-2.0
 */

#ifndef FEATHER_SENSE_RPC_HPP_
#define FEATHER_SENSE_RPC_HPP_

#include <stddef.h>
#include <stdint.h>

namespace rpc
{

/*
 * Handle one request frame and write the response frame to `out`. Returns the
 * response length, or 0 if the request was too short to answer at all -- with
 * no `seq` to echo there is nothing a host could match a reply to, so silence
 * is the only honest response.
 *
 * Every opcode answers from cached state: `get battery` reads the battery
 * thread's last sample rather than the ADC. So this never blocks, and it runs
 * on whichever thread the request arrived on -- the Bluetooth RX thread for a
 * GATT write, the USB rx thread for a COBS frame.
 */
size_t handle(const uint8_t *request, size_t len, uint8_t *out, size_t out_cap);

} /* namespace rpc */

#endif /* FEATHER_SENSE_RPC_HPP_ */
