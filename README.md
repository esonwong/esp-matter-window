# matter-window

基于 **XIAO ESP32-C6 + DRV8833 + N20 减速电机** 的 Matter 开窗器。

接入苹果家庭、Google Home、Amazon Alexa 等支持 Matter 的平台后，可用语音或 App 控制窗帘开关，也可通过板载按键本地操作。

通信层使用 **Thread（IEEE 802.15.4）**，比 Wi-Fi 功耗更低（~1–5 mA vs ~15–25 mA），适合电池供电场景。需要家中有 Thread Border Router（Apple HomePod mini、Apple TV 4K 等）。

## 硬件

| 模块 | 型号 |
|------|------|
| 主控 | Seeed XIAO ESP32-C6 |
| 电机驱动 | DRV8833（J1 跳线需短接以启用 nSLEEP） |
| 电机 | N20 减速直流电机 |

### 接线

| XIAO 引脚 | GPIO | DRV8833 |
|-----------|------|---------|
| D0 | GPIO0 | AIN1 |
| D1 | GPIO1 | AIN2 |

> **注意**：ESP32-C6 的 GPIO0–7 属于 LP GPIO，无法使用 LEDC PWM 外设驱动，需直接操作 GPIO 电平（驱动已按此方式实现）。

板载资源：

- **LED**：GPIO15（开窗快闪 200ms、关窗慢闪 800ms，校准时常亮，到位或停止后熄灭）
- **BOOT 按键**：GPIO9（低电平有效，多击操作见下表）
- **霍尔传感器**：GPIO23/D5（A3144，低电平有效，内部上拉；**两端各放一块磁铁**，同一传感器检测全开和全关）

### 按键操作

| 击数 | 功能 |
|------|------|
| 1 击 | 运动中：暂停；静止时：开→关 / 关→开 / 停→**开** |
| 2 击 | 运动中：反向（开中→改为关，关中→改为开）；静止时：开→关 / 关→开 / 停→**关** |
| 3 击 | 校准行程：先到全开端（霍尔定位），再计时跑到全关（霍尔触发停止），实测时长保存到 NVS |

> 击数在最后一击后 500 ms 内无新击则触发。校准期间单击可提前中止。

## 软件架构

```
app_main
├── window_ctrl_init()   # 电机 + LED + 按键，FreeRTOS 任务，100 ms tick
└── matter_app_init()    # Matter 协议栈，WindowCovering 设备类型
```

**端点检测**：全开和全关均靠霍尔传感器触发，不依赖行程时间。两端各放一块磁铁，窗到达端点时同一传感器触发（低电平有效）。

**行程参数**：默认 3 分钟，3 击校准后使用实测值（保存到 NVS，上限 5 分钟）。`travel_ms` 仅用于中途百分比停靠的位置估算和 Matter 百分比上报，不影响端点判断。

**校准流程**：独立 FreeRTOS 任务（`calibrate_task`）运行，LED 常亮指示。先驱动电机到全开端（等待霍尔触发），停稳后向全关方向运动，记录从开端磁铁离开到关端磁铁触发的时长，保存到 NVS。

**位置持久化**：停止或到位时将当前位置写入 NVS，重启后自动恢复，Matter 侧显示正确百分比。

**Matter 集群**：WindowCovering，启用 Lift + PositionAwareLift feature，位置约定 0 = 全开、10000 = 全关（Matter 规范）。支持 GoToLiftPercentage 和 StopMotion 命令。

## 状态机

### 状态定义

| 状态 | pos | 电机 | 霍尔传感器 | LED | Matter OperationalStatus |
|------|-----|------|-----------|-----|--------------------------|
| `WINDOW_OPEN` | 0 | coast | **有效**（开端磁铁在范围内） | 灭 | 0x00 停止 |
| `WINDOW_LEAVING_OPEN` | 0 | 关方向 | **有效→无效**（等待开端磁铁离开） | 慢闪 | 0x0A 关闭中 |
| `WINDOW_CLOSING` | 递增中 | 关方向 | 无效 → 关端触发时有效 | 慢闪 800 ms | 0x0A 关闭中 |
| `WINDOW_CLOSED` | 10000 | coast | **有效**（关端磁铁在范围内） | 灭 | 0x00 停止 |
| `WINDOW_LEAVING_CLOSED` | 10000 | 开方向 | **有效→无效**（等待关端磁铁离开） | 快闪 | 0x05 开启中 |
| `WINDOW_OPENING` | 递减中 | 开方向 | 无效 → 开端触发时有效 | 快闪 200 ms | 0x05 开启中 |
| `WINDOW_STOPPED` | 1–9999 | coast | 无效 | 灭 | 0x00 停止 |

