/*
 * led_config.h
 *
 *  Created on: 16 мая 2026 г.
 *      Author: grasser
 */

#ifndef PERIPHERY_LED_LED_CONFIG_H_
#define PERIPHERY_LED_LED_CONFIG_H_

#include "led.h"

/*----Add led in indexation before END_LED*/
typedef enum {
	GREEN_LED,
	YELLOW_LED,
	RED_LED,
	END_LED
} led_id_t;

#define NUM_LEDS	(END_LED)

extern led_ctx_t led_ctx[NUM_LEDS];
#endif /* PERIPHERY_LED_LED_CONFIG_H_ */
