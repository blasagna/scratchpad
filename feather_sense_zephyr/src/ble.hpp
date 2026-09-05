/*
 * The GATT service: four notify characteristics, one per rate class, plus the
 * RPC pair. Copyright (c) 2026 Bob DiMaiolo. SPDX-License-Identifier: Apache-2.0
 */

#ifndef FEATHER_SENSE_BLE_HPP_
#define FEATHER_SENSE_BLE_HPP_

#include <stddef.h>
#include <stdint.h>

namespace ble
{

/* Enables the controller and starts advertising. */
int start();

bool connected();

/*
 * Whether a host is subscribed to the characteristic that carries `stream_id`.
 * Streams share characteristics by rate class -- battery and button both ride
 * the events one -- so this is a question about the characteristic, not the
 * stream.
 */
bool subscribed(uint8_t stream_id);

/*
 * Notify one batch on the characteristic its header's stream_id selects. The
 * bytes are the batch, unchanged: BLE needs no framing, because a GATT
 * notification is already a delimited datagram.
 */
void notify(const uint8_t *batch, size_t len);

/* Notify one RPC response frame on the rpc response characteristic. */
void notify_rpc(const uint8_t *frame, size_t len);

} /* namespace ble */

#endif /* FEATHER_SENSE_BLE_HPP_ */
