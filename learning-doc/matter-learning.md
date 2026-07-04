# Matter 协议学习笔记

本文记录在 ESP32-C6 上用 esp-matter SDK 实现 Matter 配网的完整学习过程，
包括协议概念、代码实现、踩过的坑和解决方案。

---

## 1. Matter 是什么？

Matter（曾用名 CHIP / Project Connected Home over IP）是由 CSA 联盟
（Apple、Google、Amazon、三星等共同发起）制定的智能家居统一标准。

**核心目标**：一个设备，任意平台都能控制。
- Apple Home
- Google Home
- Amazon Alexa
- Samsung SmartThings

**通信层**：Matter 运行在 IP 网络上（Wi-Fi 或 Thread），
不依赖私有云，本地网络内就能工作。

---

## 2. Matter 数据模型

Matter 用一套严格的层次结构描述设备能力：

```
Node（节点，即一个物理设备）
└── Endpoint 0（根节点，固定，包含设备基础信息）
└── Endpoint 1（你的设备功能，如 On/Off Light）
    ├── Identify Cluster     （必选，用于"识别设备"功能）
    ├── On/Off Cluster       （核心：开关状态）
    └── Power Source Cluster （可选：电量信息）
        └── BatPercentRemaining Attribute（电量百分比）
```

- **Node**：一个 ESP32 就是一个 Node
- **Endpoint**：一个功能单元。一个灯泡插座可以有多个 Endpoint（每个插孔一个）
- **Cluster**：一组相关功能（如 On/Off Cluster 包含开关相关的所有属性和命令）
- **Attribute**：具体的值（如 `OnOff` 属性是 bool，表示当前开关状态）
- **Command**：操作指令（如 `Toggle` 命令切换开关状态）

---

## 3. 配网（Commissioning）完整流程

### 阶段 1：设备广播（未配对状态）

设备首次启动，NVS 中没有 Fabric 记录，自动进入 Commissioning Mode：
- 开启 BLE 广播，广播载荷包含 **Discriminator**（区分码，12 位，用于过滤）
- 等待 Commissioner（手机 App）扫描 QR Code

QR Code 格式：`MT:Y.K9042C00KA0648G00`（Base38 编码，此处为 CHIP 标准测试示例码，对应 passcode 20202021 / discriminator 3840）

包含信息：
- Vendor ID (VID)
- Product ID (PID)
- Setup Passcode（配对密码，8 位数字，如 `20202021`）
- Discriminator

手动配对码：11 位数字，格式 `XXXXX-XXXXXX`（前 5 位 + 后 6 位）

### 阶段 2：PASE（密码认证密钥交换）

手机 App 扫到 QR Code 后：
1. 通过 BLE 连接设备
2. 使用 **SPAKE2+** 算法完成密钥交换

**SPAKE2+ 的核心思想**：
双方在不传输明文密码的情况下，证明自己知道同一个密码，并协商出共享会话密钥。
即使有人监听 BLE 通信，也无法反推出配对密码。

### 阶段 3：Commissioning（配置阶段）

通过 PASE 建立的安全通道，App 向设备发送：
- Wi-Fi 凭据（SSID + 密码）
- NOC（Node Operational Certificate，设备的 Matter 身份证书）
- Root CA 证书（用于后续 CASE 认证）
- ACL（访问控制列表，控制谁能控制这个设备）

设备把这些存到 NVS，重启后就"加入了 Fabric"。

### 阶段 4：CASE（证书认证会话建立）

配网完成后，日常通信用 CASE 代替 PASE：
- 双方用 NOC 证书互相验证身份（类似 TLS 双向认证）
- 通过 Wi-Fi/Thread 通信（不再用 BLE）
- 支持订阅（Controller 订阅设备属性变化，设备 push 通知）

### Fabric 的概念

- **Fabric** = Matter 的"信任域"，相当于一个"家"
- 一个设备最多可以加入 **16 个 Fabric**（被 16 个"家"添加）
- `FabricTable.FabricCount()` 返回当前已加入的 Fabric 数量
- `FabricCount() > 0` → 设备已配对

---

## 4. esp-matter SDK 代码结构

### 初始化流程

