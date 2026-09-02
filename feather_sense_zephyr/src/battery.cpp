/*
 * Copyright (c) 2026 Bob DiMaiolo
 * SPDX-License-Identifier: Apache-2.0
 */

#include "battery.hpp"

#include "battery_level.hpp"
#include "codec.hpp"
#include "led.hpp"
#include "streams.hpp"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/adc/voltage_divider.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hal/nrf_power.h>

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

namespace battery
{
namespace
{

/*
 * The `vbatt` node is already in the board DTS -- a voltage-divider on ADC
 * channel 5 with output-ohms 100k and full-ohms 200k. Going through
 * voltage_divider_scale_dt() rather than multiplying by a hand-written two is
 * what keeps the ratio a devicetree fact.
 */
/* DT_PATH, not DT_NODELABEL: the board dtsi declares this node as a bare
 * `vbatt { ... }` under the root with no label to reference it by. */
const voltage_divider_dt_spec vbatt = VOLTAGE_DIVIDER_DT_SPEC_GET(DT_PATH(vbatt));

constexpr size_t kStackSize = 1024;
K_THREAD_STACK_DEFINE(stack, kStackSize);
k_thread thread;

/* The lowest priority of the sampling threads: it is the least urgent work in
 * the application and the only one that also drives a bit-banged LED.
 */
constexpr int kPriority = 9;
constexpr int kPeriodMs = 1000;

k_timer timer;
Reading current;
Band band = Band::kUnknown;
bool have_reading;

int read_millivolts(uint16_t &millivolts)
{
	int16_t raw = 0;
	adc_sequence sequence = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};

	/* Returns -ENOTSUP when the devicetree has no channel node for this
	 * spec, which is what happens without app.overlay's `channel@5` -- and
	 * is the whole reason that node exists. It leaves `buffer` alone.
	 */
	int ret = adc_sequence_init_dt(&vbatt.port, &sequence);
	if (ret < 0) {
		return ret;
	}

	ret = adc_read_dt(&vbatt.port, &sequence);
	if (ret < 0) {
		return ret;
	}

	int32_t value = raw;

	ret = adc_raw_to_millivolts_dt(&vbatt.port, &value);
	if (ret < 0) {
		return ret;
	}

	ret = voltage_divider_scale_dt(&vbatt, &value);
	if (ret < 0) {
		return ret;
	}

	millivolts = static_cast<uint16_t>(value < 0 ? 0 : value);

	return 0;
}

void entry(void *, void *, void *)
{
	k_timer_start(&timer, K_MSEC(kPeriodMs), K_MSEC(kPeriodMs));

	while (true) {
		k_timer_status_sync(&timer);

		uint16_t millivolts = 0;

		if (read_millivolts(millivolts) < 0) {
			LOG_WRN("battery read failed");
			continue;
		}

		const Reading reading = {
			.millivolts = millivolts,
			.percent = percent_from_millivolts(millivolts),
			/* USB presence, read straight off the SoC's USB
			 * regulator rather than inferred from whether a host
			 * has opened a port. No charging-state correction is
			 * applied to the reading: the divider reads the pack's
			 * terminal, and charging elevates that somewhat, but no
			 * offset has been measured on this board and an
			 * uncalibrated one would be guesswork dressed as
			 * precision. See README.md.
			 */
			.flags = static_cast<uint8_t>(nrf_power_usbregstatus_vbusdet_get(NRF_POWER)
							      ? codec::kBatteryFlagUsb
							      : 0),
		};

		const bool percent_moved = !have_reading || reading.percent != current.percent ||
					   reading.flags != current.flags;

		current = reading;
		have_reading = true;

		/* The band moves in both directions and nothing latches it --
		 * band_for() applies hysteresis against whatever is painted.
		 */
		const Band next = band_for(reading.percent, band);
		if (next != band) {
			band = next;
			led::show(band);
		}

		/* Requirement 1.7: transmitted only on a change of at least
		 * 1 %. percent is an integer, so any change is at least that.
		 */
		if (percent_moved) {
			streams::emit(codec::kStreamBattery, k_uptime_get_32(), 0, 1, &reading,
				      sizeof(reading));
		}
	}
}

} /* namespace */

Reading last()
{
	return current;
}

int start()
{
	if (!adc_is_ready_dt(&vbatt.port)) {
		LOG_ERR("the battery ADC channel is not ready");
		return -ENODEV;
	}

	int ret = adc_channel_setup_dt(&vbatt.port);
	if (ret < 0) {
		LOG_ERR("could not set up the battery ADC channel (%d)", ret);
		return ret;
	}

	k_timer_init(&timer, nullptr, nullptr);

	k_thread_create(&thread, stack, kStackSize, entry, nullptr, nullptr, nullptr, kPriority, 0,
			K_NO_WAIT);
	k_thread_name_set(&thread, "battery");

	return 0;
}

} /* namespace battery */
