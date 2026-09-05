/*
 * LSM6DS3TR-C / LSM6DS33 6-axis IMU driver.
 *
 * Copyright (c) 2026 Bob DiMaiolo
 * SPDX-License-Identifier: Apache-2.0
 *
 * A thin Zephyr driver over ST's own vendored register driver (stmemsc). Three
 * things live here that the vendor code does not provide: the bus shim, the
 * init path, and a FIFO drain that hands the application raw records.
 */

#define DT_DRV_COMPAT scratchpad_lsm6ds3trc

#include "lsm6ds3trc.h"
#include "lsm6ds3trc_priv.h"

#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <stmemsc.h>

LOG_MODULE_REGISTER(LSM6DS3TRC, CONFIG_LSM6DS3TRC_LOG_LEVEL);

/* The LSM6DS33's WHO_AM_I. The LSM6DS3TR-C's (0x6a) is LSM6DS3TR_C_ID, from
 * ST's own header. Both are accepted: Adafruit swapped one part for the other
 * in January 2024 without changing the board, and they share every register
 * this driver touches.
 */
#define LSM6DS33_ID 0x69U

/* One FIFO record is gyro x,y,z then accel x,y,z, six 16-bit words. */
#define SAMPLE_WORDS 6U
#define SAMPLE_BYTES (SAMPLE_WORDS * 2U)

/* lsm6ds3tr_c_fifo_raw_data_get() takes a uint8_t length, so one burst can
 * carry at most 255 bytes -- 21 records. 20 is the round number below that.
 */
#define MAX_BURST_SAMPLES 20U

/* The one ODR this application runs at. 208 Hz was chosen against a
 * measurement rather than a datasheet: at rest, doubling 104 -> 208 costs
 * 1.42x of gyro noise and only 1.05x of accel noise, and going higher buys
 * another root-two of gyro noise for nothing. See README.md, "streams".
 */
#define SAMPLE_PERIOD_US 4808U /* 1e6 / 208, rounded */

/* ST's own sensitivities, from lsm6ds3tr-c_reg.c:102 and :127, carried into
 * integer arithmetic: 0.061 mg/LSB at +-2 g and 8.75 mdps/LSB at +-250 dps,
 * expressed as nano-SI per LSB scaled by 100. These are the same numbers the
 * `get scale` RPC reports to the host, which is what keeps the device's
 * diagnostic conversion and the host's streaming conversion from drifting
 * apart. The gyro factor is rounded to the nearest 0.01 nrad/s because the
 * degree-to-radian conversion is irrational and an exact rational would be a
 * fiction.
 */
#define ACCEL_NANO_MS2_PER_LSB_X100 59820565LL
#define GYRO_NANO_RAD_PER_LSB_X100  15271631LL

/* The chip is little-endian and so is the nRF52840, so a FIFO burst lands in a
 * struct of int16s with no byte swapping, and goes on the (little-endian) wire
 * the same way. Nothing in the 208 Hz path touches a sample's bytes.
 */
BUILD_ASSERT(sizeof(struct lsm6ds3trc_sample) == SAMPLE_BYTES,
	     "a FIFO record must be exactly six packed int16s");

static void nano_to_sensor_value(int64_t nano, struct sensor_value *val)
{
	val->val1 = (int32_t)(nano / 1000000000LL);
	val->val2 = (int32_t)((nano % 1000000000LL) / 1000LL);
}

/*
 * Line the FIFO read pointer up with the start of a record.
 *
 * FIFO_PATTERN reports the index, within the six-word pattern, of the word the
 * next read would return. It is normally 0 -- every drain reads whole records --
 * but it is not after an overrun, and a misaligned read would silently return
 * samples with their axes rotated, which is the kind of fault that looks like
 * a mounting error rather than a bug.
 */
static int fifo_align(struct lsm6ds3trc_data *data)
{
	uint8_t discard[SAMPLE_BYTES];
	uint16_t pattern;
	uint16_t offset;
	int ret;

	ret = lsm6ds3tr_c_fifo_pattern_get(&data->ctx, &pattern);
	if (ret < 0) {
		return -EIO;
	}

	offset = pattern % SAMPLE_WORDS;
	if (offset == 0U) {
		return 0;
	}

	LOG_WRN("FIFO misaligned at word %u of the pattern; discarding %u", offset,
		SAMPLE_WORDS - offset);

	ret = lsm6ds3tr_c_fifo_raw_data_get(&data->ctx, discard,
					    (uint8_t)((SAMPLE_WORDS - offset) * 2U));
	return ret < 0 ? -EIO : 0;
}

