#include "led_ctrl.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <atomic>

static const char *TAG = "LED";

static gpio_num_t         s_gpio = GPIO_NUM_NC;
static std::atomic<led_mode_t> s_mode{LED_OFF};
static int                s_tick = 0;
static esp_timer_handle_t s_timer = nullptr;

// 每 100ms 触发一次
static void led_timer_cb(void *)
{
    s_tick++;
    led_mode_t mode = s_mode.load();
    int level = 0;

    // active-low: gpio=0 → LED on, gpio=1 → LED off
    switch (mode) {
    case LED_OFF:
        level = 1;           // off
        break;
    case LED_ON:
        level = 0;           // on（常亮）
        break;
    case LED_BLINK_FAST:   // 200ms 周期：1 on / 1 off
        level = (s_tick % 2) ? 1 : 0;
        break;
    case LED_BLINK_SLOW:   // 800ms 周期：1 on / 7 off
        level = (s_tick % 8 == 0) ? 0 : 1;
        break;
    }

    // 再次确认模式，防止 set_mode 关灯后 timer 又拉高
    if (s_mode.load() != mode) level = 1;  // active-low off

    gpio_set_level(s_gpio, level);
}

void led_ctrl_init(gpio_num_t gpio)
{
    s_gpio = gpio;

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << gpio);
    cfg.mode         = GPIO_MODE_OUTPUT;
    gpio_config(&cfg);
    gpio_set_level(s_gpio, 1);  // active-low: 1 = off

    esp_timer_create_args_t args = {
        .callback        = led_timer_cb,
        .arg             = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "led",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&args, &s_timer);
    // 初始状态 LED_OFF，不启动定时器
}

void led_ctrl_set_mode(led_mode_t mode)
{
    if (s_mode.load() == mode) return;
    const char *name[] = {"OFF", "ON", "FAST", "SLOW"};
    ESP_LOGI(TAG, "%s → %s", name[s_mode.load()], name[mode]);
    s_tick = 0;
    s_mode.store(mode);
    if (mode == LED_OFF) {
        gpio_set_level(s_gpio, 1);          // active-low: 立即关灯
        esp_timer_stop(s_timer);            // 停止定时器，省 ~0.1 mA
    } else if (!esp_timer_is_active(s_timer)) {
        esp_timer_start_periodic(s_timer, 100 * 1000);  // 重新启动
    }
}
