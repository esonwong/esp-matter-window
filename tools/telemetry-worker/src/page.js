// 项目介绍页（GET /，无需鉴权）
export const INDEX_HTML = `<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>matter-window · 太阳能 Matter 开窗器</title>
<meta name="description" content="ESP32-C6 + Thread + Matter 的太阳能自供电开窗器，接入 Apple 家庭，开源硬件项目">
<style>
:root {
  --bg: #fafaf8; --fg: #1c1c1a; --muted: #6b6b66; --card: #ffffff;
  --line: #e6e4df; --accent: #2c6e49; --accent-soft: #e8f2ec;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #16181a; --fg: #e8e6e1; --muted: #9a978f; --card: #1f2225;
    --line: #33373b; --accent: #6fbf8f; --accent-soft: #21302a;
  }
}
* { margin: 0; box-sizing: border-box; }
body {
  background: var(--bg); color: var(--fg);
  font-family: -apple-system, "PingFang SC", "Noto Sans SC", sans-serif;
  line-height: 1.7;
}
main { max-width: 720px; margin: 0 auto; padding: 3rem 1.25rem 4rem; }
h1 { font-size: 2rem; letter-spacing: -0.02em; }
.tagline { color: var(--muted); margin: .5rem 0 2rem; }
.badges { display: flex; gap: .5rem; flex-wrap: wrap; margin-bottom: 2.5rem; }
.badge {
  font-size: .8rem; padding: .2rem .6rem; border-radius: 99px;
  background: var(--accent-soft); color: var(--accent); font-weight: 500;
}
h2 { font-size: 1.15rem; margin: 2.2rem 0 .8rem; }
p { margin: .6rem 0; }
.stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: .75rem; margin: 1rem 0; }
.stat { background: var(--card); border: 1px solid var(--line); border-radius: 10px; padding: .9rem 1rem; }
.stat b { display: block; font-size: 1.35rem; color: var(--accent); }
.stat span { font-size: .82rem; color: var(--muted); }
ul { padding-left: 1.2rem; }
li { margin: .35rem 0; }
li::marker { color: var(--accent); }
.flow {
  background: var(--card); border: 1px solid var(--line); border-radius: 10px;
  padding: 1rem 1.2rem; overflow-x: auto; font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
  font-size: .82rem; white-space: pre; color: var(--muted);
}
a { color: var(--accent); }
.cta {
  display: inline-block; margin-top: 1.5rem; padding: .6rem 1.3rem;
  background: var(--accent); color: #fff; border-radius: 8px; text-decoration: none; font-weight: 500;
}
footer { margin-top: 3rem; padding-top: 1.5rem; border-top: 1px solid var(--line); color: var(--muted); font-size: .85rem; }
</style>
</head>
<body>
<main>
  <h1>matter-window</h1>
  <p class="tagline">太阳能自供电的 Matter 开窗器 —— 装上就不用管，Siri 一句话开窗</p>
  <div class="badges">
    <span class="badge">ESP32-C6</span><span class="badge">Thread</span>
    <span class="badge">Matter</span><span class="badge">Apple 家庭</span>
    <span class="badge">太阳能</span><span class="badge">MIT 开源</span>
  </div>

  <div class="stats">
    <div class="stat"><b>~4 mA</b><span>idle 平均电流（ICD LIT + light sleep）</span></div>
    <div class="stat"><b>自给自足</b><span>小太阳能板阴天也够补电</span></div>
    <div class="stat"><b>0 根网线</b><span>Thread 入网，日志与 OTA 也走 Thread</span></div>
  </div>

  <h2>它做了什么</h2>
  <ul>
    <li><b>Matter Window Covering</b>：Apple 家庭里原生显示为窗户，支持百分比开合、Siri 控制、自动化</li>
    <li><b>单霍尔 + 双磁铁端点检测</b>：一个传感器搞定全开/全关定位，不靠行程计时</li>
    <li><b>电池 + 太阳能</b>：CN3791 MPPT 直充，低电量自动 deep sleep 保护电池</li>
    <li><b>远程运维</b>：diag 日志每小时经 Thread → 边界路由器上报 Cloudflare，OTA 升级同路返回</li>
  </ul>

  <h2>数据怎么走</h2>
  <div class="flow">窗控器 (Thread SED)
   ├─ Matter ──► HomePod ──► Apple 家庭 / Siri
   └─ HTTPS ──► HomePod (NAT64) ──► Cloudflare Worker
                                      ├─ D1：diag 日志 / 电池曲线
                                      └─ R2：OTA 固件</div>

  <h2>硬件</h2>
  <ul>
    <li>Seeed XIAO ESP32-C6（Thread + BLE 配网）</li>
    <li>DRV8833 电机驱动 + N20 减速电机</li>
    <li>霍尔传感器 ×1、磁铁 ×2（两端各一）</li>
    <li>CN3791 太阳能充电 + 单节锂电</li>
  </ul>

  <a class="cta" href="https://github.com/esonwong/esp-matter-window">GitHub 上看源码与全部踩坑记录 →</a>

  <footer>MIT License · 由 <a href="https://blog.esonwong.com">Eson</a> 手搓，固件/硬件/功耗测试全过程开源</footer>
</main>
</body>
</html>`;
