/*
 * Application shell commands, on the console CDC ACM instance.
 * Copyright (c) 2026 Bob DiMaiolo. SPDX-License-Identifier: Apache-2.0
 *
 * These answer the questions the built-ins cannot: which IMU this board
 * actually carries, whether batches are being dropped and by which transport,
 * and what the SHT30's blocking read really costs.
 */

#ifdef CONFIG_SHELL

#include "battery.hpp"
#include "codec.hpp"
#include "env.hpp"
#include "imu.hpp"
#include "streams.hpp"
#include "usb.hpp"

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

#include <hal/nrf_power.h>

namespace
{

int cmd_stats(const shell *sh, size_t, char **)
{
	const streams::Counters s = streams::counters();
	const usb::Counters u = usb::counters();

	shell_print(sh, "batches   emitted %u", s.emitted);
	shell_print(sh, "dropped   ble %u  usb %u  oversize %u", s.dropped_ble, s.dropped_usb,
		    s.dropped_oversize);
	shell_print(sh, "usb       sent %u  dropped %u  rx %u  rx errors %u", u.frames_sent,
		    u.frames_dropped, u.rx_frames, u.rx_errors);
	shell_print(sh, "imu batch %u samples per notification", streams::imu_batch_samples());

	for (uint8_t id = codec::kStreamMin; id <= codec::kStreamMax; id++) {
		shell_print(sh, "stream %u  %s", id, streams::enabled(id) ? "on" : "off");
	}

	return 0;
}

int cmd_imu(const shell *sh, size_t, char **)
{
	const imu::Stats s = imu::stats();

	shell_print(sh, "WHO_AM_I  0x%02x (%s)", s.who_am_i,
		    s.who_am_i == 0x6a ? "LSM6DS3TR-C"
				       : (s.who_am_i == 0x69 ? "LSM6DS33" : "unknown"));
	shell_print(sh, "samples   %u in %u batches", s.samples, s.batches);
	shell_print(sh, "fifo      %u overruns, %u stall flushes", s.overruns, s.stall_flushes);

	return 0;
}

int cmd_battery(const shell *sh, size_t, char **)
{
	const battery::Reading r = battery::last();

	shell_print(sh, "%u mV  %u %%  flags 0x%02x%s", r.millivolts, r.percent, r.flags,
		    (r.flags & codec::kBatteryFlagUsb) != 0 ? " (usb)" : "");

	return 0;
}

int cmd_env(const shell *sh, size_t, char **)
{
	/* The number the design's 1 Hz budget rests on, and which no
	 * measurement has yet settled.
	 */
	shell_print(sh, "last SHT30 fetch blocked for %u us, %u failed so far",
		    env::last_fetch_us(), env::fetch_failures());

	return 0;
}

int cmd_stream(const shell *sh, size_t argc, char **argv)
{
	if (argc != 3) {
		shell_error(sh, "usage: fs stream <id 1-5> <0|1>");
		return -EINVAL;
	}

	const uint8_t id = static_cast<uint8_t>(strtoul(argv[1], nullptr, 0));
	const bool enable = strtoul(argv[2], nullptr, 0) != 0;

	streams::set_enabled(id, enable);
	shell_print(sh, "stream %u %s", id, streams::enabled(id) ? "on" : "off");

	return 0;
}

/*
 * Reboot into the UF2 bootloader, so a reflash needs no physical double-tap of
 * the reset button.
 *
 * The Adafruit nRF52 bootloader reads NRF_POWER->GPREGRET on startup and enters
 * UF2 mass-storage mode when it holds 0x57 (its DFU_MAGIC_UF2_RESET; 0x4e is
 * serial-only DFU and 0xa8 is BLE OTA). GPREGRET is a retained register: a soft
 * reset preserves it, and only a power-on reset clears it.
 *
 * This exists because the alternative is worse than it sounds. CircuitPython's
 * 1200-baud touch does the same thing from the host side, but Zephyr's CDC ACM
 * implements no such hook, so without this command every reflash costs a hand
 * on the board -- and the board is normally somewhere a hand is not.
 */
int cmd_bootloader(const shell *sh, size_t, char **)
{
	constexpr uint32_t kDfuMagicUf2Reset = 0x57;

	shell_print(sh, "rebooting into the UF2 bootloader; FTHRSNSBOOT should appear");
	/* The message has to reach the host before the reset does. */
	k_msleep(100);

	/* Register 0 of the two GPREGRETs: it is the one the Adafruit
	 * bootloader reads. */
	nrf_power_gpregret_set(NRF_POWER, 0, kDfuMagicUf2Reset);
	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	fs_cmds, SHELL_CMD(stats, NULL, "Batch and transport counters", cmd_stats),
	SHELL_CMD(imu, NULL, "Which IMU this board carries, and its FIFO health", cmd_imu),
	SHELL_CMD(battery, NULL, "The last battery reading", cmd_battery),
	SHELL_CMD(env, NULL, "What the last SHT30 fetch cost", cmd_env),
	SHELL_CMD_ARG(stream, NULL, "Enable or disable a stream", cmd_stream, 3, 0),
	SHELL_CMD(bootloader, NULL, "Reboot into the UF2 bootloader", cmd_bootloader),
	SHELL_SUBCMD_SET_END);

} /* namespace */

SHELL_CMD_REGISTER(fs, &fs_cmds, "feather_sense_zephyr", NULL);

#endif /* CONFIG_SHELL */
