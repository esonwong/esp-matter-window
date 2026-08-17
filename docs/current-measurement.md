# 电流测量（INA226 / INA3221 + FireBeetle）

用 DFRobot FireBeetle 2 ESP32-C6 当采样器，读 INA226/INA3221，串口吐 CSV。
解决的问题：**idle 电流到底是多少**——在此之前只能靠 vbat LUT 反推，
而 2026-08-16 的教训是 LUT 中下段偏差 5–8%，平台段几小时不动，反推出来的
16.5 mA 不可信。

固件：`tools/ina-sampler/`　上位机：`tools/ina_log.py`

---

## 接法 A：主控电池电流（INA226）

**高边分流**——分流电阻串在电池正极侧。

```
电池 (+) ──► [IN+ ] INA226 [ IN- ] ──► XIAO BAT+ 焊盘
电池 (−) ──┬────────────────────────► XIAO BAT− 焊盘
           └──────────────► FireBeetle GND

FireBeetle 3V3    ──► INA226 VCC
FireBeetle 19/SDA ──► INA226 SDA
FireBeetle 20/SCL ──► INA226 SCL
FireBeetle GND    ──► INA226 GND
INA226 VBUS       ──► IN- 那一侧（= XIAO BAT+），读主控实际拿到的电压
```

FireBeetle 自己用板载 LiPo 或 USB 供电都行；USB 插电脑才能收 CSV。

**为什么高边不用低边**：低边（串在 GND 侧）只要主控 USB 一插，地就通过电脑
和 FireBeetle 连通，分流电阻被旁路，读数直接失效。高边不受影响，而且分流在
正极时电流是**有符号**的——放电为正、充电为负，同一套接线既能测 idle 也能
测充电电流。

**0.1 Ω 分流的压降**：idle 16 mA → 1.6 mV，电机 500 mA → 50 mV。都可以忽略，
不像早先设想的 10 Ω 方案那样测电机会把电压拉穿。**一套接线测全工况。**

---

## 接法 B：太阳能收支（INA3221）

三通道各有独立分流，一次看清"面板给多少、电池进出多少、主控吃多少"，
直接回答"太阳能补多少安才能循环"。

```
ch1  太阳能板 (+) ──► [1IN+ ] [ 1IN- ] ──► CN3791 VIN
ch2  电池 (+)     ──► [2IN+ ] [ 2IN- ] ──► CN3791 BAT / XIAO BAT+
ch3  CN3791 OUT   ──► [3IN+ ] [ 3IN- ] ──► 主控负载

所有 (−) / GND 共地，FireBeetle GND 一并接过来
FireBeetle 3V3 / 19-SDA / 20-SCL ──► INA3221 VCC / SDA / SCL
```

ch2 读数符号即净流向：**正 = 电池在放电，负 = 在充电**。
24 h 积分就是当天的净收支。

---

## 两块同时接？先改地址

INA226 和 INA3221 默认都是 **0x40**，同一条 I²C 总线会撞。任选其一：

- **分开测**（推荐）：一次只焊一块，接法 A 和 B 本来也是两个不同的实验
- **改地址**：模块上 A0 焊盘接 VS(VCC) → 变成 0x41，然后改
  `tools/ina-sampler/main/ina_sampler.c` 的 `INA_ADDR`

固件靠 die ID 自动识别是 226 还是 3221（0x2260 / 0x3220），不用手工选型号。

---

## 烧录与运行

```bash
source ./env.sh
cd tools/ina-sampler
idf.py -p /dev/cu.usbmodemXXXX flash        # XXXX = FireBeetle 的口，别烧错到主控上
cd ../..
python3 tools/ina_log.py /dev/cu.usbmodemXXXX logs/idle-YYYY-MM-DD.csv
```

`ina_log.py` 实时打印滚动统计，Ctrl-C 结束时给整段平均和 24 h 耗电估算：

```
20:41:03  当前   1.842 mA   60s 均   1.907 mA  谷  0.412  峰   38.20  睡眠占比  94%   vbus 4.055 V
```

CSV 存下来的是固件原始行加一列本机时间戳，方便和 diag log、充电曲线对齐。

---

## 怎么读结果

`睡眠占比` = 一秒内电流低于 5 mA 的采样点比例，直接回答 **light sleep 到底进没进**：

| 现象 | 结论 |
|---|---|
| 均值 ~15 mA，睡眠占比 ≈ 0%，波形平坦 | **没睡**。radio 一直醒着，先查 ICD / PM 配置 |
| 均值 1–3 mA，睡眠占比 > 90%，每 5 s 一个几十 mA 的窄峰 | LIT 正常，峰就是 poll。降 poll 频率还能再省 |
| 均值 < 1 mA | 已经很好，续航瓶颈在 DRV8833 静态 1.6 mA 和面板 |

峰值几百 mA 且持续几十秒 = 电机在跑，测 idle 时应排除这段。

固件默认 AVG=1、转换时间 332 µs（约 1.5 kSPS），故意**不让 INA 芯片自己平均**——
要看清 light sleep 的窄唤醒脉冲，平均会把它抹掉。
