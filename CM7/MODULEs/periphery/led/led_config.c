/*
 * led_config.c
 *
 *  Created on: 16 мая 2026 г.
 *      Author: grasser
 */


#include "led_config.h"
#include "led.h"
#include "main.h"

led_ctx_t led_ctx[NUM_LEDS] = {
		{.led_gpio = LED1_GPIO_PORT, .led_pin = LED1_PIN},
		{.led_gpio = LED2_GPIO_PORT, .led_pin = LED2_PIN},
		{.led_gpio = LED3_GPIO_PORT, .led_pin = LED3_PIN}
};

/*---Adding an LED pin---*/
