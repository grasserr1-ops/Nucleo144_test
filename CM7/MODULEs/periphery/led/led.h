/*
 * led.h
 *
 *  Created on: 16 мая 2026 г.
 *      Author: grasser
 */

#ifndef PERIPHERY_LED_LED_H_
#define PERIPHERY_LED_LED_H_

#include "main.h"

#define BLINK_INF	UINT32_MAX

enum {
	LED_OK,
	UNKNOWN_LED
};

typedef struct{
	GPIO_TypeDef *led_gpio;
	uint16_t led_pin;
	uint8_t cycle_flag;
	uint8_t hard_flag;
	uint32_t blink_time;
	uint32_t blink_timer;
}led_ctx_t;

void ledTask(void *arg);
uint8_t blink_led(uint8_t led_id, uint32_t blink_time);
uint8_t cycle_blink_led(uint8_t led_id, uint32_t blink_time);
uint8_t set_led(uint8_t led_id);
uint8_t reset_led(uint8_t led_id);
uint8_t hard_set_led(uint8_t led_id);
uint8_t hard_reset_led(uint8_t led_id);
uint8_t toggle_led(uint8_t led_id);
uint8_t hard_toggle_led(uint8_t led_id);
uint8_t reset_hard_led_flag(uint8_t led_id);


#endif /* PERIPHERY_LED_LED_H_ */
