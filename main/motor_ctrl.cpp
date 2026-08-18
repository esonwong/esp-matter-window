#include "motor_ctrl.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_pm.h"

static const char *TAG = "MOTOR";

#define MOTOR_AIN1 GPIO_NUM_0 // D0 → DRV8833 IN1
#define MOTOR_AIN2 GPIO_NUM_1 // D1 → DRV8833 IN2
// DRV8833 nSLEEP：目前硬件上硬连 VCC（静态 ~1.6 mA）。飞线到某个 GPIO 后把这里改成对应引脚
// （建议 D6/GPIO16 或 D7/GPIO17，未被占用），静止时拉低进 sleep（~2 µA），启动前拉高等 1 ms。
// 保持 GPIO_NUM_NC 时以下逻辑全部不生效。
#define MOTOR_NSLEEP_GPIO GPIO_NUM_NC
#define MOTOR_NSLEEP_WAKE_MS 1

// light sleep 会把 GPIO 隔离成高阻，电机跑一半睡过去 IN1/IN2 就悬空了。
// 驱动期间持 NO_LIGHT_SLEEP 锁，coast 时释放。
static esp_pm_lock_handle_t s_motor_pm_lock = nullptr;
static bool s_motor_lock_held = false;
static void motor_pm_hold(bool hold)
{
    if (!s_motor_pm_lock || hold == s_motor_lock_held) return;
    if (hold) esp_pm_lock_acquire(s_motor_pm_lock);
    else      esp_pm_lock_release(s_motor_pm_lock);
    s_motor_lock_held = hold;
}

static inline void nsleep_set(bool awake)
{
    if (MOTOR_NSLEEP_GPIO == GPIO_NUM_NC) return;
    gpio_set_level(MOTOR_NSLEEP_GPIO, awake ? 1 : 0);
    if (awake) vTaskDelay(pdMS_TO_TICKS(MOTOR_NSLEEP_WAKE_MS));
}

void motor_ctrl_init(void)
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << MOTOR_AIN1) | (1ULL << MOTOR_AIN2);
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
    // 即使睡进去也让这两个脚保持输出电平，而不是被隔离
    gpio_sleep_sel_dis(MOTOR_AIN1);
    gpio_sleep_sel_dis(MOTOR_AIN2);
#if CONFIG_PM_ENABLE
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "motor", &s_motor_pm_lock);
#endif

    if (MOTOR_NSLEEP_GPIO != GPIO_NUM_NC) {
        gpio_config_t ns = {};
        ns.pin_bit_mask = (1ULL << MOTOR_NSLEEP_GPIO);
        ns.mode = GPIO_MODE_OUTPUT;
        gpio_config(&ns);
        gpio_set_level(MOTOR_NSLEEP_GPIO, 0);  // 上电即 sleep
    }

    // 默认滑行停止
    gpio_set_level(MOTOR_AIN1, 0);
    gpio_set_level(MOTOR_AIN2, 0);
    ESP_LOGI(TAG, "电机初始化完成 (AIN1=GPIO%d, AIN2=GPIO%d)", MOTOR_AIN1, MOTOR_AIN2);
}

void motor_ctrl_forward(uint8_t duty_pct)
{
    (void)duty_pct; // 开窗器全速运行，忽略速度参数
    motor_pm_hold(true);
    nsleep_set(true);
    gpio_set_level(MOTOR_AIN1, 1);
    gpio_set_level(MOTOR_AIN2, 0);
    ESP_LOGI(TAG, "正转");
}

void motor_ctrl_reverse(uint8_t duty_pct)
{
    (void)duty_pct;
    motor_pm_hold(true);
    nsleep_set(true);
    gpio_set_level(MOTOR_AIN1, 0);
    gpio_set_level(MOTOR_AIN2, 1);
    ESP_LOGI(TAG, "反转");
}

void motor_ctrl_coast(void)
{
    gpio_set_level(MOTOR_AIN1, 0);
    gpio_set_level(MOTOR_AIN2, 0);
    nsleep_set(false);
    motor_pm_hold(false);
    ESP_LOGI(TAG, "滑行停止");
}

void motor_ctrl_brake(void)
{
    // gpio_set_level(MOTOR_AIN1, 1);
    // gpio_set_level(MOTOR_AIN2, 1);
    // 删除刹车功能，改为滑行停止
    // ESP_LOGI(TAG, "刹车停止");
    motor_ctrl_coast();
}
