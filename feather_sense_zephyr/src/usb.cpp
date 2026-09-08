/*
 * Copyright (c) 2026 Bob DiMaiolo
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usb.hpp"

#include "codec.hpp"
#include "rpc.hpp"
#include "streams.hpp"

#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(usb, LOG_LEVEL_INF);

namespace usb
{
namespace
{

const device *const data_port = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_data));

/*
 * The USB device.
 *
 * prj.conf turns CDC_ACM_SERIAL_INITIALIZE_AT_BOOT off, because Zephyr's
 * boot-time initializer registers only the first CDC ACM instance -- its own
 * comment says so -- which would leave the data port built, bound to a Zephyr
 * device, and never enumerated: writes to it would succeed and go nowhere.
 * usbd_register_all_classes() below takes both.
 *
 * 0x2fe3 is the Zephyr project's vendor ID, and 0x0001 one of its test product
 * IDs. They are correct for a personal board on a bench and wrong for anything
 * distributed; the two CDC ACM instances are told apart by their interface
 * string descriptors (the `label` properties in app.overlay), not by these.
 */
constexpr uint16_t kVendorId = 0x2fe3;
constexpr uint16_t kProductId = 0x0001;

USBD_DEVICE_DEFINE(feather_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), kVendorId, kProductId);

USBD_DESC_LANG_DEFINE(feather_lang);
USBD_DESC_MANUFACTURER_DEFINE(feather_mfr, "scratchpad");
USBD_DESC_PRODUCT_DEFINE(feather_product, "Feather Sense");
USBD_DESC_SERIAL_NUMBER_DEFINE(feather_sn);
USBD_DESC_CONFIG_DEFINE(feather_fs_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(feather_fs_config, 0, 125, &feather_fs_cfg_desc);

/*
 * Transmit ring. Two threads produce into it -- the sample transmit thread and
 * the rx thread, which answers RPC requests inline -- so puts are serialised;
 * the consumer is the CDC ACM interrupt, and a ring buffer is already safe
 * against a single consumer touching only the other index.
 */
RING_BUF_DECLARE(tx_rb, 2048);
RING_BUF_DECLARE(rx_rb, 512);
k_spinlock tx_lock;

atomic_t frames_sent;
atomic_t frames_dropped;
atomic_t rx_frames;
atomic_t rx_errors;
atomic_t dtr_asserted;

constexpr size_t kRxStackSize = 2048;
K_THREAD_STACK_DEFINE(rx_stack, kRxStackSize);
k_thread rx_thread;

/* Same priority as the transmit threads: this one answers RPC requests, which
 * a host is waiting on, and it is idle the rest of the time.
 */
constexpr int kRxPriority = 7;

void interrupt_handler(const device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	/* uart_irq_update() returns void in this Zephyr, so it cannot be the
	 * left half of the usual `while (update() && is_pending())`. The comma
	 * keeps the same shape: update the cached state, then test it.
	 */
	while (uart_irq_update(dev), uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t buf[64];
			const int read = uart_fifo_read(dev, buf, sizeof(buf));

			if (read > 0) {
				/* A full rx ring means a host is writing faster
				 * than RPC requests can be answered, which
				 * should not happen; dropping is better than
				 * blocking the ISR.
				 */
				ring_buf_put(&rx_rb, buf, read);
			}
		}

		if (uart_irq_tx_ready(dev)) {
			uint8_t *chunk = nullptr;
			const uint32_t claimed = ring_buf_get_claim(&tx_rb, &chunk, 64);

			if (claimed == 0) {
				uart_irq_tx_disable(dev);
				ring_buf_get_finish(&tx_rb, 0);
				continue;
			}

			const int written = uart_fifo_fill(dev, chunk, claimed);
			ring_buf_get_finish(&tx_rb, written < 0 ? 0 : written);
		}
	}
}

/* One decoded frame, dispatched by its channel byte. */
void dispatch_frame(const uint8_t *frame, size_t len)
{
	uint8_t decoded[codec::kRpcMaxFrameBytes + 1];

	const long decoded_len = codec::cobs_decode(frame, len, decoded, sizeof(decoded));
	if (decoded_len < 2) {
		atomic_inc(&rx_errors);
		return;
	}

	atomic_inc(&rx_frames);

	if (decoded[0] != codec::kChannelRpc) {
		/* The host has no reason to send anything else. */
		atomic_inc(&rx_errors);
		return;
	}

	uint8_t response[codec::kRpcMaxFrameBytes];
	const size_t response_len = rpc::handle(&decoded[1], static_cast<size_t>(decoded_len) - 1,
						response, sizeof(response));

	if (response_len > 0) {
		send(codec::kChannelRpc, response, response_len);
	}
}

