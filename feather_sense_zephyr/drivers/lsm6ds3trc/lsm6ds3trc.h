/*
 * LSM6DS3TR-C / LSM6DS33 6-axis IMU -- application-facing API.
 *
 * Copyright (c) 2026 Bob DiMaiolo
 * SPDX-License-Identifier: Apache-2.0
 *
 * The driver also implements Zephyr's ordinary sensor API (SENSOR_CHAN_ACCEL_*
 * and SENSOR_CHAN_GYRO_*, in SI units), which is what makes `sensor get imu0`
 * work at the shell. That path is for diagnosis. The streaming path is the FIFO
 * one below: it hands back the chip's own int16 registers with no arithmetic
 * anywhere, which is what keeps the 208 Hz path a memcpy.
 */

#ifndef FEATHER_SENSE_LSM6DS3TRC_H_
#define FEATHER_SENSE_LSM6DS3TRC_H_

#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One FIFO record: the six int16 registers exactly as the chip emits them.
 *
 * Gyro precedes accel because that is the order of the FIFO pattern and of the
 * OUTX_L_G-onward register block. It is also the order that goes on the wire,
 * so the encoder never reorders anything. The two halves come out of one burst
 * and therefore share one sample instant exactly -- read separately they would
 * be about a millisecond apart and independently timestamped, which is a lie
 * about a single physical sample instant that any downstream fusion inherits.
 */
struct lsm6ds3trc_sample {
	int16_t gx, gy, gz;
	int16_t ax, ay, az;
};

/*
 * Drain up to `max_samples` complete six-axis records out of the chip's FIFO.
 *
 * Returns the number of records written to `out` (0 if the FIFO holds less than
 * one complete record), or a negative errno. Never blocks on the chip: it reads
 * what is there and returns.
 */
int lsm6ds3trc_fifo_read(const struct device *dev, struct lsm6ds3trc_sample *out,
			 uint8_t max_samples);

/*
 * How many complete six-axis records the FIFO currently holds. Negative errno
 * on a bus failure. The application uses this for its stall clamp: a FIFO
 * holding far more than one watermark is a backlog, not a buffer.
 */
int lsm6ds3trc_fifo_level(const struct device *dev);

/*
 * Discard the FIFO's contents and restart it. Used after a stall, so a backlog
 * is dropped rather than delivered as a burst of stale samples carrying
 * plausible-looking back-dated timestamps.
 */
int lsm6ds3trc_fifo_flush(const struct device *dev);

/*
 * The spacing between consecutive FIFO records, from the chip's own ODR. This
 * is what goes in a batch header's `period_us`, so the host back-dates the
 * batch from the device's clock rather than guessing from a nominal rate.
 */
uint32_t lsm6ds3trc_sample_period_us(const struct device *dev);

/*
 * The WHO_AM_I byte this chip answered at init: 0x6a for an LSM6DS3TR-C, 0x69
 * for an LSM6DS33. Reported at the shell, since which part a given board
 * carries is not knowable from the board files.
 */
uint8_t lsm6ds3trc_who_am_i(const struct device *dev);

/*
 * Whether the FIFO has overrun since the last call -- i.e. the chip dropped
 * samples because nothing drained it in time. Self-clearing. A device-side drop
 * of this kind also shows up on the host as a jump in the batch `seq`, which is
 * the point of that field; this is the direct measurement of the same thing.
 */
bool lsm6ds3trc_fifo_overrun(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* FEATHER_SENSE_LSM6DS3TRC_H_ */
