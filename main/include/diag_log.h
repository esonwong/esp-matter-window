#pragma once
#include <stdint.h>

// 板上诊断日志：环形缓冲，存 NVS namespace "diag"
// 每条 16 字节，共 256 条 = 4 KB，循环覆盖最旧
// 覆盖时长：空闲 ~10 天 / 中度使用 ~5 天（足够覆盖一个完整电池周期）
//
// 用途：调研电池续航/太阳能补能/Matter 行为
// dump 方式：烧调试固件（USJ console 开启）→ boot 时自动 printf 全部条目

enum diag_type_t : uint8_t {
    DIAG_BOOT = 1,        // 系统启动；aux1=reset_reason
    DIAG_HOURLY = 2,      // 每小时快照
    DIAG_MOTOR_CMD = 3,   // 电机指令；aux1=方向 (1=open,2=close,3=stop,4=move,5=cal)
    DIAG_MOTOR_DONE = 4,  // 电机停止；aux1=最终 state
    DIAG_BUTTON = 5,      // 按键；aux1=点击次数 (1/2/3)
    DIAG_STATE = 6,       // window_state 变化；aux1=新 state
};

struct __attribute__((packed)) diag_event_t {
    uint32_t uptime_s;     // 4: 开机后秒数
    uint8_t  type;         // 1: diag_type_t
    uint8_t  aux1;         // 1: 事件相关辅助数据
    int16_t  vbat_mv;      // 2: 电池电压 mV（ADC 未接时为 0；负值=读失败）
    uint8_t  position;     // 1: 窗位 0~100
    uint8_t  state;        // 1: window_state_t
    uint16_t motor_count;  // 2: 本次启动后电机启动累计次数
    uint16_t button_count; // 2: 本次启动后按键累计次数
    uint16_t free_heap_kb; // 2: 当前剩余 heap (KB)
};
static_assert(sizeof(diag_event_t) == 16, "diag_event_t must be 16 bytes");

// 初始化：注册 ADC、加载历史 head、启动每小时快照任务
void diag_log_init(void);

// 记录一条事件（aux1 见 diag_type_t 注释）
void diag_log_event(diag_type_t type, uint8_t aux1 = 0);

// dump：按时间顺序 printf 所有条目（一行一条 CSV）
// 仅在 USJ console 开启的调试固件里有效
void diag_log_dump(void);

// 计数器 API（供 window_ctrl 在事件发生时累加）
void diag_inc_motor(void);
void diag_inc_button(void);

// 设置位置上下文（每次 set_state 调用）
void diag_set_context(uint8_t state, uint16_t pos_100ths);

// 读取电池电压（mV）。返回 0 = ADC 未初始化；返回 < 0 = 读取失败
// 内部已平均 8 次采样
int16_t diag_get_vbat_mv(void);