> pos 遵循 Matter 规范：0 = 全开，10000 = 全关（100.00%）。
> 霍尔传感器固定在设备上，两端各放一块磁铁，窗运动到任一端点时均触发同一 GPIO。

### 状态转换

```
WINDOW_OPEN ──close 命令──► WINDOW_LEAVING_OPEN
                                    │
                              hall=false（开端磁铁离开）
                                    │
                                    ▼
                             WINDOW_CLOSING
                                    │
                       ┌────────────┤
                 stop 命令    hall=true（关端触发）/ pos+step≥target
                       │            │
                       ▼            ▼ (target=10000)
                WINDOW_STOPPED  WINDOW_CLOSED ──open 命令──► WINDOW_LEAVING_CLOSED
                       │                                              │
                 open 命令                                  hall=false（关端磁铁离开）
                       │                                              │
                       └──────────────────────────────────────────────┘
                                                                      │
                                                                      ▼
                                                               WINDOW_OPENING
                                                                      │
                                                           ┌──────────┤
                                                     stop 命令    hall=true（开端触发）/ pos≤target
                                                           │            │
                                                           ▼            ▼ (target=0)
                                                    WINDOW_STOPPED  WINDOW_OPEN

WINDOW_LEAVING_OPEN / WINDOW_LEAVING_CLOSED
             └─ stop 命令 ────────────► WINDOW_STOPPED

WINDOW_OPENING
             ├─ hall 触发（开端） ────► WINDOW_OPEN (pos=0)
             ├─ pos≤target（中途）───► WINDOW_STOPPED
             └─ stop 命令 ──────────► WINDOW_STOPPED

WINDOW_CLOSING
             ├─ hall 触发（关端） ────► WINDOW_CLOSED (pos=10000)
             ├─ pos+step≥target（中途）► WINDOW_STOPPED
             └─ stop 命令 ──────────► WINDOW_STOPPED
```

### Entry Actions（set_state 执行）

| 进入状态 | 立即执行 |
|---------|---------|
| `WINDOW_OPENING` / `WINDOW_LEAVING_CLOSED` | `motor_open()` → LED 快闪 → 唤醒 window_task |
| `WINDOW_CLOSING` / `WINDOW_LEAVING_OPEN` | `motor_close()` → LED 慢闪 → 唤醒 window_task |
| `WINDOW_OPEN / CLOSED / STOPPED` | `motor_coast()` → LED 灭 → 保存 pos 到 NVS |

电机在状态转换时立即启动，window_task 只做监控（读霍尔、更新 pos），不发电机指令。这保证了从端点状态出发时，电机已在运转，磁铁已开始离开传感器，不会在 LEAVING 状态误触发霍尔。

### 位置数据流

```
window_task（100 ms tick）
  └─ 每 tick 更新 s_pos（±step）          # 仅用于百分比估算，不用于端点判断
  └─ 每 10 tick（1 s）通过回调上报 Matter
  └─ 到位时立即上报并写 NVS

step = 10000 / (travel_ms / 100)   # 动态计算，随校准结果自动调整
```

## 依赖