int lsm6ds3trc_fifo_read(const struct device *dev, struct lsm6ds3trc_sample *out,
			 uint8_t max_samples)
{
	struct lsm6ds3trc_data *data = dev->data;
	uint16_t level_words;
	uint8_t available;
	uint8_t wanted;
	uint8_t done = 0;
	int ret;

	if (out == NULL || max_samples == 0U) {
		return -EINVAL;
	}

	ret = fifo_align(data);
	if (ret < 0) {
		return ret;
	}

	/* Read the level *after* aligning: the discard above consumed words. */
	ret = lsm6ds3tr_c_fifo_data_level_get(&data->ctx, &level_words);
	if (ret < 0) {
		return -EIO;
	}

	available = (uint8_t)MIN(level_words / SAMPLE_WORDS, (uint16_t)UINT8_MAX);
	wanted = MIN(available, max_samples);

	while (done < wanted) {
		uint8_t chunk = MIN((uint8_t)(wanted - done), (uint8_t)MAX_BURST_SAMPLES);

		/* One I2C burst per chunk. FIFO_DATA_OUT_L/H is a two-register
		 * window that the chip re-presents on every read while
		 * auto-increment is on, so a burst walks the FIFO rather than
		 * running off the end of the register map.
		 */
		ret = lsm6ds3tr_c_fifo_raw_data_get(&data->ctx, (uint8_t *)&out[done],
						    (uint8_t)(chunk * SAMPLE_BYTES));
		if (ret < 0) {
			return done > 0 ? done : -EIO;
		}

		done += chunk;
	}

	return done;
}

int lsm6ds3trc_fifo_level(const struct device *dev)
{
	struct lsm6ds3trc_data *data = dev->data;
	uint16_t level_words;

	if (lsm6ds3tr_c_fifo_data_level_get(&data->ctx, &level_words) < 0) {
		return -EIO;
	}

	return level_words / SAMPLE_WORDS;
}

int lsm6ds3trc_fifo_flush(const struct device *dev)
{
	struct lsm6ds3trc_data *data = dev->data;
	int ret;

	/* Bypass empties the FIFO; going back to stream restarts it. */
	ret = lsm6ds3tr_c_fifo_mode_set(&data->ctx, LSM6DS3TR_C_BYPASS_MODE);
	if (ret < 0) {
		return -EIO;
	}

	ret = lsm6ds3tr_c_fifo_mode_set(&data->ctx, LSM6DS3TR_C_STREAM_MODE);
	return ret < 0 ? -EIO : 0;
}

uint32_t lsm6ds3trc_sample_period_us(const struct device *dev)
{
	ARG_UNUSED(dev);

	return SAMPLE_PERIOD_US;
}

uint8_t lsm6ds3trc_who_am_i(const struct device *dev)
{
	const struct lsm6ds3trc_data *data = dev->data;

	return data->who_am_i;
}

bool lsm6ds3trc_fifo_overrun(const struct device *dev)
{
	struct lsm6ds3trc_data *data = dev->data;
	lsm6ds3tr_c_fifo_status2_t status;

	if (lsm6ds3tr_c_read_reg(&data->ctx, LSM6DS3TR_C_FIFO_STATUS2, (uint8_t *)&status, 1) < 0) {
		return false;
	}

	return status.over_run != 0U;
}

/* --- the ordinary sensor API ------------------------------------------------
 *
 * For `sensor get imu0` at the shell and for nothing else. It reads the output
 * registers directly rather than the FIFO, so it neither disturbs nor is
 * disturbed by the streaming path.
 */

