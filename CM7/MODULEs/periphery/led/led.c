/*
 * led.c
 *
 *  Created on: 16 мая 2026 г.
 *      Author: grasser
 */

#include "led.h"
#include "led_config.h"
#include "cmsis_os2.h"

uint8_t blink_led(uint8_t led_id, uint32_t blink_time) {
	if (led_id >= NUM_LEDS)
		return UNKNOWN_LED;
	led_ctx[led_id].cycle_flag = 0;
	led_ctx[led_id].blink_time = blink_time;
	return LED_OK;
}

uint8_t cycle_blink_led(uint8_t led_id, uint32_t blink_time) {
	if (led_id >= NUM_LEDS)
		return UNKNOWN_LED;
	led_ctx[led_id].cycle_flag = 1;
	led_ctx[led_id].blink_timer = blink_time/2;
	led_ctx[led_id].blink_time = blink_time/2;
	return LED_OK;
}

uint8_t set_led(uint8_t led_id) {
	if (led_id >= NUM_LEDS)
		return UNKNOWN_LED;
	led_ctx[led_id].cycle_flag = 0;
	led_ctx[led_id].blink_time = BLINK_INF;
	return LED_OK;
}

uint8_t reset_led(uint8_t led_id) {
	if (led_id >= NUM_LEDS)
		return UNKNOWN_LED;
	led_ctx[led_id].cycle_flag = 0;
	led_ctx[led_id].blink_time = 0;
	return LED_OK;
}

uint8_t hard_set_led(uint8_t led_id) {
	if (led_id >= NUM_LEDS)
		return UNKNOWN_LED;
	led_ctx[led_id].hard_flag = 1;
	led_ctx[led_id].cycle_flag = 0;
	led_ctx[led_id].blink_time = BLINK_INF;
	return LED_OK;
}

uint8_t hard_reset_led(uint8_t led_id) {
	if (led_id >= NUM_LEDS)
		return UNKNOWN_LED;
	led_ctx[led_id].hard_flag = 1;
	led_ctx[led_id].cycle_flag = 0;
	led_ctx[led_id].blink_time = 0;
	return LED_OK;
}

uint8_t toggle_led(uint8_t led_id) {
	if (led_id >= NUM_LEDS)
		return UNKNOWN_LED;
	if (led_ctx[led_id].blink_time) {
		reset_led(led_id);
	} else {
		set_led(led_id);
	}
	return LED_OK;
}

uint8_t hard_toggle_led(uint8_t led_id) {
	if (led_id >= NUM_LEDS)
		return UNKNOWN_LED;
	if (led_ctx[led_id].blink_time) {
		hard_reset_led(led_id);
	} else {
		hard_set_led(led_id);
	}
	return LED_OK;
}

uint8_t reset_hard_led_flag(uint8_t led_id) {
	if (led_id >= NUM_LEDS)
		return UNKNOWN_LED;
	led_ctx[led_id].hard_flag = 0;
	return LED_OK;
}

static void  ledTask_init() {
	for (int i = 0; i < NUM_LEDS; i++) {
		led_ctx[i].blink_time = 0;
		led_ctx[i].blink_timer = 0;
		led_ctx[i].cycle_flag = 0;
		led_ctx[i].hard_flag = 0;
	}
}


void ledTask(void *arg) {
	ledTask_init();
	while (1) {
		for(int i = 0; i < NUM_LEDS; i++) {
			if (led_ctx[i].hard_flag) {
				if (led_ctx[i].blink_time) {
					HAL_GPIO_WritePin(led_ctx[i].led_gpio, led_ctx[i].led_pin, GPIO_PIN_SET);
				} else {
					HAL_GPIO_WritePin(led_ctx[i].led_gpio, led_ctx[i].led_pin, GPIO_PIN_RESET);
				}
				continue;
			}

			if (led_ctx[i].blink_time) {
				if (!led_ctx[i].cycle_flag)
 					HAL_GPIO_WritePin(led_ctx[i].led_gpio, led_ctx[i].led_pin, GPIO_PIN_SET);
				if (led_ctx[i].blink_time != BLINK_INF)
					led_ctx[i].blink_time--;
			} else {
				if (led_ctx[i].cycle_flag) {
					HAL_GPIO_TogglePin(led_ctx[i].led_gpio, led_ctx[i].led_pin);
					led_ctx[i].blink_time = led_ctx[i].blink_timer;
				} else {
					HAL_GPIO_WritePin(led_ctx[i].led_gpio, led_ctx[i].led_pin, GPIO_PIN_RESET);
				}
			}
		}
		osDelay(1);
	}
}


