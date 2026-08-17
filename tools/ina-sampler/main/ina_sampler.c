// ── INA226 / INA3221 电流采样器 ───────────────────────────────────────────
//
// 跑在 DFRobot FireBeetle 2 ESP32-C6 上，给 esp-matter-window 主控做功耗测量。
// 串口输出 CSV，配 tools/ina_log.py 记录到文件。
//
// 为什么不用 INA226 的 CAL/Current 寄存器：直接读分流电压寄存器换算，
// 换分流电阻只要改 SHUNT_MILLIOHM，不用重新编程校准值，也少一个出错点。
//
// 接线见 docs/current-measurement.md。
//
// 输出格式（每秒一行）：
//   INA226:  t_ms,n,mean_uA,min_uA,max_uA,bus_mV,pct_below
//   INA3221: t_ms,n,ch1_mean_uA,ch1_bus_mV,ch2_mean_uA,ch2_bus_mV,ch3_mean_uA,ch3_bus_mV

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "INA";

// ── 硬件配置 ──────────────────────────────────────────────────────────────
// FireBeetle 2 ESP32-C6 丝印：19/SDA、20/SCL
#define I2C_SDA_GPIO        19
#define I2C_SCL_GPIO        20
#define I2C_FREQ_HZ         400000
#define INA_ADDR            0x40   // A0/A1 都接 GND 时的默认地址

// 模块板载分流电阻：INA226 蓝板和 INA3221 黑板都是 R100 = 0.1 Ω = 100 mΩ
#define SHUNT_MILLIOHM      100

// "进入睡眠了吗" 判据：低于此电流的采样点占比
#define SLEEP_THRESH_UA     5000   // 5 mA

// ── 寄存器 ────────────────────────────────────────────────────────────────
#define REG_CONFIG          0x00
#define REG_MFG_ID          0xFE
#define REG_DIE_ID          0xFF

#define INA226_REG_SHUNT    0x01
#define INA226_REG_BUS      0x02
#define INA226_DIE_ID       0x2260

#define INA3221_DIE_ID      0x3220
// INA3221 通道 n(1..3)：分流 = 0x01 + (n-1)*2，总线 = 0x02 + (n-1)*2

static i2c_master_dev_handle_t s_dev;

static esp_err_t reg_read(uint8_t reg, uint16_t *out)
{
    uint8_t rx[2];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, rx, 2, 100);
    if (err == ESP_OK) *out = ((uint16_t)rx[0] << 8) | rx[1];
    return err;
}