static int lsm6ds3trc_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	struct lsm6ds3trc_data *data = dev->data;
	uint8_t raw[SAMPLE_BYTES];

	if (chan != SENSOR_CHAN_ALL && chan != SENSOR_CHAN_ACCEL_XYZ &&
	    chan != SENSOR_CHAN_GYRO_XYZ) {
		return -ENOTSUP;
	}

	/* One burst from OUTX_L_G covers both halves, in the order the FIFO
	 * uses -- gyro then accel.
	 */
	if (lsm6ds3tr_c_read_reg(&data->ctx, LSM6DS3TR_C_OUTX_L_G, raw, sizeof(raw)) < 0) {
		return -EIO;
	}

	memcpy(data->last.gyro, &raw[0], sizeof(data->last.gyro));
	memcpy(data->last.accel, &raw[6], sizeof(data->last.accel));

	return 0;
}

static int lsm6ds3trc_channel_get(const struct device *dev, enum sensor_channel chan,
				  struct sensor_value *val)
{
	const struct lsm6ds3trc_data *data = dev->data;
	const int16_t *src;
	int64_t per_lsb_x100;
	size_t axis;
	size_t count;

	switch (chan) {
	case SENSOR_CHAN_ACCEL_X:
	case SENSOR_CHAN_ACCEL_Y:
	case SENSOR_CHAN_ACCEL_Z:
	case SENSOR_CHAN_ACCEL_XYZ:
		src = data->last.accel;
		per_lsb_x100 = ACCEL_NANO_MS2_PER_LSB_X100;
		axis = (chan == SENSOR_CHAN_ACCEL_XYZ) ? 0 : (size_t)(chan - SENSOR_CHAN_ACCEL_X);
		count = (chan == SENSOR_CHAN_ACCEL_XYZ) ? 3 : 1;
		break;
	case SENSOR_CHAN_GYRO_X:
	case SENSOR_CHAN_GYRO_Y:
	case SENSOR_CHAN_GYRO_Z:
	case SENSOR_CHAN_GYRO_XYZ:
		src = data->last.gyro;
		per_lsb_x100 = GYRO_NANO_RAD_PER_LSB_X100;
		axis = (chan == SENSOR_CHAN_GYRO_XYZ) ? 0 : (size_t)(chan - SENSOR_CHAN_GYRO_X);
		count = (chan == SENSOR_CHAN_GYRO_XYZ) ? 3 : 1;
		break;
	default:
		return -ENOTSUP;
	}

	for (size_t i = 0; i < count; i++) {
		nano_to_sensor_value((int64_t)src[axis + i] * per_lsb_x100 / 100LL, &val[i]);
	}

	return 0;
}

static DEVICE_API(sensor, lsm6ds3trc_api) = {
	.sample_fetch = lsm6ds3trc_sample_fetch,
	.channel_get = lsm6ds3trc_channel_get,
#ifdef CONFIG_LSM6DS3TRC_TRIGGER
	.trigger_set = lsm6ds3trc_trigger_set,
#endif
};

/* --- init ------------------------------------------------------------------- */

