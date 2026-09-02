/*
 * The binary sample pipe: a second CDC ACM instance, COBS-framed.
 * Copyright (c) 2026 Bob DiMaiolo. SPDX-License-Identifier: Apache-2.0
 *
 * The board's own `board_cdc_acm_uart` keeps the console, the shell and the
 * log. Nothing here ever writes to it. That separation is structural: the
 * CircuitPython port shared one pipe between console and data, and the
 * runtime's status-bar escape sequence landed between two binary frames on
 * every host attach.
 */

#ifndef FEATHER_SENSE_USB_HPP_
#define FEATHER_SENSE_USB_HPP_

#include <stddef.h>
#include <stdint.h>

namespace usb
{

/* Brings up the USB device with BOTH CDC ACM instances and starts the rx
 * thread. See the note in prj.conf on CDC_ACM_SERIAL_INITIALIZE_AT_BOOT.
 */
int start();

/* Whether a host has opened the data port (DTR asserted). */
bool attached();

/* COBS-frame `payload` on `channel` and queue it. Drops on a full queue. */
void send(uint8_t channel, const uint8_t *payload, size_t len);

/* Counters, for the `fs stats` shell command. */
struct Counters {
	uint32_t frames_sent;
	uint32_t frames_dropped;
	uint32_t rx_frames;
	uint32_t rx_errors;
};

Counters counters();

} /* namespace usb */

#endif /* FEATHER_SENSE_USB_HPP_ */
