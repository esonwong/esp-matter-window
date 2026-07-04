#pragma once
#include <stdint.h>

void matter_app_init(void);

// 由 window_ctrl 回调触发，更新 Matter 属性
void matter_app_update_position(uint16_t pos_100ths, int state);
