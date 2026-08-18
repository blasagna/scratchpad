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
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_string_conv.h>
#endif

LOG_MODULE_REGISTER(audio, LOG_LEVEL_DBG);

#define FFT_SIZE 2048
#define FFT_BINS (FFT_SIZE / 2)

/* interval_us is an integer, and 32 us is the closest step to 32 kHz. It does
 * NOT give 1000000/32 = 31250 Hz, which is what this used to assume.
 *
 * Zephyr programs the SAADC's sample timer through interval_to_cc()
 * (drivers/adc/adc_nrfx_saadc.c:503), which returns interval_us * 16 - 1, so
 * 32 us asks for CC = 511 rather than 512. The nRF52833 then samples at
 * 16 MHz / CC. The difference is only 0.2 %, but it is a systematic bias on
 * every frequency this reports, so it is worth deriving rather than assuming --
 * measured against a tone on a known bin centre, see README.md.
 *
 * These three must move together: SAMPLE_INTERVAL_US is the only real knob.
 */
#define SAMPLE_INTERVAL_US 32
#define SAMPLE_RATE_CC     (SAMPLE_INTERVAL_US * 16 - 1) /* 511 */
#define SAMPLE_RATE_HZ     (16000000 / SAMPLE_RATE_CC)   /* 31311 */

/* 15 * 2048 / 31311 = 0.98 s. */
#define BLOCK_COUNT 15

/* Time for the microphone bias to settle after power is applied. */
#define MIC_SETTLE_MS 10

/* What a capture should take if the SAADC's hardware sample timer engaged. The
 * FFTs run between adc_read() calls, so the real figure sits a few per cent
 * above this; several times this means the driver fell back to software timing.
 */
#define CAPTURE_EXPECTED_MS ((BLOCK_COUNT * FFT_SIZE * 1000U) / SAMPLE_RATE_HZ + MIC_SETTLE_MS)

/* Ignore DC and the bins below roughly 30 Hz. Bin width is 15.29 Hz. */
#define MIN_BIN 2

#define AUDIO_STACK_SIZE 2048
#define AUDIO_PRIORITY   6

static const struct adc_dt_spec mic_adc = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));
static const struct gpio_dt_spec mic_power =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), mic_power_gpios);

/* The SAADC's SAMPLERATE.CC timer counts PCLK16M, which derives from HFCLK --
 * and HFCLK runs from the internal RC oscillator unless something asks for the
 * crystal. The BLE controller asks around each radio event
 * (z_nrf_clock_bt_ctlr_hf_request) and drops it again, so without this a capture
 * is stitched together from crystal-accurate and RC-accurate stretches and the
 * reported frequency lands about 0.5 % low.
 *
 * Held on by default, for the roughly one second a capture lasts. That is a real
 * cost -- the crystal draws more than the RC -- but captures are user-triggered
 * and infrequent, and BLE is starting the same crystal every few tens of
 * milliseconds anyway. `audio hfxo off` turns it back off, which is how the
 * measurement in README.md was made.
 *
 * Take it with the reference-counted request API and NOT with
 * clock_control_on()/clock_control_off(). The two are not interchangeable here:
 * clock_control_nrf_common.c tags the clock with the context that started it,
 * COMMON_CTX_API for clock_control_on() against COMMON_CTX_ONOFF for a request,
 * and set_starting_state() fails with -EPERM when a second context asks while
 * the first holds it. The nRF die-temperature driver takes HFCLK through the
 * request API for every conversion (temp_nrf5_sample_fetch, which triggers the
 * TEMP START task from the request's completion callback), so with
 * clock_control_on() the once-a-second fetch that landed inside a capture got
 * -EPERM, and that failed transition leaves the onoff manager latched in its
 * error state for good: every later request returns -EIO without ever notifying,
 * and the driver's k_sem_take(&data->device_sync_sem, K_FOREVER) never returns.
 * One capture was enough to hang the temperature thread until the next reset --
 * the die-temperature BLE stream simply stopped. The request API shares the
 * clock instead of seizing it, which is what both users need.
 */
static const struct device *const hfclk = DEVICE_DT_GET_ONE(nordic_nrf_clock_hfclk);
static atomic_t hfxo_hold = ATOMIC_INIT(1);

/* The crystal's own startup_time_us is 360, so this is only a deadlock guard. */
#define HFXO_START_TIMEOUT_MS 10

