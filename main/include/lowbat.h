#pragma once
// ── 低电量保护 ────────────────────────────────────────────────────────────
// 背景：2026-08-16 dump 显示电池被放到 2.4 V 后设备在 brownout 循环里打转，
// 把一块几周新的电池深放电。ESP+Thread idle ~16 mA 会一直吸到电池死，
// 所以电压过低时主动进 deep sleep（~µA 级 + DRV8833 静态 1.6 mA），
// 定时醒来看有没有被 USB / 太阳能充回来。
//
// 阈值（迟滞）：
//   LOWBAT_SLEEP_MV  连续 LOWBAT_SLEEP_SAMPLES 次采样低于此值 → deep sleep
//   LOWBAT_RESUME_MV deep sleep 唤醒后高于此值 → 正常启动，否则再睡
// 均可用 EXTRA_CXXFLAGS=-DLOWBAT_xxx=... 覆盖（测试用）
#ifndef LOWBAT_SLEEP_MV
#define LOWBAT_SLEEP_MV       3100
#endif
#ifndef LOWBAT_RESUME_MV
#define LOWBAT_RESUME_MV      3400
#endif
#ifndef LOWBAT_SLEEP_SAMPLES
#define LOWBAT_SLEEP_SAMPLES  3
#endif
#ifndef LOWBAT_SAMPLE_SEC
#define LOWBAT_SAMPLE_SEC     60
#endif
#ifndef LOWBAT_SLEEP_SEC
#define LOWBAT_SLEEP_SEC      600
#endif
#ifndef LOWBAT_STARTUP_DELAY_SEC
#define LOWBAT_STARTUP_DELAY_SEC 120
#endif

// app_main 最前面调用（Matter/Thread 都还没起）：
// 若本次是 deep sleep 定时唤醒且电压仍 < LOWBAT_RESUME_MV，直接再睡，不返回。
void lowbat_early_check(void);

// 启动后台监测任务（在 window_ctrl_init 之后调用）
void lowbat_monitor_start(void);
