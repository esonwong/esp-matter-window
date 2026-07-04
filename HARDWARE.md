# 硬件设计

本项目是一款基于 Matter over Thread 的电动开窗器，支持 Apple Home 等 Matter 生态。

## 主控

- **模组**：Seeed XIAO ESP32-C6
- **MCU**：ESP32-C6（RISC-V，内置 802.15.4 Thread 射频）
- **连接方式**：Matter over Thread（不启用 Wi-Fi）
- **省电模式**：ICD LIT（Long Idle Time，电池供电）

## 引脚分配

| 功能 | GPIO | XIAO 标号 | 说明 |
|---|---|---|---|
| 电机 IN1 | GPIO0 | D0 | DRV8833 AIN1 |
| 电机 IN2 | GPIO1 | D1 | DRV8833 AIN2 |
| **电池电压 ADC** | **GPIO2** | **D2** | ADC1_CH2，100K+100K 分压到 BAT pad |
| 霍尔传感器 | GPIO23 | D5 | 低电平有效，内部上拉 |
| 板载 LED | GPIO15 | — | 开漏，状态指示 |
| BOOT 按键 | GPIO9 | — | 低电平有效，单/双/三击 |

## 电机驱动

- **驱动芯片**：DRV8833（单通道 H 桥）
- **控制方式**：纯 GPIO 电平（IN1/IN2 高低电平），全速运行，未启用 PWM 调速
- **三种状态**：
  - 正转：IN1=1, IN2=0
  - 反转：IN1=0, IN2=1
  - 停止：IN1=0, IN2=0（滑行停止；当前实现里 brake 也走滑行）

## 位置反馈：单霍尔 + 双磁铁

整机只有 **1 颗** 霍尔传感器（GPIO23 / D5），但窗框两端各贴一块磁铁：

- 窗运动到 **全开** 或 **全关** 端点时，同一传感器都会被触发（输出低电平）
- 因此端点检测**完全靠霍尔**，不依赖行程时间
- 中途百分比停靠（Matter `GoToLiftPercentage`）使用 `travel_ms`（标称约 30 s）做位置估算和上报，校准后写入 NVS

### LEAVING 状态防误触

从端点出发时，磁铁仍在传感器视野内，直接判 `hall=true` 会立刻把窗判为"已到端"。状态机引入两个过渡态：

- `WINDOW_LEAVING_OPEN`：从全开出发，电机已启动，等待磁铁离开 → 切到 `WINDOW_CLOSING`
- `WINDOW_LEAVING_CLOSED`：从全关出发，电机已启动，等待磁铁离开 → 切到 `WINDOW_OPENING`

只有进入正常 `OPENING/CLOSING` 后，霍尔再次拉低才视为到达对端。

## 人机交互

### LED 模式（`led_ctrl.h`）

| 模式 | 含义 |
|---|---|
| LED_OFF | 静止 |
| LED_ON | 校准模式（常亮） |
| LED_BLINK_FAST | 开窗中（200 ms 周期） |
| LED_BLINK_SLOW | 关窗中（800 ms 周期） |

### 按键功能（XIAO BOOT 按键，GPIO9）

| 操作 | 行为 |
|---|---|
| 单击 | 运动中暂停；静止时开 ↔ 关切换；校准中：停止并记录行程 |
| 双击 | 运动中反向；静止时与单击相反方向 |
| 三击 | 触发校准（先到全开端，再计时跑到全关，写入 NVS） |

按键由 `iot_button` 组件驱动，所有回调内部都先调用 `icd_wake()` 唤醒 ICD，再操作状态机。

## Matter 设备模型

- **Endpoint type**：`WindowCovering`，`type = 6 (kShutter)` —— Apple Home 显示为"窗户"而不是"窗帘"
- **Features**：`Lift` + `PositionAwareLift`
- **位置约定**：`0` = 全开，`10000` = 全关（百分之百分位 / Percent100ths，符合 Matter 规范）
- **关键属性**：
  - `CurrentPositionLiftPercent100ths`
  - `TargetPositionLiftPercent100ths`
  - `OperationalStatus`

