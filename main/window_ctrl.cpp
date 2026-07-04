#include "window_ctrl.h"
#include "led_ctrl.h"
#include "motor_ctrl.h"
#include "diag_log.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "iot_button.h"
#include "button_gpio.h"
#include <atomic>
#include <platform/CHIPDeviceLayer.h>
#if CHIP_CONFIG_ENABLE_ICD_SERVER
#include <app/icd/server/ICDNotifier.h>
#endif

static const char *TAG = "WINDOW";

// ── 硬件引脚 ──────────────────────────────────────────────────────────────
#define LED_GPIO GPIO_NUM_15  // XIAO 板载 LED
#define BTN_GPIO GPIO_NUM_9   // XIAO BOOT 按键（低电平有效）
#define HALL_GPIO GPIO_NUM_23 // D5 霍尔传感器（低电平有效，内部上拉；两端各一块磁铁）

// ── 行程参数 ──────────────────────────────────────────────────────────────
#define TICK_MS 100
#define TRAVEL_MS_DEF 180000
#define TRAVEL_MS_MIN 5000
#define TRAVEL_MS_MAX 300000

// ── NVS ───────────────────────────────────────────────────────────────────
#define NVS_NS "window"
#define NVS_KEY_TRAVEL "travel_ms"
#define NVS_KEY_REVERSED "reversed"
#define NVS_KEY_POS "pos"

// ── 状态变量 ──────────────────────────────────────────────────────────────
#define EVT_MOVE BIT0

static EventGroupHandle_t s_evt;
static std::atomic<window_state_t> s_state{WINDOW_CLOSED};
static std::atomic<uint16_t> s_pos{10000};
static std::atomic<uint16_t> s_target{10000};
static window_state_cb_t s_cb = nullptr;
static uint32_t s_travel_ms = TRAVEL_MS_DEF;
static bool s_reversed = false;
static TaskHandle_t s_window_task_handle = nullptr;
static TaskHandle_t s_cal_task_handle = nullptr;
static std::atomic<bool> s_cal_stop{false};

// ── NVS ───────────────────────────────────────────────────────────────────
static void load_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return;
    uint32_t t = 0;
    if (nvs_get_u32(h, NVS_KEY_TRAVEL, &t) == ESP_OK && t >= TRAVEL_MS_MIN && t <= TRAVEL_MS_MAX)
        s_travel_ms = t;
    uint8_t rev = 0;
    if (nvs_get_u8(h, NVS_KEY_REVERSED, &rev) == ESP_OK)
        s_reversed = (rev != 0);
    uint16_t pos = 10000;
    if (nvs_get_u16(h, NVS_KEY_POS, &pos) == ESP_OK)
        s_pos.store(pos);
    nvs_close(h);
    ESP_LOGI(TAG, "行程 %lu ms  方向%s  初始位置 %d", s_travel_ms, s_reversed ? "反转" : "正常", s_pos.load());
}

static void save_pos(uint16_t pos)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u16(h, NVS_KEY_POS, pos);
    nvs_commit(h);
    nvs_close(h);
}

static void save_travel_ms(uint32_t ms)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u32(h, NVS_KEY_TRAVEL, ms);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "行程时长已更新 %lu ms", ms);
}

// ── 动态步进 ──────────────────────────────────────────────────────────────
static uint16_t pos_step(void)
{
    return (uint16_t)(10000 / (s_travel_ms / TICK_MS));
}

// ── 带方向的电机指令 ──────────────────────────────────────────────────────
static void motor_open(void) { s_reversed ? motor_ctrl_reverse(100) : motor_ctrl_forward(100); }
static void motor_close(void) { s_reversed ? motor_ctrl_forward(100) : motor_ctrl_reverse(100); }

// ── 状态机转换：entry action + LED + 回调 ────────────────────────────────
static void set_state(window_state_t st)
{
    s_state.store(st);
    diag_set_context((uint8_t)st, s_pos.load());
    diag_log_event(DIAG_STATE, (uint8_t)st);
    switch (st)
    {
    case WINDOW_OPENING:
    case WINDOW_LEAVING_CLOSED:
        motor_open();
        diag_inc_motor();
        led_ctrl_set_mode(LED_BLINK_FAST);
        xEventGroupSetBits(s_evt, EVT_MOVE);
        break;
    case WINDOW_CLOSING:
    case WINDOW_LEAVING_OPEN:
        motor_close();
        diag_inc_motor();
        led_ctrl_set_mode(LED_BLINK_SLOW);
        xEventGroupSetBits(s_evt, EVT_MOVE);
        break;
    default: // OPEN / CLOSED / STOPPED
        motor_ctrl_coast();
        led_ctrl_set_mode(LED_OFF);
        save_pos(s_pos.load());
        break;
    }
    if (s_cb)
        s_cb(st, s_pos.load());
}

