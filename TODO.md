# TODO

## 功耗 / 续航测试

所有功耗 / 续航测试见 [`docs/power-tests.md`](docs/power-tests.md)。
第 1 轮已完成（idle 16.5 mA，但旧电池容量存疑），第 2 轮太阳能测试已完成，**第 3 轮新电池基线进行中（2026-08-16 起）**。

## 待办

### 0a. ~~第 4 轮基线：验证 light sleep 修复~~ ✅ 完成（2026-08-20）

**修复有效：idle 13 mA → ~4 mA**（84%→80% / 24.3 h），续航 6.5 天 → ~22 天，
阴天太阳能即可收支转正。功能自检正常。详见 `docs/power-tests.md` 第 4 轮。
剩余 ~4 mA 待 INA226 分解；优化顺序：nSLEEP 飞线 → INA226 → ICD poll A/B。

### 0b. 电流实测

INA226 / INA3221 已下单。到货后按 [`docs/current-measurement.md`](docs/current-measurement.md) 焊接，
用 `tools/ina-sampler/` 固件 + `tools/ina_log.py` 直接读 idle 电流，
不再靠 vbat LUT 反推。第一个要回答的问题：**light sleep 到底进没进**
（均值 ~15 mA 且睡眠占比 0% = 没进）。

### 1. 实机长期观察（高优先级）

装到窗户上连续运行约一周，目标：

- 拿到完整的首轮 vbat 曲线（256 条 diag ring buffer 应能覆盖一个完整电池周期）
- 看电机启动次数 vs 电量掉幅，验证省电是否到位
- 看 ICD LIT 模式下 Thread 链路稳定性

### 2. Dump diag log 流程

生产固件 console 关闭，无法直接 dump。用 `sdkconfig.debug` 叠加层单独构建调试固件
（命令见 CLAUDE.md「调试固件」），`-B build-debug` **不擦除**烧录，boot 15 s 后自动 printf CSV。
拿到数据后 `idf.py flash`（生产 `build/`）烧回，否则 ICD LIT 平均功耗会高几 mA。

**2026-08-16 教训**：旧电池放空后 brownout 复位循环把整个环冲成了 BOOT，历史 HOURLY 全丢
（已修：连续同原因 BOOT 合并）。以后电池快空时先 dump 再说。

### 3. 低优先级

- ~~`sdkconfig` 与 `sdkconfig.defaults` 不一致~~ 2026-08-16 已从 defaults 重新生成
- `main_app.cpp` / `matter_app.cpp` 里的 reset reason 调试打印可保留也可删；定型后大概率 reset 都是 POWERON，观察价值不大
- DRV8833 nSLEEP 控制（静止时拉低关断驱动芯片，省 ~1.6 mA）——软件已预留（`MOTOR_NSLEEP_GPIO`），等飞线
- ~~工厂重置（长按按键 5 秒）~~ ✅ 实机验证通过（2026-08-20，diag 环确认触发并重启；
  当时 LED 来不及闪导致被按了两次，已改为快闪 2 秒再重置）
- ~~低压锁定电机~~ 已实现（vbat < 3.3 V 拒绝启动）
- ~~低电量 deep sleep~~ 已实现并用测试固件验证 sleep→timer 唤醒→恢复（2026-08-16）；真实低压场景待实机观察

## 重要踩坑备忘

- **HomePod mDNS 桥缓存**可导致配网卡在 SRP 之后无 CASE —— 先重启 HomePod
- **`MAX_DYNAMIC_ENDPOINT_COUNT=2` 太小**：root(0) + WC(1) 已占满，第三个 endpoint 直接被拒
- **NVS 16KB 撑爆**：Matter 多 fabric + diag_log 一起跑会触发 `otPlatAssertFail`，错误码 `0x1105 = ESP_ERR_NVS_NOT_ENOUGH_SPACE`（已改 32KB）
- **ICD 关闭后 `ICDNotifier` 找不到**：`window_ctrl.cpp` 里用 `#if CHIP_CONFIG_ENABLE_ICD_SERVER` 守卫
- macOS 抓串口：`idf.py monitor` 需要 TTY；macOS 没有 GNU `timeout`，可用 `gtimeout` 或后台进程 + `pkill`
