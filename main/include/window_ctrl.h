#pragma once
#include <stdint.h>

// 位置约定（Matter 规范）：0 = 全开，10000 = 全关
// 行程时间：30 秒

typedef enum {
    WINDOW_CLOSED         = 0,  // 全关，电机停
    WINDOW_OPENING        = 1,  // 开窗中（霍尔已离开）
    WINDOW_OPEN           = 2,  // 全开，电机停
    WINDOW_CLOSING        = 3,  // 关窗中（霍尔已离开）
    WINDOW_STOPPED        = 4,  // 中途停止
    WINDOW_LEAVING_OPEN   = 5,  // 从全开出发：电机已启动，霍尔仍有效
    WINDOW_LEAVING_CLOSED = 6,  // 从全关出发：电机已启动，霍尔区域外
} window_state_t;

// 状态/位置变化回调（在 window_ctrl 任务上下文中调用）
typedef void (*window_state_cb_t)(window_state_t state, uint16_t pos_100ths);

// 初始化（创建控制任务，注册回调）
void window_ctrl_init(window_state_cb_t cb);

void window_ctrl_open(void);             // 全开（霍尔触发或超时）
void window_ctrl_close(void);            // 全关
void window_ctrl_move_to(uint16_t pos);  // 移动到指定位置（0=全开，10000=全关）
void window_ctrl_calibrate(void);        // 校准行程：先到全开端，再计时跑到全关，保存到 NVS
void window_ctrl_stop(void);             // 立即停止
void window_ctrl_toggle(void);           // 单击：运动中暂停，静止时开↔关
void window_ctrl_toggle_reverse(void);   // 双击：运动中反向，静止时与单击相反

window_state_t window_ctrl_get_state(void);
uint16_t       window_ctrl_get_position(void);  // 0–10000
