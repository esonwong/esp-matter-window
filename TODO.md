# TODO

## 功耗 / 续航测试

所有功耗 / 续航测试见 [`docs/power-tests.md`](docs/power-tests.md)。
第 1 轮已完成（idle 16.5 mA），第 2 轮太阳能测试进行中。

## 待办

### 1. 实机长期观察（高优先级）

装到窗户上连续运行约一周，目标：

- 拿到完整的首轮 vbat 曲线（256 条 diag ring buffer 应能覆盖一个完整电池周期）
- 看电机启动次数 vs 电量掉幅，验证省电是否到位
- 看 ICD LIT 模式下 Thread 链路稳定性

### 2. Dump diag log 流程

生产固件 console 关闭，无法直接 dump。步骤：

1. 临时打开 console：把 `sdkconfig` 里 `CONFIG_ESP_CONSOLE_NONE=y` 改成 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`
2. `idf.py build flash`（**不要 erase**，否则 NVS 配对与历史 diag 数据丢失）
3. boot 时 diag_log 自动 printf 全部条目（CSV 格式）
4. 拿到数据后改回 console 关闭并重新 flash，否则 ICD LIT 平均功耗会高几 mA

### 3. 低优先级

- `sdkconfig` 与 `sdkconfig.defaults` 存在历史遗留的小不一致，可做一次 `rm sdkconfig && idf.py build` 从 defaults 重新生成
- `main_app.cpp` / `matter_app.cpp` 里的 reset reason 调试打印可保留也可删；定型后大概率 reset 都是 POWERON，观察价值不大
- DRV8833 nSLEEP 控制（静止时拉低关断驱动芯片，省 ~1.6 mA）
- 工厂重置（长按按键 5 秒）

## 重要踩坑备忘

- **HomePod mDNS 桥缓存**可导致配网卡在 SRP 之后无 CASE —— 先重启 HomePod
- **`MAX_DYNAMIC_ENDPOINT_COUNT=2` 太小**：root(0) + WC(1) 已占满，第三个 endpoint 直接被拒
- **NVS 16KB 撑爆**：Matter 多 fabric + diag_log 一起跑会触发 `otPlatAssertFail`，错误码 `0x1105 = ESP_ERR_NVS_NOT_ENOUGH_SPACE`（已改 32KB）
- **ICD 关闭后 `ICDNotifier` 找不到**：`window_ctrl.cpp` 里用 `#if CHIP_CONFIG_ENABLE_ICD_SERVER` 守卫
- macOS 抓串口：`idf.py monitor` 需要 TTY；macOS 没有 GNU `timeout`，可用 `gtimeout` 或后台进程 + `pkill`
