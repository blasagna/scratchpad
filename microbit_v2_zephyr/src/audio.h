/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_AUDIO_H_
#define APP_AUDIO_H_

#include <stdbool.h>

/** Check the ADC channel and start the capture thread. */
int audio_start(void);

/** Ask for a capture. Ignored if one is already running. */
void audio_request_capture(void);

/** True from the moment a capture is accepted until its result is displayed. */
bool audio_capture_active(void);

#endif /* APP_AUDIO_H_ */
