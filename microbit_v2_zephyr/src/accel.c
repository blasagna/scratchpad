/*
 * LSM303AGR accelerometer at 100 Hz.
 *
 * This polls on a kernel timer rather than using the chip's data-ready
 * interrupt. The board makes the trigger unusable as Zephyr describes it, but
 * not for the reason the polarity of the pin suggests.
 *
 * On the V2 schematic the sensor's INT1_XL drives the base of T7, a DTC143E
 * digital NPN, whose collector is COMBINED_SENSOR_INT on P0.25. (The
 * magnetometer's DRDY drives T5 onto the same net, as does the interface MCU.)
 * That common-emitter stage inverts, so the chip's default ACTIVE-HIGH INT1 is
 * already the right polarity here -- the board is what makes the line
 * active-low and open-collector, and the LSM303AGR's own polarity bit should be
 * left alone.
 *
 * What is missing is a pull-up. Nothing on the board pulls this net high, and
 * Zephyr's board DTS declares irq-gpios without GPIO_PULL_UP, so lis2dh's bare
 * gpio_pin_configure_dt(..., GPIO_INPUT) leaves the pin floating. The first
 * data-ready pulls it to 0 V, the read releases T7, and the floating input just
 * stays there: measured, P0.25 sits low forever and no further edge occurs.
 * Polarity is provably not the blocker -- int1-gpio-config defaults to
 * EDGE_BOTH, which would have caught an edge in either direction.
 *
 * An overlay setting GPIO_PULL_UP on the accel's irq-gpios (plus
 * int1-gpio-config = LEVEL_LOW) would make the trigger work. Polling is kept
 * anyway because the line is shared: the interface MCU can assert it while the
 * accelerometer is idle, and hold it low across our own edges. A 6-byte burst
 * at 100 Hz is ~2 % of a 400 kHz I2C bus. See README.md, "known limitations".
 */

#include "accel.h"

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(accel, LOG_LEVEL_DBG);

#define ACCEL_SAMPLE_RATE_HZ 100
#define ACCEL_PERIOD_MS      (1000 / ACCEL_SAMPLE_RATE_HZ)

/* One second of headroom. The BLE thread drains far faster than this when a
 * central is subscribed; the depth only matters while nobody is listening.
 */
#define ACCEL_QUEUE_DEPTH 128

#define ACCEL_STACK_SIZE 1024
#define ACCEL_PRIORITY   5

K_MSGQ_DEFINE(accel_msgq, sizeof(struct accel_sample), ACCEL_QUEUE_DEPTH, 4);

static const struct device *const accel_dev = DEVICE_DT_GET(DT_ALIAS(accel0));
static K_TIMER_DEFINE(accel_timer, NULL, NULL);
static uint32_t dropped;
static uint32_t overruns;

/* The sensor API reports m/s^2. Convert via micro-m/s^2 so the whole thing stays
 * in integer arithmetic: 1 g = 9.80665 m/s^2.
 */
static int16_t to_milli_g(const struct sensor_value *val)
{
	int64_t micro_m_s2 = sensor_value_to_micro(val);

	return (int16_t)CLAMP((micro_m_s2 * 1000) / 9806650, INT16_MIN, INT16_MAX);
}

static void sample_once(void)
{
	struct sensor_value val[3];
	struct accel_sample sample;
	int err;

	err = sensor_sample_fetch_chan(accel_dev, SENSOR_CHAN_ACCEL_XYZ);
	if (err == -ENODATA) {
		/* The chip had no new sample: its data-ready bit was still clear. Normal
		 * for a poller -- it happens on the first read after the ODR change, and
		 * whenever our 10 ms tick drifts just ahead of the sensor's own clock.
		 */
		LOG_DBG("no new sample");
		return;
	}
	if (err) {
		LOG_ERR("fetch failed (%d)", err);
		return;
	}

	err = sensor_channel_get(accel_dev, SENSOR_CHAN_ACCEL_XYZ, val);
	if (err) {
		LOG_ERR("channel get failed (%d)", err);
		return;
	}

	sample.t_ms = k_uptime_get_32();
	sample.x = to_milli_g(&val[0]);
	sample.y = to_milli_g(&val[1]);
	sample.z = to_milli_g(&val[2]);

	/* Drop the oldest sample rather than the newest: a stalled consumer should
	 * cost us history, not the live signal.
	 */
	if (k_msgq_put(&accel_msgq, &sample, K_NO_WAIT) != 0) {
		struct accel_sample discard;

		(void)k_msgq_get(&accel_msgq, &discard, K_NO_WAIT);
		(void)k_msgq_put(&accel_msgq, &sample, K_NO_WAIT);
		dropped++;
	}
}

static void accel_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_timer_start(&accel_timer, K_MSEC(ACCEL_PERIOD_MS), K_MSEC(ACCEL_PERIOD_MS));

	for (;;) {
		/* Counts every expiry, so the period stays anchored to the timer
		 * instead of drifting by however long each read takes.
		 */
		uint32_t ticks = k_timer_status_sync(&accel_timer);

		if (ticks > 1) {
			overruns += ticks - 1;
		}

		sample_once();
	}
}

K_THREAD_DEFINE(accel_tid, ACCEL_STACK_SIZE, accel_thread, NULL, NULL, NULL, ACCEL_PRIORITY, 0,
		K_TICKS_FOREVER);

int accel_start(void)
{
	struct sensor_value odr = {.val1 = ACCEL_SAMPLE_RATE_HZ, .val2 = 0};
	int err;

	if (!device_is_ready(accel_dev)) {
		LOG_ERR("%s not ready", accel_dev->name);
		return -ENODEV;
	}

	/* Match the chip's output rate to the poll rate, so each read returns a
	 * fresh sample rather than repeating one.
	 */
	err = sensor_attr_set(accel_dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY,
			      &odr);
	if (err) {
		LOG_ERR("cannot set %d Hz ODR (%d)", ACCEL_SAMPLE_RATE_HZ, err);
		return err;
	}

	k_thread_start(accel_tid);
	LOG_INF("accelerometer polling at %d Hz", ACCEL_SAMPLE_RATE_HZ);
	return 0;
}

uint32_t accel_dropped(void)
{
	return dropped;
}

uint32_t accel_overruns(void)
{
	return overruns;
}