```cpp
// 1. 创建节点（自动创建 Endpoint 0）
node::config_t node_config;
node_t *node = node::create(&node_config, attribute_update_cb, identify_cb);

// 2. 创建设备端点（Endpoint 1，On/Off Light 类型）
on_off_light::config_t light_config;
light_config.on_off.on_off = false;                    // 初始状态
light_config.on_off_lighting.start_up_on_off = nullptr; // 重启不强制设状态
endpoint_t *endpoint = on_off_light::create(node, &light_config, ENDPOINT_FLAG_NONE, nullptr);

// 3. 添加 Power Source Cluster（可选，用于暴露电量）
esp_matter::cluster::power_source::config_t ps_config;
ps_config.status = 1;   // Active
ps_config.order  = 0;   // 主电源
ps_config.feature_flags = esp_matter::cluster::power_source::feature::battery::get_id();
ps_config.features.battery.bat_charge_level = 0;   // OK
cluster_t *ps_cluster = esp_matter::cluster::power_source::create(endpoint, &ps_config, CLUSTER_FLAG_SERVER);

// BatPercentRemaining 是可选属性，battery::add() 不会自动创建，需手动加
esp_matter::cluster::power_source::attribute::create_bat_percent_remaining(
    ps_cluster,
    nullable<uint8_t>(200),  // 初始值 200 = 100%（Matter 用半百分比，0-200）
    nullable<uint8_t>(0),
    nullable<uint8_t>(200)
);

// 4. 启动协议栈
esp_matter::start(app_event_cb);
```

### 属性更新（双向同步）

**方向 1：Controller → 设备**（App 控制硬件）

```cpp
// attribute_update_cb 在属性被写入前触发
static esp_err_t app_attribute_update_cb(
    esp_matter::attribute::callback_type_t type,
    uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
    esp_matter_attr_val_t *val, void *priv_data)
{
    if (type == esp_matter::attribute::PRE_UPDATE) {
        if (cluster_id == 0x00000006 && attribute_id == 0x00000000) {
            // OnOff Cluster，OnOff Attribute
            led_ctrl_set_state(val->val.b);  // 驱动 LED
        }
    }
    return ESP_OK;
}
```

**方向 2：设备 → Controller**（物理按钮同步到 App）

```cpp
// 按钮按下后，更新 Matter 属性，触发订阅通知
esp_matter_attr_val_t val = esp_matter_bool(new_state);
esp_matter::attribute::update(
    endpoint_id,
    0x00000006,  // OnOff Cluster
    0x00000000,  // OnOff Attribute
    &val
);
// Apple Home 收到订阅报告后，自动刷新 App 显示的开关状态
```

**更新电量**（每 10 秒）：

```cpp
// Matter BatPercentRemaining 范围 0-200（半百分比），所以 50% = 100
esp_matter_attr_val_t val = esp_matter_nullable_uint8((uint8_t)(level * 2));
esp_matter::attribute::update(endpoint_id, 0x0000002F, 0x0000000C, &val);
// 0x002F = Power Source Cluster
// 0x000C = BatPercentRemaining Attribute
```

### 工厂重置

```cpp
// 清除所有 Fabric 记录，设备重启回到未配对状态
chip::Server::GetInstance().ScheduleFactoryReset();
```

这个 API 是线程安全的，可以从任意任务（如按钮回调）调用，
内部会把重置操作 post 到 Matter 自己的任务队列。

---

## 5. 配网码的读取

配网完成后用 `OnboardingCodesUtil.h` 提供的函数读取：

```cpp
#include <setup_payload/OnboardingCodesUtil.h>

char qr_code[64];
char manual_code[16];
chip::MutableCharSpan qr_span(qr_code, sizeof(qr_code));
chip::MutableCharSpan code_span(manual_code, sizeof(manual_code));
chip::RendezvousInformationFlags ble_flags(chip::RendezvousInformationFlag::kBLE);

GetQRCode(qr_span, ble_flags);           // "MT:XXXXXXXXXX"
GetManualPairingCode(code_span, ble_flags); // "34970112332"
```

注意：`GetQRCode` / `GetManualPairingCode` 是**全局函数**，不在 `chip::` 命名空间里。

---

## 6. 事件回调（ChipDeviceEvent）

Matter 通过事件机制通知应用层：

```cpp
static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case DeviceEventType::kCommissioningWindowOpened:
        // BLE 广播已开启，等待扫码
        break;
    case DeviceEventType::kCommissioningComplete:
        // 配对完成！设备已加入 Fabric
        // 此时可以通知 e-paper 切换显示内容
        break;
    case DeviceEventType::kFabricRemoved:
        // Fabric 被删除（用户在 App 里删除了设备）
        // 如果设备在线，会收到此事件并自动重新打开配网窗口
        break;
    case DeviceEventType::kBLEDeinitialized:
        // 配网完成后 Matter SDK 自动关闭 BLE
        break;
    }
}
```

---

## 7. Power Source Cluster 的坑

### 坑 1：feature_flags 必须在 create() 前设置

`power_source::create()` 内部会：
1. 读取 `config->feature_flags`
2. 验证 exactly one of (Wired, Battery) 被设置（否则 assert 崩溃）
3. 自动调用对应的 `feature::battery::add()` 或 `feature::wired::add()`

**错误做法**：先 `create()`，再手动调用 `feature::battery::add()`
→ 触发 assert：`"Exactly one of the feature(s) must be supported from (PowerSource,Battery)"`

**正确做法**：在 `create()` 前设好 `feature_flags` 和 `features.battery`

