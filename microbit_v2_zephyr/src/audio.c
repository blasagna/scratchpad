/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Audio capture and peak-frequency detection.
 *
 * The micro:bit V2's microphone is an ANALOG MEMS part on AIN3, so this goes
 * through the SAADC, not the PDM peripheral. Three conditions have to hold for
 * the SAADC's hardware sample timer to engage (see start_read() in
 * drivers/adc/adc_nrfx_saadc.c): exactly one active channel, a NULL sequence
 * callback, and interval_us <= 128. Miss any of them and the driver quietly
 * falls back to software-timed sampling, which cannot hold this rate.
 *
 * One second of audio is 61 KB, so capture is block-wise: one 2048-sample read,
 * one FFT, accumulate, discard, repeat. Averaging the 15 spectra (Welch's
 * method) is what pulls a tone out of the noise floor.
 */

#include "audio.h"
#include "display.h"

#include <arm_math.h>

#include <float.h>
#include <math.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(audio, LOG_LEVEL_INF);

#define FFT_SIZE 2048
#define FFT_BINS (FFT_SIZE / 2)

/* interval_us is an integer, so 32 us is the closest step to 32 kHz -- and it
 * divides exactly, which 31 us would not.
 */
#define SAMPLE_INTERVAL_US 32
#define SAMPLE_RATE_HZ     (1000000 / SAMPLE_INTERVAL_US) /* 31250 */

/* 15 * 2048 / 31250 = 0.98 s. */
#define BLOCK_COUNT 15

/* Time for the microphone bias to settle after power is applied. */
#define MIC_SETTLE_MS 10

/* Ignore DC and the bins below roughly 30 Hz. Bin width is 15.26 Hz. */
#define MIN_BIN 2

#define AUDIO_STACK_SIZE 2048
#define AUDIO_PRIORITY   6

static const struct adc_dt_spec mic_adc = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));
static const struct gpio_dt_spec mic_power =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), mic_power_gpios);

static int16_t raw[FFT_SIZE];
static float32_t fft_in[FFT_SIZE];
static float32_t fft_out[FFT_SIZE];
static float32_t window[FFT_SIZE];
static float32_t mag_avg[FFT_BINS];

static arm_rfft_fast_instance_f32 fft;

static K_SEM_DEFINE(capture_req, 0, 1);
static atomic_t capture_active;

void audio_request_capture(void)
{
	if (atomic_get(&capture_active)) {
		return;
	}

	k_sem_give(&capture_req);
}

bool audio_capture_active(void)
{
	return atomic_get(&capture_active) != 0;
}

/* One 2048-sample block into raw[]. */
static int capture_block(void)
{
	static const struct adc_sequence_options opts = {
		.interval_us = SAMPLE_INTERVAL_US,
		.extra_samplings = FFT_SIZE - 1,
		/* Must stay NULL: a callback forces software-timed sampling. */
		.callback = NULL,
	};
	struct adc_sequence seq = {
		.options = &opts,
		.buffer = raw,
		.buffer_size = sizeof(raw),
	};
	int err;

	err = adc_sequence_init_dt(&mic_adc, &seq);
	if (err) {
		return err;
	}

	return adc_read_dt(&mic_adc, &seq);
}

/* Convert, remove DC, window, transform, and fold the magnitude spectrum into
 * the running average.
 */
static void accumulate_block(void)
{
	float32_t mean;

	for (size_t i = 0; i < FFT_SIZE; i++) {
		fft_in[i] = (float32_t)raw[i];
	}

	arm_mean_f32(fft_in, FFT_SIZE, &mean);
	arm_offset_f32(fft_in, -mean, fft_in, FFT_SIZE);
	arm_mult_f32(fft_in, window, fft_in, FFT_SIZE);

	/* Destroys fft_in, which is why the conversion above rebuilds it. */
	arm_rfft_fast_f32(&fft, fft_in, fft_out, 0);

	/* arm_rfft_fast_f32 packs DC in [0] and Nyquist in [1], then interleaved
	 * complex pairs. So bin k, for k >= 1, starts at fft_out[2k].
	 */
	mag_avg[0] += fabsf(fft_out[0]);
	for (size_t k = 1; k < FFT_BINS; k++) {
		float32_t re = fft_out[2 * k];
		float32_t im = fft_out[2 * k + 1];

		mag_avg[k] += sqrtf(re * re + im * im);
	}
}