void rx_entry(void *, void *, void *)
{
	/* Accumulates until a 0x00 delimiter. Sized for the largest thing a
	 * host sends, which is a two-byte RPC request; the slack is there so a
	 * burst of noise resynchronises at the next delimiter rather than
	 * truncating a real frame that follows it.
	 */
	uint8_t frame[codec::kRpcMaxFrameBytes];
	size_t frame_len = 0;
	bool overflowed = false;

	while (true) {
		uint8_t byte;

		if (ring_buf_get(&rx_rb, &byte, 1) != 1) {
			/* Nothing pending. This same tick is where DTR is
			 * sampled, so opening and closing the port is noticed
			 * without a second thread or a timer.
			 */
			uint32_t dtr = 0;

			(void)uart_line_ctrl_get(data_port, UART_LINE_CTRL_DTR, &dtr);
			atomic_set(&dtr_asserted, dtr != 0 ? 1 : 0);

			k_msleep(20);
			continue;
		}

		if (byte != 0) {
			if (frame_len < sizeof(frame)) {
				frame[frame_len++] = byte;
			} else {
				overflowed = true;
			}
			continue;
		}

		if (overflowed) {
			atomic_inc(&rx_errors);
		} else if (frame_len > 0) {
			dispatch_frame(frame, frame_len);
		}

		frame_len = 0;
		overflowed = false;
	}
}

} /* namespace */

void send(uint8_t channel, const uint8_t *payload, size_t len)
{
	/* The largest thing that goes out is a full batch plus its channel
	 * byte; COBS adds one byte per 254 and one more overall, and the
	 * delimiter is the last.
	 */
	constexpr size_t kMaxPlain = 1 + streams::kMaxBatchBytes;
	constexpr size_t kMaxFrame = codec::cobs_max_encoded_size(kMaxPlain) + 1;

	uint8_t plain[kMaxPlain];
	uint8_t frame[kMaxFrame];

	if (len + 1 > sizeof(plain)) {
		atomic_inc(&frames_dropped);
		return;
	}

	plain[0] = channel;
	memcpy(&plain[1], payload, len);

	const size_t encoded = codec::cobs_encode(plain, len + 1, frame, sizeof(frame) - 1);
	if (encoded == 0) {
		atomic_inc(&frames_dropped);
		return;
	}

	frame[encoded] = 0x00;
	const size_t frame_len = encoded + 1;

	/* All of the frame or none of it: a partial write would put a
	 * truncated batch on the wire, and the host would count it as a decode
	 * error rather than as the drop it actually is.
	 */
	bool queued;
	{
		k_spinlock_key_t key = k_spin_lock(&tx_lock);

		queued = ring_buf_space_get(&tx_rb) >= frame_len &&
			 ring_buf_put(&tx_rb, frame, frame_len) == frame_len;

		k_spin_unlock(&tx_lock, key);
	}

	if (!queued) {
		atomic_inc(&frames_dropped);
		return;
	}

	atomic_inc(&frames_sent);
	uart_irq_tx_enable(data_port);
}

Counters counters()
{
	return Counters{
		.frames_sent = static_cast<uint32_t>(atomic_get(&frames_sent)),
		.frames_dropped = static_cast<uint32_t>(atomic_get(&frames_dropped)),
		.rx_frames = static_cast<uint32_t>(atomic_get(&rx_frames)),
		.rx_errors = static_cast<uint32_t>(atomic_get(&rx_errors)),
	};
}

bool attached()
{
	return atomic_get(&dtr_asserted) != 0;
}

int start()
{
	int ret;

	if (!device_is_ready(data_port)) {
		LOG_ERR("cdc_acm_data is not ready");
		return -ENODEV;
	}

	ret = usbd_add_descriptor(&feather_usbd, &feather_lang);
	ret |= usbd_add_descriptor(&feather_usbd, &feather_mfr);
	ret |= usbd_add_descriptor(&feather_usbd, &feather_product);
	ret |= usbd_add_descriptor(&feather_usbd, &feather_sn);
	if (ret != 0) {
		LOG_ERR("could not add the string descriptors");
		return ret;
	}

	ret = usbd_add_configuration(&feather_usbd, USBD_SPEED_FS, &feather_fs_config);
	if (ret != 0) {
		LOG_ERR("could not add the full-speed configuration (%d)", ret);
		return ret;
	}

	/* Both CDC ACM instances: the board's console one and this
	 * application's data one.
	 */
	ret = usbd_register_all_classes(&feather_usbd, USBD_SPEED_FS, 1, nullptr);
	if (ret != 0) {
		LOG_ERR("could not register the USB classes (%d)", ret);
		return ret;
	}

	/* A CDC ACM function spans two interfaces, so the device class triple
	 * has to say "look at the interface association descriptors" rather
	 * than name a class of its own.
	 */
	usbd_device_set_code_triple(&feather_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02,
				    0x01);

	ret = usbd_init(&feather_usbd);
	if (ret != 0) {
		LOG_ERR("usbd_init failed (%d)", ret);
		return ret;
	}

	ret = usbd_enable(&feather_usbd);
	if (ret != 0) {
		LOG_ERR("usbd_enable failed (%d)", ret);
		return ret;
	}

	uart_irq_callback_set(data_port, interrupt_handler);
	uart_irq_rx_enable(data_port);

	k_thread_create(&rx_thread, rx_stack, kRxStackSize, rx_entry, nullptr, nullptr, nullptr,
			kRxPriority, 0, K_NO_WAIT);
	k_thread_name_set(&rx_thread, "usb_rx");

	return 0;
}

} /* namespace usb */
