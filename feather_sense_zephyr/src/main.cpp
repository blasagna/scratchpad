/*
 * Device readiness checks and thread startup, in dependency order.
 * Copyright (c) 2026 Bob DiMaiolo. SPDX-License-Identifier: Apache-2.0
 */

#include "battery.hpp"
#include "ble.hpp"
#include "buttons.hpp"
#include "env.hpp"
#include "imu.hpp"
#include "led.hpp"
#include "magn.hpp"
#include "streams.hpp"
#include "usb.hpp"

#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

namespace
{

/* A failed subsystem is logged and skipped rather than fatal. A board with a
 * dead magnetometer should still stream its accelerometer, and the shell should
 * still come up so the failure can be looked at.
 */
void bring_up(const char *what, int (*start)())
{
	const int ret = start();

	if (ret != 0) {
		LOG_ERR("%s did not start (%d)", what, ret);
	}
}

} /* namespace */

int main()
{
	LOG_INF("feather_sense_zephyr %s+%s", APP_VERSION_STRING, APP_GIT_DESCRIBE);

	/* USB first: the console, the shell and the log all ride a CDC ACM
	 * endpoint that nothing brings up until this runs, so everything logged
	 * before it is only visible in the deferred log's backlog.
	 */
	bring_up("usb", usb::start);

	/* Then the transports' consumers, so no producer can emit into a queue
	 * that nothing is draining.
	 */
	streams::start();
	bring_up("ble", ble::start);

	/* The LED before the battery thread, which paints it on its first
	 * reading.
	 */
	bring_up("led", led::start);

	bring_up("imu", imu::start);
	bring_up("magn", magn::start);
	bring_up("env", env::start);
	bring_up("battery", battery::start);
	bring_up("buttons", buttons::start);

	return 0;
}