static int lsm6ds3trc_init(const struct device *dev)
{
	const struct lsm6ds3trc_config *cfg = dev->config;
	struct lsm6ds3trc_data *data = dev->data;
	uint8_t reset = 1U;
	int ret;

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		LOG_ERR("I2C bus %s is not ready", cfg->i2c.bus->name);
		return -ENODEV;
	}

	data->dev = dev;
	data->ctx.read_reg = (stmdev_read_ptr)stmemsc_i2c_read;
	data->ctx.write_reg = (stmdev_write_ptr)stmemsc_i2c_write;
	data->ctx.mdelay = (stmdev_mdelay_ptr)stmemsc_mdelay;
	data->ctx.handle = (void *)&cfg->i2c;

	if (lsm6ds3tr_c_device_id_get(&data->ctx, &data->who_am_i) < 0) {
		LOG_ERR("could not read WHO_AM_I");
		return -EIO;
	}

	if (data->who_am_i != LSM6DS3TR_C_ID && data->who_am_i != LSM6DS33_ID) {
		LOG_ERR("WHO_AM_I is 0x%02x, expected 0x%02x (LSM6DS3TR-C) or 0x%02x (LSM6DS33)",
			data->who_am_i, LSM6DS3TR_C_ID, LSM6DS33_ID);
		return -ENODEV;
	}

	LOG_INF("found %s (WHO_AM_I 0x%02x)",
		data->who_am_i == LSM6DS3TR_C_ID ? "LSM6DS3TR-C" : "LSM6DS33", data->who_am_i);

	/* Start from a known state rather than from whatever the last firmware
	 * left behind: this board is reflashed without a power cycle.
	 */
	if (lsm6ds3tr_c_reset_set(&data->ctx, 1) < 0) {
		return -EIO;
	}
	for (int i = 0; reset != 0U && i < 100; i++) {
		k_msleep(1);
		if (lsm6ds3tr_c_reset_get(&data->ctx, &reset) < 0) {
			return -EIO;
		}
	}
	if (reset != 0U) {
		LOG_ERR("software reset did not complete");
		return -EIO;
	}

	/* Auto-increment is what makes a multi-byte burst read walk the
	 * register map -- and, at FIFO_DATA_OUT, what makes it walk the FIFO.
	 * It is on after reset; setting it makes the dependency explicit rather
	 * than inherited.
	 */
	ret = lsm6ds3tr_c_auto_increment_set(&data->ctx, 1);
	/* Block data update: an output register pair cannot be refreshed
	 * between its low and high halves. Matters only for the sensor-API
	 * path, which reads those registers; the FIFO is unaffected.
	 */
	ret |= lsm6ds3tr_c_block_data_update_set(&data->ctx, 1);

	ret |= lsm6ds3tr_c_xl_full_scale_set(&data->ctx, LSM6DS3TR_C_2g);
	ret |= lsm6ds3tr_c_gy_full_scale_set(&data->ctx, LSM6DS3TR_C_250dps);

	/* FIFO before ODR: with the FIFO configured first, the very first
	 * samples the chip produces are already being stored.
	 */
	ret |= lsm6ds3tr_c_fifo_xl_batch_set(&data->ctx, LSM6DS3TR_C_FIFO_XL_NO_DEC);
	ret |= lsm6ds3tr_c_fifo_gy_batch_set(&data->ctx, LSM6DS3TR_C_FIFO_GY_NO_DEC);
	ret |= lsm6ds3tr_c_fifo_watermark_set(&data->ctx, CONFIG_LSM6DS3TRC_FIFO_WATERMARK_SAMPLES *
								  SAMPLE_WORDS);
	ret |= lsm6ds3tr_c_fifo_data_rate_set(&data->ctx, LSM6DS3TR_C_FIFO_208Hz);
	ret |= lsm6ds3tr_c_fifo_mode_set(&data->ctx, LSM6DS3TR_C_STREAM_MODE);

	ret |= lsm6ds3tr_c_xl_data_rate_set(&data->ctx, LSM6DS3TR_C_XL_ODR_208Hz);
	ret |= lsm6ds3tr_c_gy_data_rate_set(&data->ctx, LSM6DS3TR_C_GY_ODR_208Hz);

	if (ret < 0) {
		LOG_ERR("could not configure the chip");
		return -EIO;
	}

#ifdef CONFIG_LSM6DS3TRC_TRIGGER
	ret = lsm6ds3trc_trigger_init(dev);
	if (ret < 0) {
		LOG_ERR("could not set up the INT1 trigger (%d)", ret);
		return ret;
	}
#endif

	return 0;
}

#ifdef CONFIG_LSM6DS3TRC_TRIGGER
#define LSM6DS3TRC_IRQ_GPIO(inst) .irq_gpio = GPIO_DT_SPEC_INST_GET(inst, irq_gpios),
#else
#define LSM6DS3TRC_IRQ_GPIO(inst)
#endif

#define LSM6DS3TRC_DEFINE(inst)                                                                    \
	static struct lsm6ds3trc_data lsm6ds3trc_data_##inst;                                      \
	static const struct lsm6ds3trc_config lsm6ds3trc_config_##inst = {                         \
		.i2c = I2C_DT_SPEC_INST_GET(inst), LSM6DS3TRC_IRQ_GPIO(inst)};                     \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, lsm6ds3trc_init, NULL, &lsm6ds3trc_data_##inst,         \
				     &lsm6ds3trc_config_##inst, POST_KERNEL,                       \
				     CONFIG_SENSOR_INIT_PRIORITY, &lsm6ds3trc_api);

DT_INST_FOREACH_STATUS_OKAY(LSM6DS3TRC_DEFINE)
