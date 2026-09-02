/*
 * The 208 Hz path: drain the chip's FIFO, batch, publish.
 * Copyright (c) 2026 Bob DiMaiolo. SPDX-License-Identifier: Apache-2.0
 *
 * Nothing here touches a sample's bytes. The FIFO burst lands directly in the
 * batch buffer behind the header, because the chip is little-endian, the SoC is
 * little-endian and the wire is little-endian, and because the IMU stream
 * carries the sensor's own int16 registers rather than converted units. That is
 * what makes a DMA'd I2C read worth having, and it is why the `get scale` RPC
 * is load-bearing rather than decorative.
 */

#include "imu.hpp"

#include "codec.hpp"
#include "streams.hpp"

#include <lsm6ds3trc.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(imu, LOG_LEVEL_INF);

namespace imu
{
namespace
{

#define IMU_NODE DT_ALIAS(imu0)

const device *const dev = DEVICE_DT_GET(IMU_NODE);

constexpr size_t kStackSize = 2048;
K_THREAD_STACK_DEFINE(stack, kStackSize);
k_thread thread;

/* The highest-priority sampling thread: it has the tightest deadline and the
 * least work per wake.
 */
constexpr int kPriority = 5;

/*
 * How often to drain when there is no interrupt to wake on. The chip's FIFO
 * watermark is CONFIG_LSM6DS3TRC_FIFO_WATERMARK_SAMPLES, so draining at that
 * cadence keeps the FIFO roughly one watermark deep. The *spacing* of samples
 * comes from the chip's own clock either way -- this only sets how long a
 * sample waits before it is sent.
 */
constexpr int kDrainPeriodMs = (CONFIG_LSM6DS3TRC_FIFO_WATERMARK_SAMPLES * 1000 + 207) / 208;

/*
 * If more than this many samples are waiting, the FIFO is a backlog rather than
 * a buffer: something stalled. Drop it and restart rather than transmitting a
 * burst of stale samples carrying plausible-looking back-dated timestamps --
 * the CircuitPython port's schedule-from-the-deadline rule, applied to a
 * hardware queue.
 */
constexpr uint8_t kStallSamples = 96;

/* One drain reads at most this many samples, which is also the most a single
 * batch can hold.
 */
constexpr uint8_t kMaxDrainSamples = streams::kMaxImuBatchSamples;

k_timer drain_timer;

uint32_t total_samples;
uint32_t total_batches;
uint32_t total_overruns;
uint32_t total_stall_flushes;

#if DT_NODE_HAS_PROP(IMU_NODE, irq_gpios)
#define IMU_HAS_TRIGGER 1
K_SEM_DEFINE(watermark_sem, 0, 1);

void watermark_handler(const device *, const sensor_trigger *)
{
	k_sem_give(&watermark_sem);
}
#endif

/*
 * Publish `count` samples as one batch.
 *
 * `t_ms` is the uptime at the FIRST sample. The drain happened at `now_ms`, and
 * the newest sample in the FIFO is the one that had just arrived, so the first
 * is (count - 1) periods older. That is an estimate of a real sample instant
 * rather than a transmit time, which is the whole reason the header carries
 * `t_ms` and `period_us` instead of leaving the host to infer both from
 * arrival.
 */
void publish(const lsm6ds3trc_sample *samples, uint8_t count, uint32_t now_ms, uint32_t period_us)
{
	const uint32_t span_ms = (static_cast<uint32_t>(count - 1) * period_us) / 1000U;

	streams::emit(codec::kStreamImu, now_ms - span_ms, static_cast<uint16_t>(period_us), count,
		      samples, count * codec::kImuSampleBytes);

	total_samples += count;
	total_batches++;
}

void drain()
{
	lsm6ds3trc_sample samples[kMaxDrainSamples];
	const uint32_t period_us = lsm6ds3trc_sample_period_us(dev);

	if (lsm6ds3trc_fifo_overrun(dev)) {
		total_overruns++;
	}

	/* The stall clamp. A FIFO this deep means nothing drained it for a
	 * quarter of a second or more, and catching up would put a burst of
	 * stale samples on the wire carrying plausible-looking back-dated
	 * timestamps. Drop the backlog instead: the gap is then visible to the
	 * host as a jump in `seq`, which is honest, rather than as data that
	 * looks fine and is not.
	 */
	const int level = lsm6ds3trc_fifo_level(dev);
	if (level > kStallSamples) {
		LOG_WRN("FIFO %d samples deep; dropping the backlog", level);
		lsm6ds3trc_fifo_flush(dev);
		total_stall_flushes++;
		return;
	}

	/* Loop until the FIFO is empty: one wake may cover several batches if a
	 * lower-priority thread held the CPU longer than one watermark.
	 */
	while (true) {
		const uint8_t cap = streams::imu_batch_samples();
		const int got = lsm6ds3trc_fifo_read(dev, samples, cap);

		if (got <= 0) {
			if (got < 0) {
				LOG_WRN("FIFO read failed (%d)", got);
			}
			return;
		}

		publish(samples, static_cast<uint8_t>(got), k_uptime_get_32(), period_us);

		if (static_cast<uint8_t>(got) < cap) {
			/* A short read means the FIFO is drained. */
			return;
		}
	}
}

void entry(void *, void *, void *)
{
	/* Discard whatever accumulated between the driver's init and this
	 * thread's first run. The chip starts converting as soon as its ODR is
	 * set, so by now the FIFO holds a second or so of boot -- samples whose
	 * real instants are long past, and which publish() would back-date from
	 * the drain time and so timestamp wrongly. Dropping them costs a
	 * fraction of a second of data at power-on and is the difference
	 * between `fs imu` reporting one overrun forever and reporting none.
	 */
	lsm6ds3trc_fifo_flush(dev);

#ifdef IMU_HAS_TRIGGER
	static const sensor_trigger trig = {
		.type = SENSOR_TRIG_FIFO_WATERMARK,
		.chan = SENSOR_CHAN_ALL,
	};

	if (sensor_trigger_set(dev, &trig, watermark_handler) == 0) {
		LOG_INF("draining the FIFO on the INT1 watermark");

		while (true) {
			/* The timeout is a safety net, not the schedule: if the
			 * interrupt stops arriving the stream degrades to the
			 * timer behaviour rather than stopping.
			 */
			k_sem_take(&watermark_sem, K_MSEC(kDrainPeriodMs * 4));
			drain();
		}
	}

	LOG_WRN("INT1 trigger refused; falling back to the timer");
#endif

	/* k_timer's periodic mode reschedules from the deadline rather than
	 * from the wake, so a late drain does not push the next one out --
	 * `due += interval`, never `due = now + interval`. The stall clamp in
	 * drain() handles the case where a drain fell so far behind that
	 * catching up would mean transmitting stale samples.
	 */
	LOG_INF("draining the FIFO every %d ms", kDrainPeriodMs);
	k_timer_start(&drain_timer, K_MSEC(kDrainPeriodMs), K_MSEC(kDrainPeriodMs));

	while (true) {
		k_timer_status_sync(&drain_timer);
		drain();
	}
}

} /* namespace */

int start()
{
	if (!device_is_ready(dev)) {
		LOG_ERR("%s is not ready", dev->name);
		return -ENODEV;
	}

	k_timer_init(&drain_timer, nullptr, nullptr);

	k_thread_create(&thread, stack, kStackSize, entry, nullptr, nullptr, nullptr, kPriority, 0,
			K_NO_WAIT);
	k_thread_name_set(&thread, "imu");

	return 0;
}

Stats stats()
{
	return Stats{
		.samples = total_samples,
		.batches = total_batches,
		.overruns = total_overruns,
		.stall_flushes = total_stall_flushes,
		.who_am_i = lsm6ds3trc_who_am_i(dev),
	};
}

} /* namespace imu */
