#include "matter_app.h"
#include "window_ctrl.h"
#include "diag_log.h"

#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>

// 从 main_app.cpp 导出
extern esp_reset_reason_t g_reset_reason_at_boot;
extern "C" const char *main_reset_reason_str(int r);
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include <esp_matter.h>
#include <esp_matter_endpoint.h>
#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <platform/ESP32/OpenthreadLauncher.h>
#include <esp_openthread_types.h>

// ESP32-C6 原生 802.15.4 无线电配置
#define OT_RADIO_CONFIG()  { .radio_mode = RADIO_MODE_NATIVE }
#define OT_HOST_CONFIG()   { .host_connection_mode = HOST_CONNECTION_MODE_NONE }
#define OT_PORT_CONFIG()   { .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10 }
// Matter WindowCovering cluster IDs
#include <app-common/zap-generated/cluster-enums.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/clusters/window-covering-server/window-covering-server.h>

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static const char *TAG = "MATTER";

static uint16_t s_endpoint_id = 1;
static uint16_t s_ps_endpoint_id = 0;  // Power Source endpoint
// 上次向 Matter 上报的 CurrentPosition，用于识别 StopMotion 命令
static uint16_t s_last_reported_pos = 10000;

// ── 电池电压 → 百分比 映射（单节 Li-ion 经验曲线）─────────────────────────
// 返回 0~200（0.5% 单位，符合 Matter BatPercentRemaining 规范）
static uint8_t vbat_mv_to_percent_x2(int16_t mv)
{
    if (mv <= 0) return 0;
    struct { int16_t mv; uint8_t pct; } table[] = {
        {4200, 100}, {4100, 90}, {4000, 80}, {3900, 65}, {3800, 50},
        {3700, 30}, {3600, 15}, {3500, 5},  {3300, 0},
    };
    if (mv >= table[0].mv) return 200;
    if (mv <= table[8].mv) return 0;
    for (int i = 0; i < 8; i++) {
        if (mv <= table[i].mv && mv >= table[i+1].mv) {
            int span_mv = table[i].mv - table[i+1].mv;
            int span_pct = table[i].pct - table[i+1].pct;
            int pct = table[i+1].pct + (mv - table[i+1].mv) * span_pct / span_mv;
            return (uint8_t)(pct * 2);  // ×2 for 0.5% units
        }
    }
    return 0;
}

// ── 电池属性更新（每 10 分钟一次）───────────────────────────────────────
static void battery_update_task(void *)
{
    using namespace chip::app::Clusters;
    // 启动后等 10 秒，等 Matter stack 完全就绪
    vTaskDelay(pdMS_TO_TICKS(10000));
    for (;;) {
        int16_t mv = diag_get_vbat_mv();
        if (mv > 0 && s_ps_endpoint_id != 0) {
            uint8_t pct_x2 = vbat_mv_to_percent_x2(mv);
            uint8_t charge_level = (mv >= 3700) ? 0 : (mv >= 3500 ? 1 : 2);  // OK/Warn/Crit

            esp_matter_attr_val_t v_volt = esp_matter_nullable_uint32((uint32_t)mv);
            attribute::update(s_ps_endpoint_id, PowerSource::Id,
                PowerSource::Attributes::BatVoltage::Id, &v_volt);

            esp_matter_attr_val_t v_pct = esp_matter_nullable_uint8(pct_x2);
            attribute::update(s_ps_endpoint_id, PowerSource::Id,
                PowerSource::Attributes::BatPercentRemaining::Id, &v_pct);

            esp_matter_attr_val_t v_lvl = esp_matter_enum8(charge_level);
            attribute::update(s_ps_endpoint_id, PowerSource::Id,
                PowerSource::Attributes::BatChargeLevel::Id, &v_lvl);

            ESP_LOGI(TAG, "[BATT] vbat=%d mV  pct=%u%%  level=%u", mv, pct_x2/2, charge_level);
        }
        vTaskDelay(pdMS_TO_TICKS(10 * 60 * 1000));
    }
}

// ── 属性更新回调 ──────────────────────────────────────────────────────────
// Matter 控制器写属性或发命令时触发（PRE_UPDATE = 执行前）
static esp_err_t app_attribute_update_cb(
    attribute::callback_type_t type,
    uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
    esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != PRE_UPDATE) return ESP_OK;

    // 仅处理 WindowCovering 集群的目标位置属性
    if (cluster_id != WindowCovering::Id) return ESP_OK;
    if (attribute_id != WindowCovering::Attributes::TargetPositionLiftPercent100ths::Id)
        return ESP_OK;

    // Matter 规范：0 = 全开，10000 = 全关
    uint16_t target = val->val.u16;
    window_state_t cur_state = window_ctrl_get_state();
    bool is_moving = (cur_state == WINDOW_OPENING || cur_state == WINDOW_CLOSING ||
                      cur_state == WINDOW_LEAVING_OPEN || cur_state == WINDOW_LEAVING_CLOSED);

    ESP_LOGI(TAG, "目标位置 %d (上报位置 %d, 运动中 %d)", target, s_last_reported_pos, is_moving);

    // StopMotion 命令：SDK 将 Target 设为上次上报的 Current
    if (is_moving && target == s_last_reported_pos) {
        ESP_LOGI(TAG, "StopMotion");
        window_ctrl_stop();
        return ESP_OK;
    }

    uint16_t current = window_ctrl_get_position();
    if (target == current) return ESP_OK;
    window_ctrl_move_to(target);
    return ESP_OK;
}

