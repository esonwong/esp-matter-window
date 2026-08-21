import { INDEX_HTML } from "./page.js";

// matter-window telemetry + OTA worker
// 认证：所有写入与读取都要 Bearer token（wrangler secret put API_TOKEN）
// 设备端点：
//   POST /ingest      设备上报：{device_id, fw, boot_id, uptime_s, vbat_mv, heap_kb, entries:[[uptime,type,aux1,vbat,pos,state,motor,btn,heap],...]}
//                     响应：{"ok":true, "ota":{"version","url","sha256"}|null}
//   GET  /fw/app.bin  固件下载（R2）
// 人看端点：
//   GET  /logs?hours=24&type=2   最近 diag（CSV）
//   GET  /reports?hours=48       心跳/vbat 曲线（CSV）
//   POST /fw/publish  {version, sha256}  设置当前要推的版本（bin 先传 R2）
//   POST /fw/clear    停止推送

const CSV_DIAG = "received_at,device_id,fw,boot_id,uptime_s,type,aux1,vbat_mv,position,state,motor_n,button_n,heap_kb";
const CSV_REPORT = "received_at,device_id,fw,boot_id,uptime_s,vbat_mv,heap_kb,rssi";

function unauthorized() { return new Response("unauthorized\n", { status: 401 }); }
function bad(msg) { return new Response(msg + "\n", { status: 400 }); }

async function auth(request, env) {
  const h = request.headers.get("Authorization") || "";
  return h === `Bearer ${env.API_TOKEN}`;
}

async function getOta(env) {
  const v = await env.DB.prepare("SELECT value FROM meta WHERE key='ota'").first();
  return v ? JSON.parse(v.value) : null;
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const p = url.pathname;

    // 公开首页：项目介绍
    if (request.method === "GET" && (p === "/" || p === "/index.html")) {
      return new Response(INDEX_HTML, { headers: { "Content-Type": "text/html; charset=utf-8" } });
    }

    if (!(await auth(request, env))) return unauthorized();

    if (request.method === "POST" && p === "/ingest") {
      let body;
      try { body = await request.json(); } catch { return bad("json"); }
      const { device_id, fw, boot_id, uptime_s, vbat_mv, heap_kb, rssi, entries } = body;
      if (!device_id) return bad("device_id");

      await env.DB.prepare(
        "INSERT INTO reports (device_id, fw, boot_id, uptime_s, vbat_mv, heap_kb, rssi) VALUES (?,?,?,?,?,?,?)"
      ).bind(device_id, fw ?? null, boot_id ?? null, uptime_s ?? null, vbat_mv ?? null, heap_kb ?? null, rssi ?? null).run();

      if (Array.isArray(entries) && entries.length) {
        const stmt = env.DB.prepare(
          "INSERT INTO diag (device_id, fw, boot_id, uptime_s, type, aux1, vbat_mv, position, state, motor_n, button_n, heap_kb) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)"
        );
        await env.DB.batch(entries.slice(0, 300).map(e =>
          stmt.bind(device_id, fw ?? null, boot_id ?? null, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8])
        ));
      }

      const ota = await getOta(env);
      // 设备已是目标版本则不推
      const push = ota && ota.version !== fw ? ota : null;
      return Response.json({ ok: true, ota: push });
    }

    if (request.method === "GET" && p === "/fw/app.bin") {
      const obj = await env.FW.get("app.bin");
      if (!obj) return new Response("no firmware\n", { status: 404 });
      return new Response(obj.body, { headers: { "Content-Type": "application/octet-stream", "Content-Length": obj.size } });
    }

    if (request.method === "POST" && p === "/fw/publish") {
      let body;
      try { body = await request.json(); } catch { return bad("json"); }
      if (!body.version || !body.sha256) return bad("version/sha256");
      const obj = await env.FW.head("app.bin");
      if (!obj) return bad("upload app.bin to R2 first");
      const ota = { version: body.version, sha256: body.sha256, size: obj.size, url: url.origin + "/fw/app.bin" };
      await env.DB.prepare("INSERT INTO meta (key, value) VALUES ('ota', ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value")
        .bind(JSON.stringify(ota)).run();
      return Response.json({ ok: true, ota });
    }

    if (request.method === "POST" && p === "/fw/clear") {
      await env.DB.prepare("DELETE FROM meta WHERE key='ota'").run();
      return Response.json({ ok: true });
    }

    if (request.method === "GET" && (p === "/logs" || p === "/reports")) {
      const hours = Math.min(parseInt(url.searchParams.get("hours") || "24"), 24 * 30);
      const type = url.searchParams.get("type");
      let rows;
      if (p === "/logs") {
        const q = type
          ? env.DB.prepare("SELECT * FROM diag WHERE received_at > datetime('now', ?) AND type = ? ORDER BY received_at").bind(`-${hours} hours`, type)
          : env.DB.prepare("SELECT * FROM diag WHERE received_at > datetime('now', ?) ORDER BY received_at").bind(`-${hours} hours`);
        rows = (await q.all()).results.map(r =>
          [r.received_at, r.device_id, r.fw, r.boot_id, r.uptime_s, r.type, r.aux1, r.vbat_mv, r.position, r.state, r.motor_n, r.button_n, r.heap_kb].join(","));
        rows.unshift(CSV_DIAG);
      } else {
        const q = env.DB.prepare("SELECT * FROM reports WHERE received_at > datetime('now', ?) ORDER BY received_at").bind(`-${hours} hours`);
        rows = (await q.all()).results.map(r =>
          [r.received_at, r.device_id, r.fw, r.boot_id, r.uptime_s, r.vbat_mv, r.heap_kb, r.rssi].join(","));
        rows.unshift(CSV_REPORT);
      }
      return new Response(rows.join("\n") + "\n", { headers: { "Content-Type": "text/plain; charset=utf-8" } });
    }

    return new Response("not found\n", { status: 404 });
  },
};
