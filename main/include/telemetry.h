#pragma once
// ── 遥测上报 + OTA（走 Thread → HomePod 边界路由器 → 公网）──────────────
//
// 每 TELEMETRY_INTERVAL_SEC 醒一次：把 diag 环新增条目 POST 到 Cloudflare Worker，
// 响应里带 ota 字段则用 esp_https_ota 拉新固件写入 OTA 槽并重启。
// 无网/失败静默跳过，条目留在环里下轮重试（NVS 记"已上报到 seq"）。
//
// 服务端与查看方式见 docs/telemetry.md。凭据在 main/telemetry_secrets.h（gitignore）。

// 启动上报任务（matter_app_init 之后调用；内部延迟首报，等 Thread attach）
void telemetry_start(void);

// 立即触发一次上报（如按键调试用），非阻塞
void telemetry_report_now(void);
