#ifndef APP_TEMP_CONVERT_HPP_
#define APP_TEMP_CONVERT_HPP_

#include <cstdint>

/*
 * Converts a 12-bit reading from the RP2040's onboard temperature sensor into
 * millidegrees Celsius, using the datasheet formula (section 4.9.5):
 *
 *   T = 27 - (V - 0.706) / 0.001721
 *
 * Kept free of Zephyr headers on purpose, so the exact same translation unit
 * compiles both for the RP2040 firmware and for the host-side native_sim test
 * under tests/ -- the C++ analog of the Rust sibling's dependency-free
 * `rp2040_temp` crate.
 */
int32_t raw_to_millicelsius(uint16_t raw);

#endif /* APP_TEMP_CONVERT_HPP_ */