/* Peak bin, refined by fitting a parabola through the log-magnitudes of the
 * peak and its two neighbours. Raw bin width is 15.26 Hz; this gets well inside
 * that. Returns Hz, or 0 if the spectrum has no usable peak.
 */
static float32_t peak_frequency(void)
{
	float32_t best = 0.0f;
	uint32_t peak = 0;
	float32_t a, b, c, denom, delta;

	for (uint32_t k = MIN_BIN; k < FFT_BINS - 1; k++) {
		if (mag_avg[k] > best) {
			best = mag_avg[k];
			peak = k;
		}
	}

	if (peak == 0 || best <= 0.0f) {
		return 0.0f;
	}

	a = logf(mag_avg[peak - 1] + FLT_MIN);
	b = logf(mag_avg[peak] + FLT_MIN);
	c = logf(mag_avg[peak + 1] + FLT_MIN);

	denom = a - 2.0f * b + c;
	delta = (denom != 0.0f) ? (0.5f * (a - c) / denom) : 0.0f;

	/* A parabola fitted to a genuine peak puts the vertex inside the bin. */
	if (delta < -0.5f || delta > 0.5f) {
		delta = 0.0f;
	}

	return ((float32_t)peak + delta) * (float32_t)SAMPLE_RATE_HZ / (float32_t)FFT_SIZE;
}

static void run_capture(void)
{
	float32_t hz;
	int err;

	memset(mag_avg, 0, sizeof(mag_avg));

	err = gpio_pin_set_dt(&mic_power, 1);
	if (err) {
		LOG_ERR("cannot power the microphone (%d)", err);
		return;
	}
	k_sleep(K_MSEC(MIC_SETTLE_MS));

	for (int i = 0; i < BLOCK_COUNT; i++) {
		err = capture_block();
		if (err) {
			LOG_ERR("block %d failed (%d)", i, err);
			(void)gpio_pin_set_dt(&mic_power, 0);
			return;
		}
		accumulate_block();
	}

	(void)gpio_pin_set_dt(&mic_power, 0);

	hz = peak_frequency();
	if (hz <= 0.0f) {
		LOG_WRN("no peak found");
		display_text("?");
		return;
	}

	LOG_INF("peak frequency %u Hz", (uint32_t)(hz + 0.5f));
	display_frequency((uint32_t)(hz + 0.5f));
}

static void audio_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		k_sem_take(&capture_req, K_FOREVER);

		atomic_set(&capture_active, 1);
		run_capture();
		atomic_set(&capture_active, 0);
	}
}

K_THREAD_DEFINE(audio_tid, AUDIO_STACK_SIZE, audio_thread, NULL, NULL, NULL, AUDIO_PRIORITY, 0,
		K_TICKS_FOREVER);

int audio_start(void)
{
	int err;

	if (!adc_is_ready_dt(&mic_adc)) {
		LOG_ERR("ADC not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&mic_power)) {
		LOG_ERR("microphone power GPIO not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&mic_power, GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("cannot configure microphone power (%d)", err);
		return err;
	}

	err = adc_channel_setup_dt(&mic_adc);
	if (err) {
		LOG_ERR("cannot set up ADC channel (%d)", err);
		return err;
	}

	/* Use the size-specific initialiser: arm_rfft_fast_init_f32() dispatches
	 * on length and drags every twiddle table from 32 to 4096 into flash.
	 */
	if (arm_rfft_fast_init_2048_f32(&fft) != ARM_MATH_SUCCESS) {
		LOG_ERR("FFT init failed");
		return -EINVAL;
	}

	arm_hanning_f32(window, FFT_SIZE);

	k_thread_start(audio_tid);
	LOG_INF("audio ready: %d Hz, %d x %d-point FFT", SAMPLE_RATE_HZ, BLOCK_COUNT, FFT_SIZE);
	return 0;
}
