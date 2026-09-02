/*
 * Copyright (c) 2026 Bob DiMaiolo
 * SPDX-License-Identifier: Apache-2.0
 */

#include "streams.hpp"

#include "ble.hpp"
#include "codec.hpp"
#include "usb.hpp"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(streams, LOG_LEVEL_INF);

namespace streams
{
namespace
{

struct Batch {
	uint16_t len;
	uint8_t data[kMaxBatchBytes];
};

/*
 * Eight batches deep on each transport. At 208 Hz in 19-sample batches the IMU
 * produces about 11 per second, so this is roughly 0.7 s of slack -- enough to
 * ride out a connection-interval hiccup, short enough that a real stall is
 * reported as a drop rather than hidden as latency.
 */
constexpr size_t kQueueDepth = 8;

K_MSGQ_DEFINE(ble_txq, sizeof(Batch), kQueueDepth, 4);
K_MSGQ_DEFINE(usb_txq, sizeof(Batch), kQueueDepth, 4);

constexpr size_t kTxStackSize = 2048;

K_THREAD_STACK_DEFINE(ble_tx_stack, kTxStackSize);
K_THREAD_STACK_DEFINE(usb_tx_stack, kTxStackSize);

k_thread ble_tx_thread;
k_thread usb_tx_thread;

/* Both transmit threads sit above the env and battery threads so a full queue
 * drains ahead of new low-rate work.
 */
constexpr int kTxPriority = 7;

/* One sequence counter per stream, and exactly one thread emits each stream --
 * imu, magn, env and battery have a thread apiece, and button events arrive on
 * the input subsystem's. So each element has a single writer and the increment
 * in emit() needs no lock. Emitting one stream from two places would break that
 * silently, as duplicated sequence numbers the host would read as a stall.
 */
uint16_t seq[codec::kStreamCount];
atomic_t stream_enabled[codec::kStreamCount];
atomic_t imu_batch = kMaxImuBatchSamples;

atomic_t emitted;
atomic_t dropped_ble;
atomic_t dropped_usb;
atomic_t dropped_oversize;

bool valid_stream(uint8_t stream_id)
{
	return stream_id >= codec::kStreamMin && stream_id <= codec::kStreamMax;
}

size_t index_of(uint8_t stream_id)
{
	return static_cast<size_t>(stream_id - codec::kStreamMin);
}

void ble_tx_entry(void *, void *, void *)
{
	Batch batch;

	while (true) {
		k_msgq_get(&ble_txq, &batch, K_FOREVER);
		ble::notify(batch.data, batch.len);
	}
}

void usb_tx_entry(void *, void *, void *)
{
	Batch batch;

	while (true) {
		k_msgq_get(&usb_txq, &batch, K_FOREVER);
		usb::send(codec::kChannelSamples, batch.data, batch.len);
	}
}

} /* namespace */

void emit(uint8_t stream_id, uint32_t t_ms, uint16_t period_us, uint8_t count, const void *samples,
	  size_t samples_bytes)
{
	if (!valid_stream(stream_id) || !enabled(stream_id) || count == 0) {
		return;
	}

	if (codec::kBatchHeaderBytes + samples_bytes > kMaxBatchBytes) {
		atomic_inc(&dropped_oversize);
		LOG_WRN("stream %u batch of %zu bytes exceeds the %zu-byte limit", stream_id,
			samples_bytes, kMaxBatchBytes);
		return;
	}

	Batch batch;
	const codec::BatchHeader header = {
		.t_ms = t_ms,
		.seq = seq[index_of(stream_id)]++,
		.period_us = period_us,
		.stream_id = stream_id,
		.count = count,
	};

	codec::pack_batch_header(batch.data, header);
	memcpy(&batch.data[codec::kBatchHeaderBytes], samples, samples_bytes);
	batch.len = static_cast<uint16_t>(codec::kBatchHeaderBytes + samples_bytes);

	atomic_inc(&emitted);

	/* Only queue for a transport that has somewhere to put it. Queueing for
	 * an unsubscribed link would fill the queue with batches nobody asked
	 * for and then drop the ones somebody did.
	 */
	if (ble::subscribed(stream_id) && k_msgq_put(&ble_txq, &batch, K_NO_WAIT) != 0) {
		atomic_inc(&dropped_ble);
	}

	if (usb::attached() && k_msgq_put(&usb_txq, &batch, K_NO_WAIT) != 0) {
		atomic_inc(&dropped_usb);
	}
}

bool set_enabled(uint8_t stream_id, bool enable)
{
	if (!valid_stream(stream_id)) {
		return false;
	}

	atomic_set(&stream_enabled[index_of(stream_id)], enable ? 1 : 0);

	return enable;
}

bool enabled(uint8_t stream_id)
{
	if (!valid_stream(stream_id)) {
		return false;
	}

	return atomic_get(&stream_enabled[index_of(stream_id)]) != 0;
}

uint8_t imu_batch_samples()
{
	return static_cast<uint8_t>(atomic_get(&imu_batch));
}

void set_imu_batch_samples(uint8_t samples)
{
	if (samples == 0) {
		samples = 1;
	}
	if (samples > kMaxImuBatchSamples) {
		samples = kMaxImuBatchSamples;
	}

	atomic_set(&imu_batch, samples);
}

Counters counters()
{
	return Counters{
		.emitted = static_cast<uint32_t>(atomic_get(&emitted)),
		.dropped_ble = static_cast<uint32_t>(atomic_get(&dropped_ble)),
		.dropped_usb = static_cast<uint32_t>(atomic_get(&dropped_usb)),
		.dropped_oversize = static_cast<uint32_t>(atomic_get(&dropped_oversize)),
	};
}

void start()
{
	for (size_t i = 0; i < codec::kStreamCount; i++) {
		atomic_set(&stream_enabled[i], 1);
	}

	k_thread_create(&ble_tx_thread, ble_tx_stack, kTxStackSize, ble_tx_entry, nullptr, nullptr,
			nullptr, kTxPriority, 0, K_NO_WAIT);
	k_thread_name_set(&ble_tx_thread, "tx_ble");

	k_thread_create(&usb_tx_thread, usb_tx_stack, kTxStackSize, usb_tx_entry, nullptr, nullptr,
			nullptr, kTxPriority, 0, K_NO_WAIT);
	k_thread_name_set(&usb_tx_thread, "tx_usb");
}

} /* namespace streams */
