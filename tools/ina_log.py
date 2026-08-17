#!/usr/bin/env python3
"""记录 ina-sampler 固件的串口 CSV 输出，并实时打印滚动统计。

用法：
    python3 tools/ina_log.py /dev/cu.usbmodemXXXX logs/idle-2026-08-18.csv
    python3 tools/ina_log.py <port> <outfile> --window 60

输出文件是固件原样的 CSV，行首多一列本机时间戳，方便和 diag log / 充电曲线对齐。
串口掉线（复位、重枚举）自动重连。Ctrl-C 结束时打印整段汇总。
"""
import sys, time, argparse, datetime as dt

try:
    import serial
except ImportError:
    sys.exit("需要 pyserial：source env.sh 后再跑，或 pip install pyserial")

ap = argparse.ArgumentParser()
ap.add_argument("port")
ap.add_argument("outfile")
ap.add_argument("--window", type=int, default=60, help="滚动统计窗口（秒）")
a = ap.parse_args()


def open_port():
    for _ in range(400):
        try:
            return serial.Serial(a.port, 115200, timeout=1)
        except Exception:
            time.sleep(0.25)
    sys.exit("串口一直打不开")


ser = open_port()
buf = b""
rows = []          # (datetime, mean_uA, min_uA, max_uA, pct_below)
all_mean, all_n = 0.0, 0
print(f"记录中 → {a.outfile}（Ctrl-C 结束）")

with open(a.outfile, "a") as f:
    try:
        while True:
            try:
                chunk = ser.read(4096)
            except serial.SerialException:
                try:
                    ser.close()
                except Exception:
                    pass
                time.sleep(2)
                ser = open_port()
                continue
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").strip()
                if not line:
                    continue
                now = dt.datetime.now()
                f.write(f"{now:%Y-%m-%d %H:%M:%S},{line}\n")
                f.flush()

                if line.startswith("#") or line.startswith("t_ms"):
                    print(line)
                    continue
                parts = line.split(",")
                # INA226: t_ms,n,mean,min,max,bus_mV,pct_below
                if len(parts) == 7:
                    try:
                        mean, mn, mx, bus, pct = (int(parts[2]), int(parts[3]),
                                                  int(parts[4]), int(parts[5]), int(parts[6]))
                    except ValueError:
                        continue
                    rows.append((now, mean, mn, mx, pct))
                    all_mean += mean
                    all_n += 1
                    cut = now - dt.timedelta(seconds=a.window)
                    rows[:] = [r for r in rows if r[0] >= cut]
                    w = sum(r[1] for r in rows) / len(rows)
                    wmin = min(r[2] for r in rows)
                    wmax = max(r[3] for r in rows)
                    wpct = sum(r[4] for r in rows) / len(rows)
                    print(f"{now:%H:%M:%S}  当前 {mean/1000:7.3f} mA   "
                          f"{a.window}s 均 {w/1000:7.3f} mA  谷 {wmin/1000:6.3f}  峰 {wmax/1000:7.2f}  "
                          f"睡眠占比 {wpct:3.0f}%   vbus {bus/1000:.3f} V")
                else:
                    print(line)
    except KeyboardInterrupt:
        pass

if all_n:
    print(f"\n整段：{all_n} 秒，平均 {all_mean/all_n/1000:.3f} mA "
          f"→ 24h 耗电 ≈ {all_mean/all_n/1000*24:.0f} mAh")
