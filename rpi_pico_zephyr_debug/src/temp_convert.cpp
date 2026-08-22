#include "temp_convert.hpp"

namespace
{

/* ADC reference voltage in microvolts (the RP2040 samples against 3.3 V). */
constexpr int64_t VREF_UV = 3'300'000;

/* Full scale of the RP2040's 12-bit SAR ADC. */
constexpr int64_t ADC_FULL_SCALE = 4096;

/* Sensor voltage at 27 C, in microvolts (RP2040 datasheet, section 4.9.5). */
constexpr int64_t V_AT_27C_UV = 706'000;

/* Sensor slope: microvolts per degree Celsius. The voltage falls as the die
 * warms; the datasheet gives 1.721 mV/C.
 */
constexpr int64_t UV_PER_DEGREE = 1721;

} // namespace

int32_t raw_to_millicelsius(uint16_t raw)
{
	/* Work in microvolts so the divide doesn't quantise to ~0.58 C steps,
	 * and in int64_t throughout -- both `raw * 3'300'000` and the scaled
	 * offset overflow int32_t.
	 */
	int64_t voltage_uv = static_cast<int64_t>(raw) * VREF_UV / ADC_FULL_SCALE;

	return static_cast<int32_t>(27'000 - (voltage_uv - V_AT_27C_UV) * 1000 / UV_PER_DEGREE);
}
