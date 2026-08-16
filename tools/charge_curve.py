#!/usr/bin/env python3
"""分析 logs/charge-*.csv（vbatlog 记录：`YYYY-mm-dd HH:MM:SS,vbat_mv`，# 开头为事件行）。

输出：起止时间/电压、总时长、每 30 分钟一段的平均斜率、估算充入 mAh、是否已到终止。

估算模型（SGM40567 线性充电，spec 120 mA）：
  vbat < 3.0 V        预充  ~12 mA
  3.0 V ≤ vbat < 4.15 V  CC   ~120 mA（实测平均 ~98 mA，见 HARDWARE.md，用 --cc 覆盖）
  vbat ≥ 4.15 V       CV   电流线性衰减到终止 ~12 mA，按 60 mA 均值估
注意：ADC 测的是 BAT pad，充电时比开路电压高 20–50 mV。

用法：python3 tools/charge_curve.py logs/charge-2026-08-16.csv [--cc 98]
"""
import sys, argparse, datetime as dt

ap = argparse.ArgumentParser()
ap.add_argument("csv")
ap.add_argument("--cc", type=float, default=98.0, help="CC 阶段平均电流 mA")
ap.add_argument("--bucket", type=int, default=30, help="分段分钟数")
a = ap.parse_args()

pts, events = [], []
for line in open(a.csv):
    line = line.strip()
    if not line: continue
    t, v = line.split(",", 1)
    t = dt.datetime.strptime(t, "%Y-%m-%d %H:%M:%S")
    if v.startswith("#"): events.append((t, v[1:].strip())); continue
    pts.append((t, int(v)))
if len(pts) < 2: sys.exit("数据不足")

t0, v0 = pts[0]; t1, v1 = pts[-1]
dur_h = (t1 - t0).total_seconds() / 3600
print(f"起点 {t0}  {v0} mV")
print(f"终点 {t1}  {v1} mV")
print(f"时长 {dur_h:.2f} h   总升幅 {v1 - v0} mV   平均 {(v1 - v0) / (dur_h * 60):.2f} mV/min")

# 分段斜率
print(f"\n每 {a.bucket} min 一段：")
b0 = t0; bv0 = v0; last = None
for t, v in pts:
    if (t - b0).total_seconds() >= a.bucket * 60:
        m = (t - b0).total_seconds() / 60
        print(f"  {b0:%m-%d %H:%M} → {t:%H:%M}  {bv0}→{v} mV  {(v - bv0) / m:+.2f} mV/min")
        b0, bv0 = t, v
    last = (t, v)
if last and last[0] != b0:
    m = (last[0] - b0).total_seconds() / 60
    if m > 1: print(f"  {b0:%m-%d %H:%M} → {last[0]:%H:%M}  {bv0}→{last[1]} mV  {(last[1] - bv0) / m:+.2f} mV/min（未满一段）")

# 充入电量估算：按相邻点电压所处阶段积分
mah = 0.0
for (ta, va), (tb, vb) in zip(pts, pts[1:]):
    dh = (tb - ta).total_seconds() / 3600
    if dh > 0.5: continue  # 断档（烧录/休眠）不计
    v = (va + vb) / 2
    i = 12 if v < 3000 else (a.cc if v < 4150 else 60)
    mah += i * dh
print(f"\n估算充入 ≈ {mah:.0f} mAh（CC 按 {a.cc:.0f} mA）")

# 终止判断：最后 60 min 斜率 ≈ 0 且 vbat ≥ 4.15 V
tail = [(t, v) for t, v in pts if (t1 - t).total_seconds() <= 3600]
if len(tail) >= 2:
    slope = (tail[-1][1] - tail[0][1]) / max((tail[-1][0] - tail[0][0]).total_seconds() / 60, 1)
    if v1 >= 4150 and abs(slope) < 0.2:
        print(f"状态：已到 CV 平台/终止（最后 1h {slope:+.2f} mV/min）")
    elif v1 >= 4150:
        print(f"状态：CV 阶段（最后 1h {slope:+.2f} mV/min），接近充满")
    else:
        print(f"状态：仍在充（最后 1h {slope:+.2f} mV/min）")

if events:
    print(f"\n事件 {len(events)} 条：")
    for t, e in events[-10:]: print(f"  {t}  {e[:100]}")
