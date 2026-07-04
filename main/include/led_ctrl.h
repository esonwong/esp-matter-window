#pragma once
#include "driver/gpio.h"

typedef enum {
    LED_OFF,
    LED_ON,           // 常亮（校准模式）
    LED_BLINK_FAST,   // 200ms 周期（开窗）
    LED_BLINK_SLOW,   // 800ms 周期（关窗）
} led_mode_t;

void led_ctrl_init(gpio_num_t gpio);
void led_ctrl_set_mode(led_mode_t mode);
