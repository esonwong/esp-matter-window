#include "matter_app.h"
#include "window_ctrl.h"
#include "diag_log.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <nvs_flash.h>

static const char *TAG = "MAIN";

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW (esp_restart)";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "UNKNOWN";
    }
}

// window_ctrl 状态变化回调 → 更新 Matter 属性
static void on_window_state(window_state_t state, uint16_t pos_100ths)
{
    matter_app_update_position(pos_100ths, (int)state);
}

// 通过全局变量传给 matter_app 之后再打印（boot 早期 USB-JTAG console 还没起来）
esp_reset_reason_t g_reset_reason_at_boot = ESP_RST_UNKNOWN;

extern "C" const char *main_reset_reason_str(int r)
{
    return reset_reason_str((esp_reset_reason_t)r);
}

extern "C" void app_main(void)
{
    g_reset_reason_at_boot = esp_reset_reason();

    // NVS（Matter 和 Wi-Fi 凭据必须）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 板上诊断日志（NVS 环形缓冲 + ADC 电池电压采样）
    diag_log_init();

    // 开窗器状态机（电机 + LED + 按键）
    window_ctrl_init(on_window_state);

    // Matter 协议栈（WindowCovering 设备）
    matter_app_init();

    ESP_LOGI(TAG, "初始化完成");
}
