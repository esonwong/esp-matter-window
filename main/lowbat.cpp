#include "lowbat.h"
#include "diag_log.h"
#include "window_ctrl.h"
#include "motor_ctrl.h"
#include "led_ctrl.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LOWBAT";

static void enter_deep_sleep(int16_t mv)
{
    ESP_LOGW(TAG, "vbat=%d mV，进入 deep sleep %d s", mv, LOWBAT_SLEEP_SEC);
    esp_sleep_enable_timer_wakeup((uint64_t)LOWBAT_SLEEP_SEC * 1000000ULL);
    esp_deep_sleep_start();
}

void lowbat_early_check(void)
{
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) return;
    diag_adc_init();
    int16_t mv = diag_get_vbat_mv();
    if (mv <= 0) return;  // ADC 不可用：不敢下判断，正常启动
    if (mv < LOWBAT_RESUME_MV) {
        // 仍在低电量：不写 NVS、不起协议栈，直接再睡
        enter_deep_sleep(mv);
    }
    ESP_LOGI(TAG, "deep sleep 唤醒，vbat=%d mV ≥ %d，恢复正常运行", mv, LOWBAT_RESUME_MV);
}

static bool window_idle(void)
{
    window_state_t st = window_ctrl_get_state();
    return st == WINDOW_OPEN || st == WINDOW_CLOSED || st == WINDOW_STOPPED;
}

static void lowbat_task(void *)
{
    int below = 0;
    // 启动后先等 2 分钟：让 Matter/Thread 起来、电压读数稳定
    vTaskDelay(pdMS_TO_TICKS(LOWBAT_STARTUP_DELAY_SEC * 1000));
    for (;;) {
        int16_t mv = diag_get_vbat_mv();
        if (mv > 0 && mv < LOWBAT_SLEEP_MV) {
            below++;
            ESP_LOGW(TAG, "vbat=%d mV < %d (%d/%d)", mv, LOWBAT_SLEEP_MV, below, LOWBAT_SLEEP_SAMPLES);
        } else {
            below = 0;
        }
        if (below >= LOWBAT_SLEEP_SAMPLES && window_idle()) {
            diag_log_event(DIAG_LOWBAT, 1);   // 落盘（内部 save_ring）
            motor_ctrl_coast();
            led_ctrl_set_mode(LED_OFF);
            vTaskDelay(pdMS_TO_TICKS(200));    // 给 NVS 写入 / 日志一点时间
            enter_deep_sleep(mv);
        }
        vTaskDelay(pdMS_TO_TICKS(LOWBAT_SAMPLE_SEC * 1000));
    }
}

void lowbat_monitor_start(void)
{
    xTaskCreate(lowbat_task, "lowbat", 3072, nullptr, 1, nullptr);
    ESP_LOGI(TAG, "低电量监测已启动 (sleep<%d mV, resume≥%d mV)", LOWBAT_SLEEP_MV, LOWBAT_RESUME_MV);
}
