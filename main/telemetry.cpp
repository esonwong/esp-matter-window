#include "telemetry.h"
#include "telemetry_secrets.h"
#include "diag_log.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_openthread_dns64.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <cstdio>
#include <cstring>

static const char *TAG = "TELEM";

struct resp_buf_t { char *buf; size_t cap; size_t len; };

#ifndef TELEMETRY_INTERVAL_SEC
#define TELEMETRY_INTERVAL_SEC (60 * 60) // 每小时一报
#endif
#define TELEMETRY_FIRST_DELAY_SEC 90     // 开机先等 Thread attach 稳定
#define TELEMETRY_BATCH_MAX 64           // 单次最多带的 diag 条目
#define NVS_NS "telem"
#define NVS_KEY_SENT "sent_seq"

static uint32_t s_boot_id = 0;
static char s_device_id[13] = {};
static TaskHandle_t s_task = nullptr;

static uint32_t load_sent_seq(void)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_KEY_SENT, &v);
        nvs_close(h);
    }
    return v;
}

static void save_sent_seq(uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, NVS_KEY_SENT, v);
    nvs_commit(h);
    nvs_close(h);
}

// 响应里的 ota url（够简单就手撕，不引 JSON 库）
static bool parse_ota(const char *resp, char *url, size_t url_len, char *version, size_t ver_len)
{
    const char *o = strstr(resp, "\"ota\":");
    if (!o || strncmp(o + 6, "null", 4) == 0) return false;
    const char *u = strstr(o, "\"url\":\"");
    const char *v = strstr(o, "\"version\":\"");
    if (!u || !v) return false;
    u += 7;
    const char *ue = strchr(u, '"');
    v += 11;
    const char *ve = strchr(v, '"');
    if (!ue || !ve || (size_t)(ue - u) >= url_len || (size_t)(ve - v) >= ver_len) return false;
    memcpy(url, u, ue - u); url[ue - u] = 0;
    memcpy(version, v, ve - v); version[ve - v] = 0;
    return true;
}

static void do_ota(const char *url, const char *version)
{
    ESP_LOGW(TAG, "OTA 开始：%s → %s", esp_app_get_description()->version, version);
    esp_http_client_config_t http = {};
    http.url = url;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.timeout_ms = 30000;
    http.buffer_size = 2048;
    http.keep_alive_enable = true;
    char auth[96];
    snprintf(auth, sizeof(auth), "Bearer %s", TELEMETRY_TOKEN);

    esp_https_ota_config_t ota = {};
    ota.http_config = &http;
    ota.http_client_init_cb = [](esp_http_client_handle_t c) {
        static char s_auth[96];
        snprintf(s_auth, sizeof(s_auth), "Bearer %s", TELEMETRY_TOKEN);
        return esp_http_client_set_header(c, "Authorization", s_auth);
    };
    (void)auth;

    esp_err_t err = esp_https_ota(&ota);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "OTA 完成，重启");
        diag_log_event(DIAG_BOOT, 0); // 落盘一次，别丢 pending 数据
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    ESP_LOGE(TAG, "OTA 失败：%s（下轮再试）", esp_err_to_name(err));
}

static void report_once(void)
{
    // 1. 收集增量
    static diag_event_t events[TELEMETRY_BATCH_MAX];
    uint32_t next_seq = 0;
    uint32_t sent = load_sent_seq();
    uint32_t n = diag_get_events_since(sent, events, TELEMETRY_BATCH_MAX, &next_seq);

    // 2. 组 JSON
    static char body[4096];
    int off = snprintf(body, sizeof(body),
        "{\"device_id\":\"%s\",\"fw\":\"%s\",\"boot_id\":%lu,\"uptime_s\":%lu,"
        "\"vbat_mv\":%d,\"heap_kb\":%lu,\"entries\":[",
        s_device_id, esp_app_get_description()->version, (unsigned long)s_boot_id,
        (unsigned long)(esp_timer_get_time() / 1000000),
        diag_get_vbat_mv(), (unsigned long)(esp_get_free_heap_size() / 1024));
    for (uint32_t i = 0; i < n && off < (int)sizeof(body) - 96; i++) {
        const diag_event_t &e = events[i];
        off += snprintf(body + off, sizeof(body) - off, "%s[%lu,%u,%u,%d,%u,%u,%u,%u,%u]",
            i ? "," : "", (unsigned long)e.uptime_s, e.type, e.aux1, e.vbat_mv,
            e.position, e.state, e.motor_count, e.button_count, e.free_heap_kb);
    }
    off += snprintf(body + off, sizeof(body) - off, "]}");

    // 3. POST（perform 的响应体走 HTTP_EVENT_ON_DATA，必须用事件回调接）
    static char resp[512];
    resp_buf_t rb = { resp, sizeof(resp) - 1, 0 };
    esp_http_client_config_t cfg = {};
    cfg.url = TELEMETRY_URL "/ingest";
    cfg.method = HTTP_METHOD_POST;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 15000;
    cfg.user_data = &rb;
    cfg.event_handler = [](esp_http_client_event_t *evt) -> esp_err_t {
        if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
            resp_buf_t *r = (resp_buf_t *)evt->user_data;
            size_t take = evt->data_len;
            if (r->len + take > r->cap) take = r->cap - r->len;
            memcpy(r->buf + r->len, evt->data, take);
            r->len += take;
        }
        return ESP_OK;
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return;
    char auth[96];
    snprintf(auth, sizeof(auth), "Bearer %s", TELEMETRY_TOKEN);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, off);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    resp[rb.len] = 0;
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "上报失败 err=%s status=%d（%lu 条待重试）",
                 esp_err_to_name(err), status, (unsigned long)n);
        return;
    }

    save_sent_seq(next_seq);
    ESP_LOGI(TAG, "上报成功：%lu 条 diag，seq→%lu", (unsigned long)n, (unsigned long)next_seq);

    // 4. OTA 检查
    char url[192], version[32];
    if (parse_ota(resp, url, sizeof(url), version, sizeof(version)))
        do_ota(url, version);
}

static void telemetry_task(void *)
{
    // DNS64/NAT64：让 getaddrinfo 透明走 Thread 边界路由器
    esp_err_t err = esp_openthread_dns64_client_init();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "dns64 init: %s（家里若有原生 IPv6 仍可工作）", esp_err_to_name(err));

    vTaskDelay(pdMS_TO_TICKS(TELEMETRY_FIRST_DELAY_SEC * 1000));
    for (;;) {
        report_once();
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TELEMETRY_INTERVAL_SEC * 1000));
        (void)notified; // 被 telemetry_report_now 提前唤醒也只是提早跑一轮
    }
}

void telemetry_report_now(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

void telemetry_start(void)
{
    s_boot_id = esp_random();
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_IEEE802154);
    snprintf(s_device_id, sizeof(s_device_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    xTaskCreate(telemetry_task, "telem", 6144, nullptr, 3, &s_task);
    ESP_LOGI(TAG, "遥测已启动 device_id=%s 周期=%d s", s_device_id, TELEMETRY_INTERVAL_SEC);
}
