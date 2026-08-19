#![no_std]
#![no_main]

use defmt::info;
use embassy_executor::Spawner;
use embassy_rp::adc::{Adc, Channel, Config as AdcConfig, InterruptHandler};
use embassy_rp::bind_interrupts;
use embassy_rp::gpio::{Level, Output};
use embassy_time::Timer;
use {defmt_rtt as _, panic_probe as _};

bind_interrupts!(struct Irqs {
    ADC_IRQ_FIFO => InterruptHandler;
});

/// Size of the temperature-history ring buffer. Deliberately small so the
/// bug below fires within a few seconds instead of requiring a long wait.
const HISTORY_LEN: usize = 8;

#[embassy_executor::main]
async fn main(_spawner: Spawner) {
    let p = embassy_rp::init(Default::default());

    let mut led = Output::new(p.PIN_25, Level::Low);
    let mut adc = Adc::new(p.ADC, Irqs, AdcConfig::default());
    let mut temp_channel = Channel::new_temp_sensor(p.ADC_TEMP_SENSOR);

    let mut history = [0i32; HISTORY_LEN];
    let mut index: usize = 0;

    info!("pico-probe-lab starting; RTT link is live");

    loop {
        led.toggle();

        let raw = adc.read(&mut temp_channel).await.unwrap();
        let millicelsius = raw_to_millicelsius(raw);
        info!("sample {}: raw={} temp={} m°C", index, raw, millicelsius);

        // --- Deliberate bug --------------------------------------------
        // `index` should wrap with `index = (index + 1) % HISTORY_LEN`,
        // but it just keeps incrementing. After HISTORY_LEN samples this
        // indexing panics with an out-of-bounds access.
        //
        // Debugging ideas:
        //   - Set a breakpoint on the line below and step through the
        //     last couple of iterations to watch `index` approach the
        //     limit.
        //   - Set a watchpoint on `index` instead of a breakpoint and let
        //     the program run — it'll stop the moment the value changes.
        //   - Let it panic once, then inspect the backtrace `probe-rs`
        //     prints, and use `probe-rs attach` + gdb to look at the
        //     `history` array and `index` value at the point of the crash.
        history[index] = millicelsius;
        index += 1;
        // -----------------------------------------------------------------

        Timer::after_millis(500).await;
    }
}

/// Converts a 12-bit reading from the RP2040's onboard temperature sensor
/// into millidegrees Celsius, using the formula from the RP2040 datasheet
/// (section 4.9.5).
fn raw_to_millicelsius(raw: u16) -> i32 {
    let voltage_mv = (raw as i32) * 3300 / 4096;
    27_000 - (voltage_mv - 706) * 1000 / 1721
}
