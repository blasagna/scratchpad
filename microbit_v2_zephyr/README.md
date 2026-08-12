# bbc micro:bit v2 zephyr rtos application

Learn [Zephyr RTOS](https://docs.zephyrproject.org/latest/index.html) by building an application for the [micro:bit V2](https://docs.zephyrproject.org/latest/boards/bbc/microbit_v2/doc/index.html) board, which has several on board sensors, buttons, LEDs, and BLE communication capabilities.

This application sits in a repo outside of the standard zephyr workspace. See [zephyr application development](https://docs.zephyrproject.org/latest/develop/application/index.html#application), focusing on the "Zephyr freestanding application" pattern. The zephyr workspace is at ~/zephyrproject.

## requirements

1. Sample the accelerometer at 100 Hz continuously
1. Sample the MCU temperature continuously at 1 Hz
1. Capture press and release events for both the A and B buttons
1. Play different short audible buzzes when button A or button B is pressed
1. When button A is pressed, capture audio for 1 second. Calculate the peak frequency of the audio using an on-device FFT computation. Expect tones in the range of human hearing. Print the frequency to the LED matrix display.
1. Make the device a BLE peripheral.
  1. Expose a characteristic to stream the accelerometer data in fixed-point values over notifications for high throughput. Batch the samples to maximize throughput over each connection interval.
  1. Expose a characteristic to stream the temperature data in notifications.
  1. Expose a characteristic to stream the A and B button press and release events in notifications.