static esp_err_t reg_write(uint8_t reg, uint16_t val)
{
    uint8_t tx[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    return i2c_master_transmit(s_dev, tx, sizeof(tx), 100);
}

// ── 换算 ──────────────────────────────────────────────────────────────────
// INA226 分流电压 LSB = 2.5 µV；I(µA) = V_shunt(µV) / R(Ω) = raw * 2.5 * 1000 / R(mΩ)
static inline int32_t ina226_shunt_to_ua(int16_t raw)
{
    return (int32_t)((int64_t)raw * 2500 / SHUNT_MILLIOHM);
}
// INA3221 分流电压 13 位左对齐，LSB = 40 µV
static inline int32_t ina3221_shunt_to_ua(int16_t raw)
{
    return (int32_t)((int64_t)(raw >> 3) * 40000 / SHUNT_MILLIOHM);
}

// ── 采样循环 ──────────────────────────────────────────────────────────────
static void run_ina226(void)
{
    // AVG=1, VBUSCT=332µs, VSHCT=332µs, MODE=分流+总线连续
    // 单次转换 ~664 µs，比 AVG=16 的 35 ms 快得多——要看清 light sleep 的
    // 短唤醒脉冲就不能让芯片替我们平均掉。
    ESP_ERROR_CHECK(reg_write(REG_CONFIG, 0x4000 | (0 << 9) | (2 << 6) | (2 << 3) | 7));

    printf("# INA226  shunt=%d mOhm  sleep_thresh=%d uA\n", SHUNT_MILLIOHM, SLEEP_THRESH_UA);
    printf("t_ms,n,mean_uA,min_uA,max_uA,bus_mV,pct_below\n");

    while (1) {
        int64_t t_end = esp_timer_get_time() + 1000000;  // 1 秒窗口
        int64_t sum = 0;
        int32_t mn = INT32_MAX, mx = INT32_MIN;
        uint32_t n = 0, below = 0;
        uint16_t bus_raw = 0;

        while (esp_timer_get_time() < t_end) {
            uint16_t raw;
            if (reg_read(INA226_REG_SHUNT, &raw) != ESP_OK) continue;
            int32_t ua = ina226_shunt_to_ua((int16_t)raw);
            sum += ua; n++;
            if (ua < mn) mn = ua;
            if (ua > mx) mx = ua;
            if (ua < SLEEP_THRESH_UA) below++;
        }
        reg_read(INA226_REG_BUS, &bus_raw);

        if (n == 0) { ESP_LOGW(TAG, "无有效采样"); vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        printf("%" PRId64 ",%" PRIu32 ",%" PRId32 ",%" PRId32 ",%" PRId32 ",%" PRIu32 ",%" PRIu32 "\n",
               esp_timer_get_time() / 1000, n, (int32_t)(sum / n), mn, mx,
               (uint32_t)(bus_raw * 125 / 100),   // 总线电压 LSB = 1.25 mV
               below * 100 / n);
        fflush(stdout);
    }
}

static void run_ina3221(void)
{
    // 三通道使能，AVG=1，转换时间 332 µs，分流+总线连续
    ESP_ERROR_CHECK(reg_write(REG_CONFIG, 0x7000 | (0 << 9) | (2 << 6) | (2 << 3) | 7));

    printf("# INA3221  shunt=%d mOhm  ch1/ch2/ch3\n", SHUNT_MILLIOHM);
    printf("t_ms,n,ch1_mean_uA,ch1_bus_mV,ch2_mean_uA,ch2_bus_mV,ch3_mean_uA,ch3_bus_mV\n");

    while (1) {
        int64_t t_end = esp_timer_get_time() + 1000000;
        int64_t sum[3] = {0};
        uint32_t n = 0;
        uint16_t bus[3] = {0};

        while (esp_timer_get_time() < t_end) {
            uint16_t raw;
            bool ok = true;
            int32_t v[3];
            for (int c = 0; c < 3; c++) {
                if (reg_read(0x01 + c * 2, &raw) != ESP_OK) { ok = false; break; }
                v[c] = ina3221_shunt_to_ua((int16_t)raw);
            }
            if (!ok) continue;
            for (int c = 0; c < 3; c++) sum[c] += v[c];
            n++;
        }
        for (int c = 0; c < 3; c++) reg_read(0x02 + c * 2, &bus[c]);

        if (n == 0) { ESP_LOGW(TAG, "无有效采样"); vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        printf("%" PRId64 ",%" PRIu32, esp_timer_get_time() / 1000, n);
        for (int c = 0; c < 3; c++)
            printf(",%" PRId32 ",%" PRId32,
                   (int32_t)(sum[c] / n), (int32_t)((int16_t)bus[c] >> 3) * 8);  // 总线 LSB = 8 mV
        printf("\n");
        fflush(stdout);
    }
}

void app_main(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = INA_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev));

    uint16_t mfg = 0, die = 0;
    if (reg_read(REG_MFG_ID, &mfg) != ESP_OK) {
        ESP_LOGE(TAG, "0x%02X 无应答——检查 SDA=GPIO%d / SCL=GPIO%d / VCC / GND 焊点",
                 INA_ADDR, I2C_SDA_GPIO, I2C_SCL_GPIO);
        return;
    }
    reg_read(REG_DIE_ID, &die);
    ESP_LOGI(TAG, "mfg=0x%04X die=0x%04X", mfg, die);

    if (die == INA226_DIE_ID)       run_ina226();
    else if (die == INA3221_DIE_ID) run_ina3221();
    else ESP_LOGE(TAG, "未知芯片 die=0x%04X（期望 0x2260 INA226 或 0x3220 INA3221）", die);
}
