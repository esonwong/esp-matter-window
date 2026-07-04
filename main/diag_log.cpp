#include "diag_log.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include <atomic>
#include <string.h>

static const char *TAG = "DIAG";

#define DIAG_NS         "diag"
#define DIAG_KEY_HEAD   "head"
#define DIAG_KEY_RING   "ring"
#define DIAG_RING_LEN   256

// ── ADC：D2 (GPIO2 = ADC1_CH2)，外接 100K+100K 分压到 BAT pad ──
#define VBAT_ADC_UNIT   ADC_UNIT_1
#define VBAT_ADC_CHAN   ADC_CHANNEL_2
#define VBAT_ADC_ATTEN  ADC_ATTEN_DB_12  // 0~3.1V，覆盖 4.2V/2 = 2.1V 安全
#define VBAT_DIVIDER    2                // 分压比

static adc_oneshot_unit_handle_t s_adc = nullptr;
static adc_cali_handle_t s_cali = nullptr;
static bool s_adc_ok = false;

static SemaphoreHandle_t s_mutex = nullptr;
static uint8_t s_head = 0;
static diag_event_t s_ring[DIAG_RING_LEN] = {};

static std::atomic<uint16_t> s_motor_count{0};
static std::atomic<uint16_t> s_button_count{0};
static std::atomic<uint8_t>  s_ctx_state{0};
static std::atomic<uint8_t>  s_ctx_position{0};

// ── ADC 初始化与读取 ─────────────────────────────────────────────────────
static void adc_init(void)
{
    adc_oneshot_unit_init_cfg_t init = { .unit_id = VBAT_ADC_UNIT };
    if (adc_oneshot_new_unit(&init, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "ADC unit 初始化失败");
        return;
    }
    adc_oneshot_chan_cfg_t chan = { .atten = VBAT_ADC_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_config_channel(s_adc, VBAT_ADC_CHAN, &chan) != ESP_OK) {
        ESP_LOGW(TAG, "ADC channel 配置失败");
        return;
    }
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = VBAT_ADC_UNIT,
        .chan = VBAT_ADC_CHAN,
        .atten = VBAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC 校准初始化失败");
        return;
    }
    s_adc_ok = true;
    ESP_LOGI(TAG, "ADC 已就绪 (D2/GPIO2)");
}

static int16_t read_vbat_mv(void)
{
    if (!s_adc_ok) return 0;
    int sum = 0, ok = 0;
    for (int i = 0; i < 8; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, VBAT_ADC_CHAN, &raw) != ESP_OK) continue;
        if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) continue;
        sum += mv;
        ok++;
    }
    if (ok == 0) return -1;
    return (int16_t)((sum / ok) * VBAT_DIVIDER);
}

int16_t diag_get_vbat_mv(void) { return read_vbat_mv(); }

// ── NVS 读写 ──────────────────────────────────────────────────────────────
static void load_ring(void)
{
    nvs_handle_t h;
    if (nvs_open(DIAG_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(s_ring);
    nvs_get_blob(h, DIAG_KEY_RING, s_ring, &sz);
    nvs_get_u8(h, DIAG_KEY_HEAD, &s_head);
    nvs_close(h);
    // LEN=256 时 s_head (uint8_t) 自然限制在 0..255，无需边界检查；
    // 若日后改成 LEN<256 需要在这里 clamp 回 0
    static_assert(DIAG_RING_LEN == 256, "调整 LEN 后需要重新加边界检查或加宽 s_head 类型");
}

static void save_ring(void)
{
    nvs_handle_t h;
    if (nvs_open(DIAG_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, DIAG_KEY_RING, s_ring, sizeof(s_ring));
    nvs_set_u8(h, DIAG_KEY_HEAD, s_head);
    nvs_commit(h);
    nvs_close(h);
}

// ── 公共 API ──────────────────────────────────────────────────────────────
void diag_inc_motor(void)  { s_motor_count.fetch_add(1); }
void diag_inc_button(void) { s_button_count.fetch_add(1); }

void diag_set_context(uint8_t state, uint16_t pos_100ths)
{
    s_ctx_state.store(state);
    s_ctx_position.store((uint8_t)(pos_100ths / 100));
}

void diag_log_event(diag_type_t type, uint8_t aux1)
{
    if (!s_mutex) return;
    diag_event_t ev = {};
    ev.uptime_s     = (uint32_t)(esp_timer_get_time() / 1000000);
    ev.type         = (uint8_t)type;
    ev.aux1         = aux1;
    ev.vbat_mv      = read_vbat_mv();
    ev.position     = s_ctx_position.load();
    ev.state        = s_ctx_state.load();
    ev.motor_count  = s_motor_count.load();
    ev.button_count = s_button_count.load();
    ev.free_heap_kb = (uint16_t)(esp_get_free_heap_size() / 1024);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_ring[s_head] = ev;
    s_head = (s_head + 1) % DIAG_RING_LEN;
    save_ring();
    xSemaphoreGive(s_mutex);
}

// ── 每小时快照任务 ──────────────────────────────────────────────────────
static void hourly_task(void *)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3600 * 1000));
        diag_log_event(DIAG_HOURLY, 0);
    }
}

// ── 调试固件：30 秒打印一次 VBAT，方便 live 观察充电曲线 ───────────────
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
static void debug_print_task(void *)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30 * 1000));
        int16_t v = read_vbat_mv();
        uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
        printf("[VBAT] t=%lus vbat=%d mV  heap=%lu KB\n",
            (unsigned long)up, v, (unsigned long)(esp_get_free_heap_size() / 1024));
    }
}
#endif

// ── Dump：CSV 格式打印所有条目（按时间顺序，最旧→最新）─────────────────
void diag_log_dump(void)
{
    printf("\n=== DIAG LOG DUMP (head=%u) ===\n", s_head);
    printf("uptime_s,type,aux1,vbat_mv,pos,state,motor_n,button_n,heap_kb\n");
    for (int i = 0; i < DIAG_RING_LEN; i++) {
        int idx = (s_head + i) % DIAG_RING_LEN;
        const diag_event_t &e = s_ring[idx];
        if (e.uptime_s == 0 && e.type == 0) continue;  // 空槽
        printf("%lu,%u,%u,%d,%u,%u,%u,%u,%u\n",
            (unsigned long)e.uptime_s, e.type, e.aux1, e.vbat_mv,
            e.position, e.state, e.motor_count, e.button_count, e.free_heap_kb);
    }
    printf("=== END DIAG DUMP ===\n\n");
}

// ── 初始化 ───────────────────────────────────────────────────────────────
void diag_log_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    adc_init();
    load_ring();

    // 启动事件，aux1 = reset_reason
    diag_log_event(DIAG_BOOT, (uint8_t)esp_reset_reason());

    // 调试固件（USJ console 开启）：boot 后延迟 15 秒再 dump
    // （USB-JTAG 重枚举 + host 端 serial capture 重连有时要 10 秒，
    //  给足窗口，dump 才不会被吞）
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    xTaskCreate([](void *) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        diag_log_dump();
        vTaskDelete(nullptr);
    }, "diag_dump", 3072, nullptr, 1, nullptr);
#endif

    xTaskCreate(hourly_task, "diag_hourly", 3072, nullptr, 1, nullptr);
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    xTaskCreate(debug_print_task, "diag_dbg", 3072, nullptr, 1, nullptr);
#endif
    ESP_LOGI(TAG, "诊断日志已启动 (head=%u, 容量=%d 条)", s_head, DIAG_RING_LEN);
}
