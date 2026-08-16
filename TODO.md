# TODO

## 功耗 / 续航测试

所有功耗 / 续航测试见 [`docs/power-tests.md`](docs/power-tests.md)。
第 1 轮已完成（idle 16.5 mA，但旧电池容量存疑），第 2 轮太阳能测试已完成，**第 3 轮新电池基线进行中（2026-08-16 起）**。

## 待办

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
- DRV8833 nSLEEP 控制（静止时拉低关断驱动芯片，省 ~1.6 mA）
- ~~工厂重置（长按按键 5 秒）~~ 已实现并编译通过（2026-08-16，长按动作待实机验证）
- ~~低压锁定电机~~ 已实现（vbat < 3.3 V 拒绝启动）

## 重要踩坑备忘

- **HomePod mDNS 桥缓存**可导致配网卡在 SRP 之后无 CASE —— 先重启 HomePod
- **`MAX_DYNAMIC_ENDPOINT_COUNT=2` 太小**：root(0) + WC(1) 已占满，第三个 endpoint 直接被拒
- **NVS 16KB 撑爆**：Matter 多 fabric + diag_log 一起跑会触发 `otPlatAssertFail`，错误码 `0x1105 = ESP_ERR_NVS_NOT_ENOUGH_SPACE`（已改 32KB）
- **ICD 关闭后 `ICDNotifier` 找不到**：`window_ctrl.cpp` 里用 `#if CHIP_CONFIG_ENABLE_ICD_SERVER` 守卫
- macOS 抓串口：`idf.py monitor` 需要 TTY；macOS 没有 GNU `timeout`，可用 `gtimeout` 或后台进程 + `pkill`