// ── 设备事件回调 ──────────────────────────────────────────────────────────
static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "[配网] 配对窗口已打开，BLE 广播中...");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "[配网] 配对完成");
        break;
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "[网络] IPv6 地址已分配");
        break;
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved: {
        ESP_LOGI(TAG, "[配网] Fabric 已移除，重新开启配对窗口");
        chip::Server::GetInstance().GetFabricTable().DeleteAllFabrics();
        auto &commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
        commissionMgr.OpenBasicCommissioningWindow();
        break;
    }
    default:
        break;
    }
}

// ── 更新 Matter 属性（由 window_ctrl 回调触发） ───────────────────────────
void matter_app_update_position(uint16_t pos_100ths, int state)
{
    s_last_reported_pos = pos_100ths;

    // CurrentPositionLiftPercent100ths（nullable uint16）
    esp_matter_attr_val_t pos_val = esp_matter_nullable_uint16(pos_100ths);
    attribute::update(s_endpoint_id, WindowCovering::Id,
        WindowCovering::Attributes::CurrentPositionLiftPercent100ths::Id, &pos_val);

    // OperationalStatus：bit0-1 = 全局方向（0停，1开，2关）
    uint8_t op_status = 0;
    if (state == WINDOW_OPENING || state == WINDOW_LEAVING_CLOSED) op_status = 0x01;
    if (state == WINDOW_CLOSING || state == WINDOW_LEAVING_OPEN)   op_status = 0x02;
    // bit2-3 = lift，同方向
    op_status |= (op_status << 2);
    esp_matter_attr_val_t op_val = esp_matter_uint8(op_status);
    attribute::update(s_endpoint_id, WindowCovering::Id,
        WindowCovering::Attributes::OperationalStatus::Id, &op_val);

    // 停止时（到位或中途停）将 Target 同步为实际位置，否则控制器状态异常
    if (state == WINDOW_OPEN || state == WINDOW_CLOSED || state == WINDOW_STOPPED) {
        attribute::update(s_endpoint_id, WindowCovering::Id,
            WindowCovering::Attributes::TargetPositionLiftPercent100ths::Id, &pos_val);
    }
}

// ── 初始化 ────────────────────────────────────────────────────────────────
void matter_app_init(void)
{
    // 创建 Matter 节点
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, nullptr);
    if (!node) {
        ESP_LOGE(TAG, "节点创建失败");
        return;
    }

    // 创建 WindowCovering 端点（Shutter 类型，Apple Home 识别为"窗户"而非"窗帘"）
    window_covering::config_t wc_config;
    wc_config.window_covering.type = 6; // Type::kShutter
    // 必须启用 Lift + PositionAwareLift feature，否则集群创建会 abort
    wc_config.window_covering.feature_flags =
        cluster::window_covering::feature::lift::get_id() |
        cluster::window_covering::feature::position_aware_lift::get_id();
    // 初始位置：全关（10000 = 100.00%）
    uint16_t init_pos = window_ctrl_get_position();
    wc_config.window_covering.features.position_aware_lift.current_position_lift_percent_100ths =
        nullable<uint16_t>(init_pos);
    wc_config.window_covering.features.position_aware_lift.target_position_lift_percent_100ths =
        nullable<uint16_t>(init_pos);
    endpoint_t *ep = window_covering::create(node, &wc_config, ENDPOINT_FLAG_NONE, nullptr);
    if (!ep) {
        ESP_LOGE(TAG, "WindowCovering 端点创建失败");
        return;
    }
    s_endpoint_id = endpoint::get_id(ep);

    // Power Source endpoint（独立 endpoint，battery feature）
    power_source::config_t ps_config;
    ps_config.power_source.feature_flags = cluster::power_source::feature::battery::get_id();
    endpoint_t *ps_ep = power_source::create(node, &ps_config, ENDPOINT_FLAG_NONE, nullptr);
    if (!ps_ep) {
        ESP_LOGE(TAG, "Power Source 端点创建失败");
    } else {
        s_ps_endpoint_id = endpoint::get_id(ps_ep);
        // 默认 cluster 只建必填属性（BatChargeLevel），BatVoltage 和
        // BatPercentRemaining 是可选属性，要显式建出来才能 update
        cluster_t *ps_cluster = cluster::get(ps_ep, PowerSource::Id);
        cluster::power_source::attribute::create_bat_voltage(ps_cluster, 0, 0x00, 0xFFFFFFFF);
        cluster::power_source::attribute::create_bat_percent_remaining(ps_cluster, 0, 0, 200);
        ESP_LOGI(TAG, "Power Source 端点已创建，id=%d", s_ps_endpoint_id);
    }

    // 配置 OpenThread 平台（必须在 start() 前调用）
    esp_openthread_platform_config_t ot_config = {
        .radio_config = OT_RADIO_CONFIG(),
        .host_config  = OT_HOST_CONFIG(),
        .port_config  = OT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&ot_config);

    // 启动 Matter 协议栈（初始位置已通过 wc_config 设置，无需再次 update）
    esp_matter::start(app_event_cb);

    // 打印配对码（首次使用时扫码）
    PrintOnboardingCodes(chip::RendezvousInformationFlags(
        chip::RendezvousInformationFlag::kBLE));
    ESP_LOGI(TAG, "Matter 启动，等待配对...");

    // 启动电池属性更新任务（5 分钟周期）
    if (s_ps_endpoint_id != 0) {
        xTaskCreate(battery_update_task, "batt_upd", 4096, nullptr, 1, nullptr);
    }

    // 打印 reset reason（app_main 阶段 USB-JTAG console 还没起来会被吞）
    ESP_LOGI(TAG, "[RESET] reason=%d (%s)",
        (int)g_reset_reason_at_boot, main_reset_reason_str((int)g_reset_reason_at_boot));
}