// ── 校准任务（独立，暂停 window_task） ───────────────────────────────────
static void calibrate_task(void *arg)
{
    led_ctrl_set_mode(LED_ON);
    vTaskSuspend(s_window_task_handle);
    s_cal_stop.store(false);

    // ── 阶段 1：前往全开端（霍尔精确定位） ──────────────────────────────
    ESP_LOGI(TAG, "[CAL] 前往全开端");
    motor_open();
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(TRAVEL_MS_MAX);
    bool at_open = false;
    while (xTaskGetTickCount() < deadline)
    {
        if (s_cal_stop.load())
            break;
        if (gpio_get_level(HALL_GPIO) == 0)
        {
            at_open = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    motor_ctrl_brake();
    vTaskDelay(pdMS_TO_TICKS(200));
    motor_ctrl_coast();

    if (!at_open)
    {
        ESP_LOGW(TAG, "[CAL] 未到达全开端，放弃");
        goto cal_done;
    }
    s_pos.store(0);
    s_state.store(WINDOW_OPEN);
    if (s_cb)
        s_cb(WINDOW_OPEN, 0);
    save_pos(0);
    vTaskDelay(pdMS_TO_TICKS(500));

    // ── 阶段 2：从全开端计时到全关（霍尔触发停止） ──────────────────────
    ESP_LOGI(TAG, "[CAL] 开始向全关运动");
    {
        motor_close();
        deadline = xTaskGetTickCount() + pdMS_TO_TICKS(TRAVEL_MS_MAX);

        // 等开端磁铁离开传感器
        while (xTaskGetTickCount() < deadline)
        {
            if (s_cal_stop.load())
                goto cal_stop;
            if (gpio_get_level(HALL_GPIO) != 0)
                break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        // 记录开始时刻，等待关端磁铁触发
        {
            TickType_t start = xTaskGetTickCount();
            deadline = start + pdMS_TO_TICKS(TRAVEL_MS_MAX);
            bool at_close = false;
            while (xTaskGetTickCount() < deadline)
            {
                if (s_cal_stop.load())
                    break;
                if (gpio_get_level(HALL_GPIO) == 0)
                {
                    at_close = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            uint32_t elapsed = (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
            motor_ctrl_brake();
            vTaskDelay(pdMS_TO_TICKS(200));
            motor_ctrl_coast();

            if (at_close && elapsed >= TRAVEL_MS_MIN && elapsed <= TRAVEL_MS_MAX)
            {
                s_travel_ms = elapsed;
                save_travel_ms(elapsed);
                ESP_LOGI(TAG, "[CAL] 完成，行程 %lu ms", s_travel_ms);
            }
            else
            {
                ESP_LOGW(TAG, "[CAL] 未到全关端或时长超范围 (%lu ms)，未保存", elapsed);
            }
            s_pos.store(10000);
            s_state.store(WINDOW_CLOSED);
            if (s_cb)
                s_cb(WINDOW_CLOSED, 10000);
            save_pos(10000);
        }
        goto cal_done;

    cal_stop:
        motor_ctrl_brake();
        vTaskDelay(pdMS_TO_TICKS(200));
        motor_ctrl_coast();
        ESP_LOGI(TAG, "[CAL] 已中止");
    }

cal_done:
    led_ctrl_set_mode(LED_OFF);
    s_cal_task_handle = nullptr;
    vTaskResume(s_window_task_handle);
    vTaskDelete(nullptr);
}

// ── 按键回调 ──────────────────────────────────────────────────────────────
void window_ctrl_calibrate(void);
void window_ctrl_toggle_reverse(void);

// 通知 Matter ICD 栈：用户触发活跃模式（UAT），使 LIT 设备从睡眠唤醒
// 必须在 CHIP 任务上下文内调用，通过 ScheduleWork 投递
#if CHIP_CONFIG_ENABLE_ICD_SERVER
static void icd_wake_work(intptr_t) {
    chip::app::ICDNotifier::GetInstance().NotifyNetworkActivityNotification();
}
static inline void icd_wake() {
    chip::DeviceLayer::PlatformMgr().ScheduleWork(icd_wake_work);
}
#else
static inline void icd_wake() {}
#endif

static void btn_cb_single(void *, void *)
{
    icd_wake();
    diag_inc_button();
    diag_log_event(DIAG_BUTTON, 1);
    if (s_cal_task_handle)
        s_cal_stop.store(true); // 校准中：停止并记录
    else
        window_ctrl_toggle();
}
static void btn_cb_double(void *, void *)
{
    icd_wake();
    diag_inc_button();
    diag_log_event(DIAG_BUTTON, 2);
    if (!s_cal_task_handle)
        window_ctrl_toggle_reverse();
}
static void btn_cb_three(void *, void *)
{
    icd_wake();
    diag_inc_button();
    diag_log_event(DIAG_BUTTON, 3);
    window_ctrl_calibrate();
}

// ── 控制任务：纯监控，不发电机指令 ──────────────────────────────────────
static void window_task(void *arg)
{
    int report_tick = 0;
    while (1)
    {
        window_state_t st = s_state.load();

        if (st != WINDOW_OPENING && st != WINDOW_CLOSING &&
            st != WINDOW_LEAVING_OPEN && st != WINDOW_LEAVING_CLOSED)
        {
            // 静止：挂起等待运动命令，CPU 可进 light sleep
            report_tick = 0;
            xEventGroupWaitBits(s_evt, EVT_MOVE, pdTRUE, pdFALSE, portMAX_DELAY);
            continue;
        }

        // 运动中：监控位置和霍尔传感器（电机已由 set_state entry action 启动）
        uint16_t pos = s_pos.load();
        uint16_t tgt = s_target.load();
        uint16_t step = pos_step();
        bool hall = (gpio_get_level(HALL_GPIO) == 0);
        report_tick++;

        switch (st)
        {
        case WINDOW_OPENING:
            if (tgt == 0)
            {
                // 全开：靠霍尔触发，位置仅用于上报
                if (hall)
                {
                    motor_ctrl_brake();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    s_pos.store(0);
                    set_state(WINDOW_OPEN);
                    ESP_LOGI(TAG, "全开（霍尔）");
                }
                else
                {
                    s_pos.store(pos < step ? (uint16_t)0 : pos - step);
                    if (s_cb && report_tick % 10 == 0)
                        s_cb(st, s_pos.load());
                }
            }
            else
            {
                // 中途停靠：靠位置
                if (pos <= tgt)
                {
                    motor_ctrl_brake();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    s_pos.store(tgt);
                    set_state(WINDOW_STOPPED);
                    ESP_LOGI(TAG, "停止 pos=%d", tgt);
                }
                else
                {
                    s_pos.store(pos - step);
                    if (s_cb && report_tick % 10 == 0)
                        s_cb(st, s_pos.load());
                }
            }
            break;

        case WINDOW_CLOSING:
            if (tgt == 10000)
            {
                // 全关：靠霍尔触发，位置仅用于上报
                if (hall)
                {
                    motor_ctrl_brake();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    s_pos.store(10000);
                    set_state(WINDOW_CLOSED);
                    ESP_LOGI(TAG, "全关（霍尔）");
                }
                else
                {
                    s_pos.store(pos + step > 10000 ? (uint16_t)10000 : pos + step);
                    if (s_cb && report_tick % 10 == 0)
                        s_cb(st, s_pos.load());
                }
            }
            else
            {
                // 中途停靠：靠位置
                if (pos + step >= tgt)
                {
                    motor_ctrl_brake();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    s_pos.store(tgt);
                    set_state(WINDOW_STOPPED);
                    ESP_LOGI(TAG, "停止 pos=%d", tgt);
                }
                else
                {
                    s_pos.store(pos + step);
                    if (s_cb && report_tick % 10 == 0)
                        s_cb(st, s_pos.load());
                }
            }
            break;

        case WINDOW_LEAVING_OPEN:
            // 等待开端磁铁离开传感器，之后进入正常关闭行程
            if (!hall)
                set_state(WINDOW_CLOSING);
            break;

        case WINDOW_LEAVING_CLOSED:
            // 等待关端磁铁离开传感器，之后进入正常开启行程
            if (!hall)
                set_state(WINDOW_OPENING);
            break;

        default:
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

// ── 公开 API ──────────────────────────────────────────────────────────────
void window_ctrl_init(window_state_cb_t cb)
{
    s_cb = cb;
    s_evt = xEventGroupCreate();
    load_settings();

    led_ctrl_init(LED_GPIO);

    button_config_t btn_cfg = {};
    button_gpio_config_t gpio_cfg = {
        .gpio_num = BTN_GPIO,
        .active_level = 0,
        .enable_power_save = true,
        .disable_pull = false,
    };
    button_handle_t btn = nullptr;
    iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn);

    button_event_args_t args2 = {.multiple_clicks = {.clicks = 2}};
    button_event_args_t args3 = {.multiple_clicks = {.clicks = 3}};
    iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, nullptr, btn_cb_single, nullptr);
    iot_button_register_cb(btn, BUTTON_MULTIPLE_CLICK, &args2, btn_cb_double, nullptr);
    iot_button_register_cb(btn, BUTTON_MULTIPLE_CLICK, &args3, btn_cb_three, nullptr);

    gpio_config_t hall_cfg = {};
    hall_cfg.pin_bit_mask = (1ULL << HALL_GPIO);
    hall_cfg.mode = GPIO_MODE_INPUT;
    hall_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    hall_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    hall_cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&hall_cfg);

    motor_ctrl_init();
    xTaskCreate(window_task, "window", 4096, nullptr, 5, &s_window_task_handle);
    ESP_LOGI(TAG, "初始化完成");
}

void window_ctrl_open(void)
{
    window_state_t cur = s_state.load();
    if (cur == WINDOW_OPEN)
        return;
    ESP_LOGI(TAG, "开窗");
    s_target.store(0);
    set_state(cur == WINDOW_CLOSED ? WINDOW_LEAVING_CLOSED : WINDOW_OPENING);
}

void window_ctrl_close(void)
{
    window_state_t cur = s_state.load();
    if (cur == WINDOW_CLOSED)
        return;
    ESP_LOGI(TAG, "关窗");
    s_target.store(10000);
    set_state(cur == WINDOW_OPEN ? WINDOW_LEAVING_OPEN : WINDOW_CLOSING);
}

void window_ctrl_move_to(uint16_t pos)
{
    uint16_t current = s_pos.load();
    if (pos == current)
        return;
    ESP_LOGI(TAG, "移动至 %d（当前 %d）", pos, current);
    s_target.store(pos);
    window_state_t cur = s_state.load();
    if (pos < current)
        set_state(cur == WINDOW_CLOSED ? WINDOW_LEAVING_CLOSED : WINDOW_OPENING);
    else
        set_state(cur == WINDOW_OPEN ? WINDOW_LEAVING_OPEN : WINDOW_CLOSING);
}

void window_ctrl_calibrate(void)
{
    if (s_cal_task_handle)
        return; // 已在校准中
    ESP_LOGI(TAG, "开始校准");
    xTaskCreate(calibrate_task, "cal", 4096, nullptr, 5, &s_cal_task_handle);
}

void window_ctrl_stop(void)
{
    motor_ctrl_brake();
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "停止 pos=%d", s_pos.load());
    set_state(WINDOW_STOPPED); // entry: coast
}

void window_ctrl_toggle(void)
{
    switch (s_state.load())
    {
    case WINDOW_OPENING:
    case WINDOW_LEAVING_CLOSED:
    case WINDOW_CLOSING:
    case WINDOW_LEAVING_OPEN:
        window_ctrl_stop();
        break;
    case WINDOW_CLOSED:
        window_ctrl_open();
        break;
    case WINDOW_OPEN:
        window_ctrl_close();
        break;
    case WINDOW_STOPPED:
        window_ctrl_open();
        break;
    }
}

void window_ctrl_toggle_reverse(void)
{
    switch (s_state.load())
    {
    case WINDOW_OPENING:
    case WINDOW_LEAVING_CLOSED:
        set_state(WINDOW_STOPPED);
        vTaskDelay(pdMS_TO_TICKS(500));
        window_ctrl_close();
        break; // 反向：先滑行停，再改为关
    case WINDOW_CLOSING:
    case WINDOW_LEAVING_OPEN:
        set_state(WINDOW_STOPPED);
        vTaskDelay(pdMS_TO_TICKS(500));
        window_ctrl_open();
        break; // 反向：先滑行停，再改为开
    case WINDOW_OPEN:
        window_ctrl_close();
        break;
    case WINDOW_CLOSED:
        window_ctrl_open();
        break;
    case WINDOW_STOPPED:
        window_ctrl_close();
        break; // 与单击相反：倾向关
    }
}

window_state_t window_ctrl_get_state(void) { return s_state.load(); }
uint16_t window_ctrl_get_position(void) { return s_pos.load(); }