- **ESP-IDF v5.5.2**（固定 tag `v5.5.2`，本项目在此版本验证）
- **[esp-matter](https://github.com/espressif/esp-matter)**：main 分支无 release tag 且 API 变动频繁，建议固定到已验证的 commit `a13535b57aa7623fa6db23b540077d69d91e53f3`（connectedhomeip 子模块 `faf4d09ad13fc0c01be988c54ed819ff838567ee`）
- Thread Border Router：Apple HomePod mini / Apple TV 4K（Thread 支持版）

## 环境准备

以下以 `$HOME/esp` 作为 SDK 安装目录（惯例示例，可自选位置；路径不同则通过 `IDF_PATH` / `ESP_MATTER_PATH` 环境变量告知本项目的 `env.sh`）。

### 1. 安装 ESP-IDF v5.5.2

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c6
source export.sh   # 后续安装 esp-matter 前必须已 source
```

### 2. 安装 esp-matter

全递归克隆有数十 GB，推荐官方浅克隆方案（macOS 把 `--platform` 后的 `darwin` 保留，Linux 换成 `linux`）：

```bash
cd ~/esp
git clone --depth 1 https://github.com/espressif/esp-matter.git
cd esp-matter
# 可选但推荐：固定到已验证的 commit
git fetch --depth 1 origin a13535b57aa7623fa6db23b540077d69d91e53f3
git checkout a13535b57aa7623fa6db23b540077d69d91e53f3
git submodule update --init --depth 1
cd connectedhomeip/connectedhomeip
./scripts/checkout_submodules.py --platform esp32 darwin --shallow
cd ../..
./install.sh   # 不需要 chip-tool 可加 --no-host-tool
```

### 3. 每次开发前激活环境

顺序固定：先 ESP-IDF、后 esp-matter。项目提供的 `env.sh` 已封装：

```bash
source ./env.sh
# 等价于：
#   source $IDF_PATH/export.sh          # 默认 $HOME/esp/esp-idf
#   export ESP_MATTER_PATH=<esp-matter 路径>   # 默认 $HOME/esp/esp-matter
#   source $ESP_MATTER_PATH/export.sh
```

SDK 装在其他位置时先导出变量再 source：

```bash
IDF_PATH=/path/to/esp-idf ESP_MATTER_PATH=/path/to/esp-matter source ./env.sh
```

esp-matter 的 `export.sh` 会自动设置 `ESP_MATTER_PATH` 并把 pigweed/gn 加入 PATH，无需手动改 PATH。

## 构建与烧录

```bash
source ./env.sh              # 激活环境（见上）

# 首次构建：设置目标芯片（依据 sdkconfig.defaults 生成 sdkconfig）
idf.py set-target esp32c6

idf.py build
idf.py flash monitor

# 需要留存日志时：
mkdir -p logs
stdbuf -oL idf.py flash monitor 2>&1 | tee ./logs/$(date +%Y%m%d_%H%M%S).txt
```

> **默认（省电）配置关闭了串口控制台**（`CONFIG_ESP_CONSOLE_NONE=y`），`idf.py monitor` 不会有任何输出。要看日志需临时把 sdkconfig 改为 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` 重新构建烧录，调试完再改回。
>
> **修改 `sdkconfig.defaults` 后**需让 sdkconfig 重新生成：`rm sdkconfig && idf.py build`（或 `idf.py fullclean`）。
>
> 若设备陷入崩溃重启循环，按住 **BOOT** 再按 **RESET** 进入下载模式后重新烧录。

## 配对

1. 烧录启动后，串口打印二维码 URL 和 Manual Pairing Code（默认 `34970112332`）
2. 打开苹果家庭 App → 添加配件 → 扫描二维码或输入手动配对码
3. 等待配网完成（约 30–60 秒，包含 BLE PASE + Thread 入网）
4. 设备显示为 **Window Covering（Roller Shade）** 类型

> **注意**：配对码来自 CHIP 测试参数（`CONFIG_ENABLE_TEST_SETUP_PARAMS=y`，VID `0xFFF1` / PID `0x8000`，passcode `20202021`、discriminator `3840`），**仅限开发使用，不可用于商用产品**。
>
> **注意**：配网需要家中有 Thread Border Router 在线。Apple 家庭中 HomePod mini 和 Apple TV 4K 均可充当 Border Router，多台设备自动组成 Thread mesh。

## 通信：Thread over Matter

使用 ESP32-C6 内置 802.15.4 无线电，无需 Wi-Fi。

**需求**：家中至少一台 Thread Border Router（Apple HomePod mini / Apple TV 4K 2nd/3rd gen）。多台 HomePod mini 可组成 Thread mesh，覆盖更大范围。

**配网流程**：
1. 设备上电后自动开启 BLE 广播
2. 用 Apple 家庭 App 扫描串口打印的二维码（或输入 Manual Pairing Code）
3. BLE 完成 PASE 握手 + 证书交换，Border Router 下发 Thread 网络凭据
4. 设备加入 Thread 网络，CASE 会话建立，配网完成
5. BLE 自动关闭，后续通过 Thread 通信

## ICD（间歇连接设备）

本设备为电池供电，配置为 Matter **LIT ICD**（Long Idle Time Intermittently Connected Device）。

### 省电工作原理

ICD 分两个阶段：

**阶段 1：SIT 轮询（配网后，Apple Home 注册前）**

设备每 5 秒向 Thread 父节点（HomePod mini）发一次 Data Request，取回缓存的命令包。无线电其余时间睡眠。

**阶段 2：LIT Check-In（Apple Home 注册为 ICD 客户端后）**

Apple Home 完成 `RegisterClient` 后，设备切换到 LIT 模式：
- 每 **10 分钟**主动发一次 Check-In 消息
- Apple Home 有命令时回复 `Stay Active`，设备进入活跃模式处理
- Apple Home 无命令时忽略，设备立即重新入睡
- 无线电占空比极低，静止时电流可降至 ~1 mA 以下

### 参数配置

| 参数 | 值 | 说明 |
|------|----|------|
| 慢速轮询间隔 | 5000 ms | LIT 注册前的 SIT 轮询频率 |
| 快速轮询间隔 | 500 ms | 活跃模式期间的轮询频率 |
| 活跃模式时长 | 2000 ms | 收到命令后保持活跃的最短时间 |
| 活跃模式阈值 | 5000 ms | LIT 要求最低 5 秒 |
| 空闲模式间隔 | 600 s | LIT Check-In 间隔（10 分钟） |

### 用户触发唤醒（UAT）

按键按下时调用 `ICDNotifier::NotifyNetworkActivityNotification()`，设备立即进入活跃模式，Matter 栈可即时处理本地操作并上报状态。

### 确认 LIT 已生效

配网后查看串口日志，看到以下内容说明 Apple Home 已完成 ICD 客户端注册：
```
ICDManager: RegisterClient command received
```

## 功耗

Thread 待机电流约 1–5 mA（相比 Wi-Fi 的 15–25 mA 大幅降低）。

**已实施：**

- **Thread（ICD/SED）**：设备按间隔轮询父节点，其余时间无线电休眠
- **IEEE 802.15.4 睡眠**：`CONFIG_IEEE802154_SLEEP_ENABLE`，Thread 无线电按 ICD 调度休眠
- **PM 框架 + Tickless Idle**：`CONFIG_PM_ENABLE` + `CONFIG_FREERTOS_USE_TICKLESS_IDLE`，CPU 在所有任务阻塞时进 light sleep
- **DFS**：`CONFIG_PM_DFS_INIT_AUTO`，动态调整 CPU 频率
- **window_task 事件驱动**：静止时任务挂起在 `xEventGroupWaitBits()`，收到运动命令才恢复 100 ms 轮询
- **iot_button power save**：`enable_power_save = true`，按键改用 GPIO 中断唤醒
- **LED 定时器按需启停**：`LED_OFF` 时调用 `esp_timer_stop()` 停止 100 ms 定时器；切换到闪烁/常亮模式时再启动，避免 CPU 空转唤醒

**待优化：**

- **霍尔传感器**：目前运动期间轮询 `gpio_get_level()`，可改为 GPIO 中断；静止时可关闭内部上拉（约 0.1 mA）
- **DRV8833 nSLEEP**：若 PCB 已接 nSLEEP 引脚，静止时拉低可完全关断驱动芯片（节省约 1–2 mA）

## 踩坑记录

调试过程中遇到的非显而易见问题，供以后参考。

### 1. 配网卡住：FailSafe 超时（`Failed to complete commissioning`）

**现象**：Apple 家庭扫码后转圈，约 60 秒超时失败。串口出现 `Long dispatch time: 2084ms`。

**根因**：`CONFIG_LWIP_IPV6_AUTOCONFIG=y` 时，LwIP 和 OpenThread 同时争抢 IPv6 地址配置，互相阻塞，导致 Matter 消息处理严重延迟，FailSafe 计时器超时。

**修复**：

```
CONFIG_LWIP_IPV6_AUTOCONFIG=n   # OpenThread 全权管理 IPv6，禁止 LwIP 自动配置
CONFIG_USE_MINIMAL_MDNS=n       # 必须用完整 mDNS 平台实现，Thread 发现才正常
CONFIG_LWIP_ND6=n
CONFIG_LWIP_IPV4=n
CONFIG_DISABLE_IPV4=y
```

参照官方 `esp-matter/examples/light_switch/sdkconfig.defaults.esp32c6` 对齐即可。

---

### 2. 配网成功，28 秒后 Thread 断连

**现象**：配网完成后设备在 Apple 家庭显示"无响应"，串口约 28 秒后出现 Thread 断开。

**根因**：`CONFIG_IEEE802154_SLEEP_ENABLE=y` 必须配合 `CONFIG_ENABLE_ICD_SERVER=y` 使用。ICD Server 负责在正确时机唤醒无线电收包；若只开 `IEEE802154_SLEEP_ENABLE` 而没有 ICD，无线电会持续睡眠，HomePod mini 收不到心跳，约 28 秒后把子节点踢出 Thread 网络。

**修复**：两项同时启用：

```
CONFIG_IEEE802154_SLEEP_ENABLE=y
CONFIG_ENABLE_ICD_SERVER=y
```

---

### 3. 链接报错：`undefined reference to 'MatterIcdManagementPluginServerInitCallback()'`

**根因**：`CONFIG_ENABLE_ICD_SERVER=y` 但 `CONFIG_SUPPORT_ICD_MANAGEMENT_CLUSTER=n`，ICD 管理 Cluster 的入口函数未编译进来。

**修复**：

```
CONFIG_SUPPORT_ICD_MANAGEMENT_CLUSTER=y
```

---

### 4. 启动崩溃：`ICDManager::Init abort` — LIT 条件不满足

**现象**：开机 abort，日志提示 LIT ICD 初始化失败。

**LIT 的三个硬性要求**（缺一不可）：

| 要求 | 配置项 |
|------|--------|
| Check-In 协议（允许控制器注册） | `CONFIG_ICD_CLIENTS_SUPPORTED_PER_FABRIC=2` |
| ActiveModeThreshold ≥ 5000 ms | `CONFIG_ICD_ACTIVE_MODE_THRESHOLD_MS=5000` |
| 实现 UAT（用户触发唤醒） | 见下条 |

---

### 5. 按键触发崩溃：`Chip stack locking error`

**现象**：按下按键后崩溃，串口报 `Code is unsafe/racy` at `ICDManager.cpp`。

**根因**：`ICDNotifier::NotifyNetworkActivityNotification()` 是 CHIP 栈内部函数，必须在 CHIP 任务上下文中调用。iot_button 的回调运行在独立 FreeRTOS 任务里，直接调用会触发 CHIP 的线程安全检查。

**修复**：通过 `ScheduleWork` 投递到 CHIP 任务执行：

```cpp
static void icd_wake_work(intptr_t) {
    chip::app::ICDNotifier::GetInstance().NotifyNetworkActivityNotification();
}
// 在按键回调里调用：
chip::DeviceLayer::PlatformMgr().ScheduleWork(icd_wake_work);
```

所有会操作 Matter 栈的代码（从非 CHIP 任务发起）都应走 `ScheduleWork`。

---

## 待办

- [x] 霍尔传感器端点检测（两端磁铁，单传感器）
- [x] 行程时长校准与 NVS 持久化
- [x] 百分比位置控制（GoToLiftPercentage）
- [x] StopMotion 命令支持
- [x] 位置断电恢复
- [x] 通信层从 Wi-Fi 迁移到 Thread（IEEE 802.15.4）
- [x] Matter over Thread 配网（Apple 家庭）
- [x] ICD 配置（电池设备省电）
- [ ] DRV8833 nSLEEP 控制（静止时拉低关断驱动芯片，节省 ~1–2 mA）
- [ ] 工厂重置（长按按键 5 秒）

## 许可

代码以 [MIT License](LICENSE) 发布。`docs/datasheets/` 内的 XIAO ESP32-C6 原理图版权归 Seeed Studio（OSHW，来源见 [docs/datasheets/README.md](docs/datasheets/README.md)）。
