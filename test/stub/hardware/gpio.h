// Host-side stub of the Pico SDK <hardware/gpio.h>, sized for RP2350B (48 pins)
// so the GPIO-base window checks are exercisable.
#ifndef _STUB_HARDWARE_GPIO_H
#define _STUB_HARDWARE_GPIO_H

#include <cstdint>

typedef unsigned int uint;

#define NUM_BANK0_GPIOS 48
#define GPIO_IN        0
#define GPIO_OUT       1
#define GPIO_FUNC_SIO  5
#define GPIO_FUNC_PIO0 6

typedef uint gpio_function_t;

void gpio_init(uint gpio);
void gpio_set_dir(uint gpio, bool out);
void gpio_put(uint gpio, bool value);
bool gpio_get(uint gpio);
void gpio_pull_up(uint gpio);
void gpio_disable_pulls(uint gpio);
void gpio_set_function(uint gpio, gpio_function_t fn);

#endif