## XIAO ESP32-C6 板载电源链（来源：官方原理图 v1.0）

原理图：[`docs/datasheets/XIAO_ESP32C6_v1.0_SCH.pdf`](docs/datasheets/XIAO_ESP32C6_v1.0_SCH.pdf)

> 原理图版权归 Seeed Studio，XIAO 系列以 OSHW 形式开源（[官方下载](https://files.seeedstudio.com/wiki/SeeedStudio-XIAO-ESP32C6/XIAO-ESP32-C6_v1.0_SCH_PDF_24028.pdf) / [OSHW-XIAO-Series 仓库，MIT](https://github.com/Seeed-Studio/OSHW-XIAO-Series)）。

```
                +5.6V Zener clamp (D3)
                       │
USB-C VBUS ───┬────────┴─────► Q1 (P-MOSFET LP0404N3T5G) ──┐
              │                                              │
              └──► U3 SGM40567-4.2 (Li-ion charger, 120mA) ──► BAT pad
                                                            │
                          BAT ──► D1 (Schottky LMBR4010, ──┤
                                      600mV@1A)             │
                                                            ▼
                                              +5V rail ──► U1 SGM6029C (buck) ──► +3V3
```

| 元件 | 型号 | 作用 | 关键参数 |
|---|---|---|---|
| 充电 IC | **SGM40567-4.2XG** | 单节锂电线性充电 | 充电电流 120 mA（IREF=200 kΩ）|
| 主稳压 | **SGM6029C** | **DC-DC Buck**，5V→3.3V | 开关式，效率 ~90% |
| VBUS OVP | **LNZ8F5V6T5G** | 5.6 V Zener 钳位 | **200 mW**（脆弱！）|
| 电源选择 | Q1 LP0404N3T5G + D1 LMBR4010BST5G | VBUS / BAT OR-ing | Schottky 压降 600 mV @ 1A |

### 与"USB 接太阳能板"直接相关的两个点

1. **VBUS 上有一颗 5.6 V / 200 mW 的 Zener (D3)**。你接 6 V 太阳能板（实际 Voc 可能 7 V+）会让这颗 Zener **持续导通**——按 200 mW 算，反向击穿后只能吃掉约 36 mA。如果光照下面板能输出更多电流，这颗 Zener 可能已经被烧损/降级，进入半导通状态，把太阳能能量大量浪费成热（甚至直接吃掉电池给的 4 V 也可能漏一些）。
2. **充电 IC 是线性的 SGM40567，最大 120 mA 充电**。即使 VBUS 5 V 正常，从 5 V 到 4.2 V 的电压差全部在 IC 内部烧成热（线性充电固有），效率约 70%。
3. **从电池→USB 方向是被阻断的**：Q1 关断 + 充电 IC 内部 reverse blocking + D1 是从 BAT 朝 +5V 单向。所以"电池通过 USB 反向漏电"不会发生（我之前的第一条假设错了）。

### 这意味着什么

- 6 V 太阳能板**绝对不能直接接 USB**——会持续烧 Zener D3，迟早把它烧坏，烧坏后 VBUS 保护就没了
- 必须在太阳能板和 USB 之间加 **5 V buck**（输出锁死 5 V，不超过 5.3 V 安全）
- 或者更规范：**绕过 USB**，用专用 MPPT 太阳能充电 IC（CN3791 等）直接接 BAT pad，完全不经过这条 VBUS 链路

## 当前供电方案（CN3791 + 6V 太阳能板）

太阳能板**绕过 USB**，通过 CN3791 MPPT 充电模块直接给电池充电；XIAO 也直接从电池/BAT pad 取电。USB 口仅用于烧录调试。

```
[6V 太阳能板]
   │ + ──┐
   │ − ──┼──► CN3791 SOLAR_IN
        │
[CN3791 模块（PH2.0 接口）]
        │
   BAT+ ┼──┬──► 锂电池 +
        │  └──► XIAO BAT pad (D5/D8 之间靠近 D8 那块焊盘)
   BAT− ┼──┬──► 锂电池 −
        │  └──► XIAO GND
```

关键点：
- CN3791 是 **buck 拓扑 + 内置 MPPT**，宽输入（4.5~28V），可换成 9V/12V 板而不用改电路
- R050 (50mΩ) 采样电阻对应充电电流上限 ~2A，小面板根本到不了，由面板自然限流
- **CN3791 内部已有反向保护**，夜间不会通过面板反向漏电，无需外加 SS14
- 电池接头：CN3791 模块用 **JST-PH 2.0**，购买锂电时认准 "PH 2.0" 字样
- 上电顺序：先把电池充满到 4.0V 以上再装入，避免 CN3791 在空电池上启动反复重启
- USB 口完全空着，不再触发板上 5.6V Zener (D3) 和 USJ 控制台耗电问题

## 电池电压 ADC（用于诊断）

为了在板上记录电池电压曲线，BAT pad 通过分压器接到 D2 (GPIO2 / ADC1_CH2)：

```
BAT pad ──[R1 100kΩ]──┬──► D2 (GPIO2, ADC1_CH2)
                       │
                      [R2 100kΩ]
                       │      ┌──[C1 0.1µF]──► GND（滤波）
                      GND ────┘
```

- 分压比 1:2，4.2V → 2.1V，落在 ADC 12dB 衰减量程内（0~3.1V）
- 持续耗电 = 4.2V / 200kΩ ≈ **21 µA**（相比 ICD LIT 平均功耗可忽略）
- 软件在 `diag_log.cpp` 里用 `adc_oneshot` + 曲线拟合校准，每条日志事件采样 8 次平均，乘 2 还原为 BAT 电压（mV）

## 板上诊断日志（NVS 环形缓冲）

日志写入 NVS namespace `"diag"`，128 条循环缓冲，每条 16 字节，总占 ~2KB。

**记录事件**（`enum diag_type_t`）：
| 类型 | 触发 | aux1 含义 |
|---|---|---|
| `DIAG_BOOT` (1) | 每次开机 | `esp_reset_reason()` |
| `DIAG_HOURLY` (2) | 每小时 | 0 |
| `DIAG_MOTOR_CMD` (3) | （预留）| 方向 |
| `DIAG_MOTOR_DONE` (4) | （预留）| 最终 state |
| `DIAG_BUTTON` (5) | 按键 | 1=单击, 2=双击, 3=三击 |
| `DIAG_STATE` (6) | window_state 变化 | 新 state |

**每条事件字段**：uptime_s, type, aux1, vbat_mv, position, state, motor_count, button_count, free_heap_kb

**Dump 流程**：
1. 临时把 `sdkconfig.defaults` 第 5 行 `CONFIG_ESP_CONSOLE_NONE=y` 改回 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`
2. `idf.py build flash monitor`
3. 设备 boot 时会自动 dump 一次，CSV 格式打印到串口（`=== DIAG LOG DUMP ===` 包围）
4. 复制日志到分析
5. 重新关掉 USJ console，烧回生产固件继续跑

调试固件不会清空日志（NVS 里），新事件会继续累积到环形缓冲。

## 固件配套优化（已应用到 `sdkconfig.defaults`）

电源链改好后，固件侧也关掉了几个会持续耗电的项：

| 配置 | 原值 | 新值 | 原因 |
|---|---|---|---|
| `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` | y | n（改 NONE）| USJ 外设/PHY 常开会让 ICD LIT 平均功耗升 mA 级 |
| `CONFIG_ESP_CONSOLE_NONE` | n | y | 生产固件不需要日志输出 |
| `CONFIG_OPENTHREAD_CONSOLE_TYPE_USB_SERIAL_JTAG` | y | n（改 UART） | 同上 |
| `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP` | not set | not set | **不要打开**：会让 iot_button 在 light sleep 后丢按键 |

调试时临时改回 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` 烧调试固件即可。

## ICD 配置摘要（`sdkconfig.defaults`）

```
CONFIG_ENABLE_ICD_SERVER=y
CONFIG_ENABLE_ICD_LIT=y
CONFIG_ICD_SLOW_POLL_INTERVAL_MS=5000
CONFIG_ICD_FAST_POLL_INTERVAL_MS=500
CONFIG_ICD_ACTIVE_MODE_INTERVAL_MS=2000
CONFIG_ICD_ACTIVE_MODE_THRESHOLD_MS=5000
CONFIG_ICD_IDLE_MODE_INTERVAL_SEC=600
CONFIG_SUPPORT_ICD_MANAGEMENT_CLUSTER=y
```

## 已知与硬件相关的注意点

- `motor_ctrl.h` 顶部注释写的 `D0=GPIO2, D1=GPIO3, 20kHz PWM`，与实际实现（GPIO0/GPIO1，纯电平）不一致 —— 以 `motor_ctrl.cpp` 为准
- 霍尔传感器低电平有效，上电时若磁铁恰好覆盖传感器，状态机会判为已在端点状态
- VBUS 上的 5.6 V Zener (D3) 只有 200 mW 容量，不要接超过 5.3 V 的电源到 USB 口
- **ESP32-C6 上只有 GPIO0~GPIO7 是 LP_GPIO，可作为 light sleep 唤醒源**。本项目用到的 BOOT 按键（GPIO9）和霍尔传感器（GPIO23）都不在 LP 域，所以不能开 `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP=y`——开了之后 `iot_button` 在 GPIO9 上初始化会直接失败（"GPIO 9 is not a valid wakeup source"），按键完全不响应。如果将来要进一步省电，需要把按键飞线到 D2/D3/D4（GPIO2/3/4 之类的 LP_GPIO）

## 已观察的功耗组成与下版硬件改进

### USB 充电（实测，2026-05-17 dump）
- SGM40567 4.2V 线性充电 IC，spec 120 mA
- 实测从 3882 mV → 4094 mV 用了 9 小时，充入 ~880 mAh
- **平均充电电流 ~98 mA**（CC 阶段接近 120 mA，CV 阶段逐渐降到终止 ~12 mA）
- 4094 mV 没有到 4.2V 终止可能因为 ADC 测的 BAT pad 比充电 IC 输出端有几十 mV 压降 / ADC 标定误差

### Idle 电流拆解（ICD LIT 模式，~16.5 mA 平均）
**测量方法**（2026-05-17 14:22 实测）：定型固件，太阳能断开，纯电池放电 4 小时。
HOURLY 快照 vbat：4080 → 4064 → 4058 → 4054 → 4050 mV（4 小时降 30 mV）。
按 LUT 插值 SoC 88% → 85% = 3%，3% × 2200 mAh / 4h = **16.5 mA**。
和 Apple Home 同步显示的 87% → 84% 一致。和"上次 5 天电池没电"的旧观察反算
2200/120h = 18 mA 也吻合，说明太阳能那次基本没补到电。

- **DRV8833 quiescent ~1.6 mA**（nSLEEP 硬连 VCC，从不进 sleep）—— 占 idle 10%
- **ESP32-C6 + Thread radio polling: ~14.5 mA**（**大头**，占 88%）
- 其它（霍尔上拉等）：< 0.5 mA

**纯电池理论续航**：2200 mAh / 16.5 mA = **137 小时 = 5.7 天**

### 下版硬件改进方向（按 ROI 排序）
真正的耗电大头是 ESP32-C6+Thread radio（14.5 mA），DRV8833 只占 10%。所以
省电优化重点应该在软件 / Matter 配置侧，硬件改动 ROI 不高。

1. **软件侧**：查 Thread polling 间隔（当前 `ICD_SLOW_POLL_INTERVAL_MS=5000`，
   5 秒一次轮询本身就 mA 级）、Matter subscription 流量、LightSleep 是否真的进了
2. **DRV8833 nSLEEP 接 GPIO**：运动结束就拉低，1.6 mA → 2 µA。绝对值省 1.6 mA
   把续航从 5.7 天延长到 ~6.3 天（+11%）
3. **VM 加 P-MOSFET 全切电机驱动板供电**：比 nSLEEP 多省零点几 mA，但成本高、
   PCB 改动大，不划算
4. **按键飞线到 LP_GPIO (GPIO2/3/4)**：可以开 `PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP`，
   进一步压低 ESP32-C6 idle 功耗（潜力大，需要硬件改）
