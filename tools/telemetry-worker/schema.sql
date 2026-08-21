CREATE TABLE IF NOT EXISTS diag (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  received_at TEXT NOT NULL DEFAULT (datetime('now')),
  device_id TEXT NOT NULL,
  fw TEXT,
  boot_id INTEGER,          -- 设备端本次开机的随机标识，区分不同开机周期
  uptime_s INTEGER,         -- 事件发生时的设备开机秒数
  type INTEGER,             -- diag_type_t
  aux1 INTEGER,
  vbat_mv INTEGER,
  position INTEGER,
  state INTEGER,
  motor_n INTEGER,
  button_n INTEGER,
  heap_kb INTEGER
);
CREATE INDEX IF NOT EXISTS idx_diag_device_time ON diag(device_id, received_at);
CREATE TABLE IF NOT EXISTS reports (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  received_at TEXT NOT NULL DEFAULT (datetime('now')),
  device_id TEXT NOT NULL,
  fw TEXT,
  boot_id INTEGER,
  uptime_s INTEGER,
  vbat_mv INTEGER,
  heap_kb INTEGER,
  rssi INTEGER
);
CREATE INDEX IF NOT EXISTS idx_reports_device_time ON reports(device_id, received_at);
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