static int16_t raw[FFT_SIZE];
static float32_t fft_in[FFT_SIZE];
static float32_t fft_out[FFT_SIZE];
static float32_t window[FFT_SIZE];
static float32_t mag_avg[FFT_BINS];

static arm_rfft_fast_instance_f32 fft;

static K_SEM_DEFINE(capture_req, 0, 1);
static atomic_t capture_active;

/* Enough of the last capture to describe it on the shell. raw[] and mag_avg[]
 * survive between captures precisely so they can be inspected afterwards; these
 * say which capture they belong to and whether it ran at the right rate.
 */
static uint32_t capture_count;
static uint32_t capture_ms;

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

/* Centre frequency of bin k. Takes a float because the peak is interpolated to
 * a fractional bin; bin_hz(1.0f) is the bin width.
 */
static float32_t bin_hz(float32_t k)
{
	return k * (float32_t)SAMPLE_RATE_HZ / (float32_t)FFT_SIZE;
}

/* Peak bin, refined by fitting a parabola through the log-magnitudes of the
 * peak and its two neighbours. Raw bin width is 15.29 Hz; this gets well inside
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

	return bin_hz((float32_t)peak + delta);
}

static void run_capture(void)
{
	float32_t hz;
	uint32_t started;
	bool hold;
	int err;

	memset(mag_avg, 0, sizeof(mag_avg));

	/* Taken before the clock is read, so the crystal's ramp-up does not land
	 * in the elapsed figure the sample-rate check depends on.
	 */
	hold = atomic_get(&hfxo_hold) != 0;
	if (hold) {
		err = nrf_clock_control_request_sync(hfclk, NULL, K_MSEC(HFXO_START_TIMEOUT_MS));
		if (err) {
			LOG_WRN("cannot hold HFXO (%d)", err);
			hold = false;
		}
	}

	started = k_uptime_get_32();

	err = gpio_pin_set_dt(&mic_power, 1);
	if (err) {
		LOG_ERR("cannot power the microphone (%d)", err);
		goto release;
	}
	k_sleep(K_MSEC(MIC_SETTLE_MS));

	for (int i = 0; i < BLOCK_COUNT; i++) {
		err = capture_block();
		if (err) {
			LOG_ERR("block %d failed (%d)", i, err);
			(void)gpio_pin_set_dt(&mic_power, 0);
			goto release;
		}
		accumulate_block();
	}

	(void)gpio_pin_set_dt(&mic_power, 0);
	if (hold) {
		(void)nrf_clock_control_release(hfclk, NULL);
		hold = false;
	}

	/* Sanity check on the sample clock. The SAADC silently falls back to
	 * software-timed sampling if any precondition slips, so a capture that
	 * takes much longer than BLOCK_COUNT * FFT_SIZE / SAMPLE_RATE_HZ means the
	 * hardware timer did not engage and the reported frequency is wrong.
	 */
	capture_ms = k_uptime_get_32() - started;
	capture_count++;
	LOG_INF("capture #%u took %u ms (expected ~%u)", capture_count, capture_ms,
		CAPTURE_EXPECTED_MS);

	hz = peak_frequency();
	if (hz <= 0.0f) {
		LOG_WRN("no peak found");
		display_text("?");
		return;
	}

	LOG_INF("peak frequency %u Hz", (uint32_t)(hz + 0.5f));
	display_frequency((uint32_t)(hz + 0.5f));
	return;

