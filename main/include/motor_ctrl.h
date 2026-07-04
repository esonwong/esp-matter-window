#pragma once

#include <stdint.h>

// XIAO ESP32-C6: D0=GPIO2 (AIN1), D1=GPIO3 (AIN2)
// 驱动芯片：DRV8833，单通道 H 桥

// 初始化电机 PWM（LEDC_TIMER_1, CHANNEL_1/2，20kHz）
void motor_ctrl_init(void);

// 正转，duty 0–100（百分比）
void motor_ctrl_forward(uint8_t duty_pct);

// 反转，duty 0–100
void motor_ctrl_reverse(uint8_t duty_pct);

// 滑行停止（AIN1=0, AIN2=0）
void motor_ctrl_coast(void);

// 刹车停止（AIN1=1, AIN2=1）
void motor_ctrl_brake(void);
