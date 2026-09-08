/*
 * Stream fan-out: one encoder, both transports.
 * Copyright (c) 2026 Bob DiMaiolo. SPDX-License-Identifier: Apache-2.0
 *
 * Producers call emit(); this module stamps the per-stream sequence number,
 * packs the batch header, and hands the identical bytes to the BLE and the USB
 * transmit queues. Neither transport gets its own encoder, and `stream_id`
 * stays in the header even on BLE -- where the characteristic already implies
 * it -- so that the bytes on the two links are the same bytes.
 */

#ifndef FEATHER_SENSE_STREAMS_HPP_
#define FEATHER_SENSE_STREAMS_HPP_

#include <stddef.h>
#include <stdint.h>

namespace streams
{

/*
 * The largest batch any stream produces: a 10-byte header plus 19 IMU samples,
 * which is what a 247-byte ATT MTU leaves room for. Rounded up to a multiple of
 * four so the queue element does not carry tail padding.
 */
constexpr size_t kMaxBatchBytes = 240;

/* The cap on IMU samples per batch when no BLE MTU has been negotiated. */
constexpr uint8_t kMaxImuBatchSamples = 19;

/*
 * The device timestamp every batch header carries: `k_uptime_get_32()` plus the
 * offset below. Every producer calls this rather than the kernel directly, so
 * that all five streams share one clock and one offset -- which is what makes
 * the 32-bit wrap reachable at all.
 */
uint32_t now_ms();

/*
 * Shift that clock. A test hook, and the only way to reach the `t_ms` wrap
 * without waiting 49.7 days for it: set the offset a few seconds short of
 * 2^32 and every stream crosses the boundary together, on the shipped image
 * rather than on a special build. Nothing in normal operation calls this and
 * the offset starts at 0.
 */
void set_clock_offset_ms(uint32_t offset);
uint32_t clock_offset_ms();

/*
 * Stamp, pack and publish one batch. `count` samples of `samples_bytes` total
 * are copied in behind the header. Silently drops the batch if its stream is
 * disabled, if it does not fit, or if a transport's queue is full -- a drop is
 * visible to the host as a gap in the stream's `seq`, which is what that field
 * is for.
 */
void emit(uint8_t stream_id, uint32_t t_ms, uint16_t period_us, uint8_t count, const void *samples,
	  size_t samples_bytes);

/*
 * Record that a batch this stream would have emitted was lost before it got
 * here: a sample whose sensor could not be read, a half-filled batch discarded,
 * a FIFO backlog flushed. Advances the stream's sequence number without sending
 * anything, so the host sees the gap.
 *
 * Without this a producer-side drop is invisible: emit() is what stamps `seq`,
 * so a batch dropped before reaching it consumes no number and leaves the
 * sequence contiguous across the hole. Measured, on the board -- eleven seconds
 * of missing env samples with `seq gaps 0`. See README.md, "and a drop that
 * never reaches emit() has to say so itself".
 *
 * Call it from the same thread that emits that stream. That is what keeps the
 * single-writer rule emit() relies on, and it is why this takes no lock.
 */
void drop(uint8_t stream_id);

/* Streaming enable, per stream (RPC opcode 0x02). All streams start enabled. */
bool set_enabled(uint8_t stream_id, bool enable);
bool enabled(uint8_t stream_id);

/*
 * How many IMU samples fit in one notification at the currently negotiated MTU.
 * ble.cpp recomputes this when a subscription arrives; until then it is
 * kMaxImuBatchSamples, which is also what the serial path uses -- the CDC bulk
 * endpoint is not the constraint on that link.
 */
uint8_t imu_batch_samples();
void set_imu_batch_samples(uint8_t samples);

/* Counters, for the `fs stats` shell command. */
struct Counters {
	uint32_t emitted;
	/* Batches lost before emit(), counted by drop(). A different thing from
	 * the two transport counters below, which count batches that were built
	 * and then had nowhere to go.
	 */
	uint32_t dropped_source;
	uint32_t dropped_ble;
	uint32_t dropped_usb;
	uint32_t dropped_oversize;
};

Counters counters();

/* Starts the two transmit threads. */
void start();

} /* namespace streams */

#endif /* FEATHER_SENSE_STREAMS_HPP_ */