release:
	if (hold) {
		(void)nrf_clock_control_release(hfclk, NULL);
	}
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

	if (!device_is_ready(hfclk)) {
		LOG_ERR("HFCLK not ready");
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

#ifdef CONFIG_SHELL

/* Shell diagnostics for the capture path.
 *
 * The pipeline is otherwise write-only: accumulate_block() builds a 1024-bin
 * averaged spectrum and peak_frequency() collapses it to the single number that
 * reaches the LED matrix. When that number is wrong, the spectrum and the last
 * raw block are what say why -- a harmonic picked over the fundamental, a
 * clipped or unpowered microphone, a sample clock that fell back to software
 * timing. Both buffers are static and outlive the capture, so these commands
 * only have to read them.
 *
 * Both refuse to read while the audio thread is mid-capture, which is the only
 * writer. The check is a plain load rather than a lock: these are human-driven
 * commands and the alternative -- holding a mutex across a one-second capture --
 * buys nothing here.
 */

#define SPECTRUM_ROWS_DEFAULT 8
#define SPECTRUM_ROWS_MAX     32

/* Common preamble: refuse unless there is a settled capture to describe. */
static int check_capture(const struct shell *sh)
{
	if (atomic_get(&capture_active)) {
		shell_warn(sh, "capture in progress, try again in a second");
		return -EBUSY;
	}

	if (capture_count == 0) {
		shell_warn(sh, "no capture yet -- press button A");
		return -ENODATA;
	}

	return 0;
}

/* Statistics over the last 2048-sample block, in ADC counts and in millivolts.
 *
 * The channel is single-ended with ADC_GAIN_1_4 against ADC_REF_VDD_1_4, so
 * full scale is VDD across the 12-bit range and a healthy analog microphone
 * idles near mid-scale. A mean near zero means mic-power-gpios never asserted;
 * samples resting on either rail mean the signal is clipping.
 */
static int cmd_audio_raw(const struct shell *sh, size_t argc, char **argv)
{
	const int32_t full_scale = BIT(mic_adc.resolution) - 1;
	int32_t min = INT32_MAX;
	int32_t max = INT32_MIN;
	int64_t sum = 0;
	uint32_t at_low = 0;
	uint32_t at_high = 0;
	float32_t mean, sq = 0.0f, rms;
	int32_t mean_mv, pp_mv;
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	err = check_capture(sh);
	if (err) {
		return err;
	}

	for (size_t i = 0; i < FFT_SIZE; i++) {
		int32_t v = raw[i];

		min = MIN(min, v);
		max = MAX(max, v);
		sum += v;
		at_low += (v <= 0);
		at_high += (v >= full_scale);
	}

	mean = (float32_t)sum / (float32_t)FFT_SIZE;

	/* AC RMS: the DC bias is most of the signal, and it is what
	 * accumulate_block() subtracts before windowing.
	 */
	for (size_t i = 0; i < FFT_SIZE; i++) {
		float32_t d = (float32_t)raw[i] - mean;

		sq += d * d;
	}
	rms = sqrtf(sq / (float32_t)FFT_SIZE);

	/* adc_raw_to_millivolts_dt() converts in place and applies the gain,
	 * reference, and resolution from the devicetree channel -- the values
	 * app.overlay compiled in, not whatever the channel is set to now. The
	 * `adc` shell command reconfigures this same channel, so after using it
	 * to retune the gain, the counts here stay true and the millivolts do
	 * not. See README.md.
	 */
	mean_mv = (int32_t)(mean + 0.5f);
	pp_mv = max - min;
	if (adc_raw_to_millivolts_dt(&mic_adc, &mean_mv) != 0 ||
	    adc_raw_to_millivolts_dt(&mic_adc, &pp_mv) != 0) {
		mean_mv = -1;
		pp_mv = -1;
	}

	shell_print(sh, "capture #%u, block %d of %d, %u ms elapsed (expected ~%u)", capture_count,
		    BLOCK_COUNT, BLOCK_COUNT, capture_ms, CAPTURE_EXPECTED_MS);
	shell_print(sh, "%d samples at %d Hz, full scale %d counts", FFT_SIZE, SAMPLE_RATE_HZ,
		    full_scale);
	shell_print(sh, "  min %6d   max %6d   peak-to-peak %6d (%d mV)", min, max, max - min,
		    pp_mv);
	shell_print(sh, "  mean %11.1f counts (%d mV)   ac rms %.1f counts", (double)mean, mean_mv,
		    (double)rms);
	shell_print(sh, "  at rails: %u low, %u high", at_low, at_high);

	return 0;
}

/* The loudest bins of the averaged spectrum, strongest first.
 *
 * Rows are selected by repeated scan rather than by sorting mag_avg[]: the
 * array is 1024 floats that the next capture wants intact, and at most 32 rows
 * a scan apiece is far cheaper than a copy. Ordering is on (magnitude, bin) so
 * that bins of exactly equal magnitude still emerge one per row instead of the
 * lowest one repeating.
 */
static int cmd_audio_spectrum(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t rows = SPECTRUM_ROWS_DEFAULT;
	float32_t prev_mag = FLT_MAX;
	uint32_t prev_bin = 0;
	float32_t top = 0.0f;
	float32_t floor_mag;
	int err;

	if (argc == 2) {
		/* shell_strtol() writes *err only when the conversion fails, so
		 * this has to start at 0 -- left uninitialised it rejects every
		 * valid argument on whatever the stack happened to hold.
		 */
		long n;

		err = 0;
		n = shell_strtol(argv[1], 10, &err);

		if (err != 0 || n < 1 || n > SPECTRUM_ROWS_MAX) {
			shell_error(sh, "rows must be 1..%d", SPECTRUM_ROWS_MAX);
			return -EINVAL;
		}
		rows = (uint32_t)n;
	}

	err = check_capture(sh);
	if (err) {
		return err;
	}

	/* Over the same bins peak_frequency() searches, so the ratio between a
	 * row and this is what says whether a peak stands out at all.
	 */
	arm_mean_f32(&mag_avg[MIN_BIN], FFT_BINS - MIN_BIN, &floor_mag);

	shell_print(sh, "capture #%u, %d blocks averaged, %u ms elapsed (expected ~%u)",
		    capture_count, BLOCK_COUNT, capture_ms, CAPTURE_EXPECTED_MS);
	shell_print(sh, "%d-point fft at %d Hz, bin width %.2f Hz, bins %d..%d searched", FFT_SIZE,
		    SAMPLE_RATE_HZ, (double)bin_hz(1.0f), MIN_BIN, FFT_BINS - 2);
	shell_print(sh, "interpolated peak %.2f Hz, mean magnitude %.1f", (double)peak_frequency(),
		    (double)floor_mag);
	shell_print(sh, " rank   bin        Hz     magnitude      dBc");

	for (uint32_t r = 0; r < rows; r++) {
		float32_t best = 0.0f;
		uint32_t best_bin = 0;
		bool found = false;

		for (uint32_t k = MIN_BIN; k < FFT_BINS; k++) {
			float32_t m = mag_avg[k];

			/* Strictly after the previous row in (magnitude, bin)
			 * order -- everything already printed, skipped.
			 */
			if (m > prev_mag || (m == prev_mag && k <= prev_bin)) {
				continue;
			}
			if (!found || m > best || (m == best && k < best_bin)) {
				best = m;
				best_bin = k;
				found = true;
			}
		}

		if (!found) {
			break;
		}
		if (r == 0) {
			top = best;
		}

		/* dBc, relative to the strongest bin, so row 1 is always 0.0.
		 * FLT_MIN keeps log10f off zero on a silent spectrum.
		 */
		shell_print(sh, " %4u  %4u  %8.2f  %12.1f  %7.1f", r + 1, best_bin,
			    (double)bin_hz((float32_t)best_bin), (double)best,
			    (double)(20.0f * log10f((best + FLT_MIN) / (top + FLT_MIN))));

		prev_mag = best;
		prev_bin = best_bin;
	}

	return 0;
}

/* Turn the crystal hold off or back on, or report it.
 *
 * The hold is on by default, so this is mostly a way to get the old behaviour
 * back: measuring the sample rate both ways on one board, in one room, with no
 * reflash in between to argue about.
 */
static int cmd_audio_hfxo(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 2) {
		if (strcmp(argv[1], "on") == 0) {
			atomic_set(&hfxo_hold, 1);
		} else if (strcmp(argv[1], "off") == 0) {
			atomic_set(&hfxo_hold, 0);
		} else {
			shell_error(sh, "expected on or off");
			return -EINVAL;
		}
	}

	/* The status is whatever is true at this instant, and with BLE
	 * advertising "running" is perfectly possible even with the hold off --
	 * the controller takes the crystal for each radio event.
	 */
	shell_print(sh, "hfxo hold %s, hfclk %s right now", atomic_get(&hfxo_hold) ? "on" : "off",
		    clock_control_get_status(hfclk, NULL) == CLOCK_CONTROL_STATUS_ON ? "running"
										     : "stopped");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_audio,
	SHELL_CMD_ARG(hfxo, NULL,
		      "Hold the HF crystal on across a capture (default on).\n"
		      "Usage: audio hfxo [on|off]   (no argument reports)",
		      cmd_audio_hfxo, 1, 1),
	SHELL_CMD_ARG(raw, NULL,
		      "Statistics over the last captured block.\n"
		      "Usage: audio raw",
		      cmd_audio_raw, 1, 0),
	SHELL_CMD_ARG(spectrum, NULL,
		      "Loudest bins of the last averaged spectrum.\n"
		      "Usage: audio spectrum [rows]   (default 8, max 32)",
		      cmd_audio_spectrum, 1, 1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(audio, &sub_audio, "Audio capture diagnostics", NULL);

#endif /* CONFIG_SHELL */
