#ifndef APP_DISPLAY_H_
#define APP_DISPLAY_H_

#include <stdint.h>

int display_init(void);

/** Scroll a frequency in Hz across the 5x5 matrix. */
void display_frequency(uint32_t hz);

/** Scroll a short string. Calls are asynchronous and cancel any previous one. */
void display_text(const char *text);

#endif /* APP_DISPLAY_H_ */