### 坑 2：BatPercentRemaining 是可选属性

`feature::battery::add()` 只添加必选属性（`BatChargeLevel`、`BatReplacementNeeded`）。
`BatPercentRemaining` 是 Optional，需要手动调用：

```cpp
esp_matter::cluster::power_source::attribute::create_bat_percent_remaining(
    cluster, nullable<uint8_t>(200), nullable<uint8_t>(0), nullable<uint8_t>(200));
```

如果不添加，`attribute::update()` 会报 `Failed to get attribute handle`。

### 坑 3：nullable 的命名空间

`nullable<T>` 是 esp-matter 自己定义的全局模板类（在 `esp_matter_attribute_utils.h`），
**不在任何命名空间里**，直接用 `nullable<uint8_t>(200)` 即可。
写 `esp_matter::nullable<uint8_t>` 会编译报错。

---

## 8. Apple Home 的电量显示

只要 Power Source Cluster 的 `BatPercentRemaining` 属性被正确创建并有有效值，
Apple Home 就会显示电量图标，On/Off Light 也不例外。

实测本项目（On/Off Light + Power Source Cluster）在 Apple Home 中正常显示电量百分比。

**关键条件**：
1. `feature_flags` 包含 Battery feature bit（在 `create()` 前设置）
2. `BatPercentRemaining` 属性被手动创建（可选属性，`battery::add()` 不会自动创建）
3. 通过 `attribute::update()` 定期更新属性值

也可以用 `chip-tool` 直接读取验证数据是否到位：

```bash
chip-tool powersource read bat-percent-remaining <node-id> 1
```

---

## 9. 构建环境配置

### esp-matter 安装

克隆 esp-matter 后（路径以 `~/esp/esp-matter` 为例，可自选），**必须**运行一次 `install.sh`：

```bash
cd ~/esp/esp-matter
./install.sh   # 生成 .environment（含 gn/pigweed 工具）和 build_overrides/pigweed_environment.gni
```

如果跳过 `install.sh`，Matter SDK 的 gn 构建会找不到 `pigweed_environment.gni` 而报错。

注意：`install.sh` 和后续 source 的必须是**同一份** esp-matter checkout。只要如此，esp-matter 的 `export.sh` 会自动把 `.environment/cipd/packages/pigweed` 加入 PATH，无需任何手动 PATH 配置。

### 环境激活（每次开发前，顺序固定）

```bash
source <ESP-IDF 安装目录>/export.sh      # 如 ~/esp/esp-idf/export.sh
source <esp-matter 安装目录>/export.sh   # 如 ~/esp/esp-matter/export.sh，会自动导出 ESP_MATTER_PATH
```

本项目根目录的 `env.sh` 封装了以上两步（路径通过 `IDF_PATH` / `ESP_MATTER_PATH` 环境变量覆盖）。也可在自己的 shell 配置里加 alias，路径按本机实际安装位置调整。

### 自定义分区表（4MB Flash）

Matter SDK 编译后约 1.6MB，默认 2MB app 分区放不下，需要自定义：

```csv
nvs,        data, nvs,      0x10000, 0x4000,
phy_init,   data, phy,      0x14000, 0x1000,
nvs_keys,   data, nvs_keys, 0x15000, 0x1000, encrypted
fctry,      data, nvs,      0x16000, 0x6000,   ← Matter DAC 证书
app,        app,  factory,  0x20000, 0x3E0000,  ← 3.75MB，59% 空闲
```

`sdkconfig.defaults` 需要显式设置 flash 大小：
```
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_PARTITION_TABLE_OFFSET=0xC000
```

---

## 10. 调试技巧

### 查看 Matter 日志级别

Matter SDK 日志量很大，正常使用时建议关闭详细日志：
```
CONFIG_ENABLE_CHIP_SHELL=n
CONFIG_CHIP_LOG_DEFAULT_LEVEL_PROGRESS=y   # 只看 Progress 级别
```

### chip-tool 常用命令

```bash
# 配对（通过蓝牙）
chip-tool pairing ble-wifi <node-id> <ssid> <password> <passcode> <discriminator>

# 读属性
chip-tool onoff read on-off <node-id> <endpoint-id>
chip-tool powersource read bat-percent-remaining <node-id> 1

# 写属性（控制 LED）
chip-tool onoff on <node-id> 1
chip-tool onoff off <node-id> 1
chip-tool onoff toggle <node-id> 1
```

### 工厂重置

- **长按按钮 3 秒**：调用 `ScheduleFactoryReset()`，清除 NVS Fabric 记录，重启
- **命令行**：`idf.py erase-flash && idf.py flash monitor`

从 Apple Home 删除设备时，如果设备离线，Home 不会通知设备清除 Fabric。
设备重启后仍认为"已配对"（NVS 里还有 Fabric 记录），不会显示配对码。
此时需要长按按钮或擦除 Flash 来重置。
