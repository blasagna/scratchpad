/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_BLE_H_
#define APP_BLE_H_

#include <stdint.h>

/** Bring up the stack, register the GATT service, and start advertising. */
int ble_start(void);

/** Queue a temperature reading, in hundredths of a degree Celsius. */
void ble_notify_temp(int16_t centi_c);

/** Queue a button event. @p button is 0 for A and 1 for B; @p pressed is 1/0. */
void ble_notify_button(uint8_t button, uint8_t pressed, uint32_t t_ms);

#endif /* APP_BLE_H_ */
