/*
 * A minimal RP2040 (Raspberry Pi Pico W) firmware whose only job is to be a
 * target for a debugger -- breakpoints, watchpoints, and a fatal-error
 * backtrace -- driven over SWD by a Raspberry Pi Debug Probe and OpenOCD.
 *
 * It is the C++/Zephyr counterpart of ../rpi_pico_rust_debug (Rust/embassy +
 * probe-rs). See README.md for the wiring and the debugging walkthrough.
 *
 * The heart of it is a deliberate bug: a ring-buffer index that never wraps.
 * See the loop in main() and README.md's "What to try".
 */

#include "temp_convert.hpp"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>
#endif

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* File-scope `static` rather than an anonymous namespace, deliberately: the
 * whole point of this firmware is the debugger, so `history`, `sample_index`,
 * and `sample_count` are kept at plain file scope where GDB resolves them by
 * their source names (`print sample_index`, `watch sample_count`). An anonymous
 * namespace tucks them under a `(anonymous namespace)` scope instead, which
 * some GDB versions then want qualified. (C++ still mangles the *linkage*
 * symbol either way -- `_ZL7history` -- but GDB reads the DWARF name, not that.)
 */

/* Size of the temperature-history ring buffer. Deliberately small so the bug
 * below fires within a few seconds instead of requiring a long wait.
 */
constexpr size_t HISTORY_LEN = 8;

/* One sample every 500 ms -- a slow, human-readable heartbeat. */
constexpr int SAMPLE_PERIOD_MS = 500;

static int32_t history[HISTORY_LEN];
static size_t sample_index;

/* A file-scope mirror of `sample_index`, updated after every increment. It is
 * a clean, named target for a hardware watchpoint (`watch sample_count` in
 * GDB): it lives at a fixed address in RAM and appears in the debug info by
 * name. `volatile` stops -Og from optimising the store away.
 *
 * Unlike the Rust sibling -- where the equivalent `index` is a local of an
 * async state machine that the debugger can't resolve, so the mirror is the
 * only way to watch it -- here the loop's `sample_index` is itself a plain
 * static that GDB reads directly. So this is a convenience that keeps the
 * watchpoint exercise identical across the two ports, not a necessity.
 */
static volatile uint32_t sample_count;

/* Optional heartbeat LED. On a Pico W the onboard LED hangs off the CYW43
 * wireless chip rather than a plain GPIO, so this is an EXTERNAL LED on a free
 * pin, declared under `zephyr,user` in app.overlay. If no overlay provides one
 * the spec is zero-initialised and the LED is simply skipped.
 */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(DT_PATH(zephyr_user), led_gpios, {0});

/* A synthetic stand-in for an ADC reading. The Rust sibling samples the RP2040
 * onboard temperature sensor here; this port generates the raw 12-bit value in
 * software so the exercise is deterministic and needs no working ADC -- the
 * point is the debugger and the bug, not the sensor. It sweeps a sawtooth
 * across a plausible band around the datasheet's 27 C reference (raw ~= 876).
 */
static uint16_t next_raw()
{
	static uint16_t tick;

	tick++;
	return static_cast<uint16_t>(820 + (tick * 3) % 120);
}

#ifdef CONFIG_SHELL
/* `temp` shell command: dump the ring buffer and the two indices over the
 * console, an on-device introspection path beside the external debugger.
 */
static int cmd_temp(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	/* Snapshot the loop's shared state under a scheduler lock so the dump is
	 * internally consistent even if main() is mid-iteration -- the shell I/O
	 * then runs outside the lock, where it's free to block.
	 */
	int32_t snapshot[HISTORY_LEN];
	size_t index;
	uint32_t count;

	k_sched_lock();
	index = sample_index;
	count = sample_count;
	for (size_t i = 0; i < HISTORY_LEN; i++) {
		snapshot[i] = history[i];
	}
	k_sched_unlock();

	shell_print(sh, "sample_index=%zu sample_count=%u (HISTORY_LEN=%zu)", index, count,
		    HISTORY_LEN);
	for (size_t i = 0; i < HISTORY_LEN; i++) {
		shell_print(sh, "  history[%zu] = %d m°C", i, snapshot[i]);
	}

	return 0;
}

SHELL_CMD_REGISTER(temp, NULL, "Print the temperature-history ring buffer", cmd_temp);
#endif

int main(void)
{
	LOG_INF("rpi_pico_zephyr_debug starting");

	bool led_ready = false;

	if (led.port != NULL) {
		if (gpio_is_ready_dt(&led)) {
			gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
			led_ready = true;
		} else {
			LOG_WRN("heartbeat LED not ready; continuing without it");
		}
	}

	for (;;) {
		uint16_t raw = next_raw();
		int32_t millicelsius = raw_to_millicelsius(raw);

		LOG_INF("sample %zu: raw=%u temp=%d m°C", sample_index, raw, millicelsius);

		/* --- Deliberate bug ---------------------------------------------
		 * `sample_index` should wrap with `(sample_index + 1) %
		 * HISTORY_LEN`, but it just keeps incrementing. After
		 * HISTORY_LEN samples it walks off the end of `history`.
		 *
		 * In C++ that write is silent undefined behaviour -- unlike
		 * Rust, where indexing is bounds-checked and panics on its own.
		 * The __ASSERT below stands in for that check: with
		 * CONFIG_ASSERT=y it turns the out-of-bounds access into a
		 * deterministic fatal error -- with the failing expression,
		 * file:line, and (via CONFIG_EXCEPTION_STACK_TRACE) a backtrace
		 * -- which is what the walkthrough in README.md drives. Remove
		 * it and the same bug becomes memory corruption you'd have to
		 * hunt by its symptoms; that contrast is itself the lesson.
		 *
		 * Debugging ideas (see README.md):
		 *   - Break on the `history[...]` write and step the last few
		 *     iterations, watching `sample_index` approach the limit
		 *     (`print sample_index` works here -- it's a plain static).
		 *   - Set a watchpoint on `sample_count` and let it run: it
		 *     stops the instant the value changes.
		 *   - Just let it run into the assert and read the backtrace.
		 */
		__ASSERT(sample_index < HISTORY_LEN, "history index %zu out of bounds",
			 sample_index);
		history[sample_index] = millicelsius;
		sample_index = sample_index + 1;
		// The one-line fix (README.md, "Fixing the bug"):
		// sample_index = (sample_index + 1) % HISTORY_LEN;
		sample_count = sample_index;
		/* ---------------------------------------------------------------- */

		if (led_ready) {
			gpio_pin_toggle_dt(&led);
		}
		k_msleep(SAMPLE_PERIOD_MS);
	}

	return 0;
}
