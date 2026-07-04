# ESP Matter Window — Claude 注意事项

## 构建 / 烧录命令

在运行任何 `idf.py` 命令之前，必须先激活工具链环境：

```bash
source ./env.sh
```

该脚本会依次 source ESP-IDF 和 esp-matter 的 `export.sh`（SDK 路径可用 `IDF_PATH` / `ESP_MATTER_PATH` 环境变量覆盖，默认 `$HOME/esp/` 下，见 README「环境准备」）。之后才能正常执行：

```bash
idf.py build
idf.py flash monitor
```

## 硬件设计要点

- **霍尔传感器**：只有一个（GPIO23/D5），但**两端各放一块磁铁**。窗运动到全开或全关时，同一传感器都会触发（低电平有效）。
- **端点检测**：全开和全关均靠霍尔传感器触发，不依赖行程时间（`travel_ms`）。
- **`travel_ms`** 仅用于中途百分比停靠（GoToLiftPercentage）的位置估算，以及 Matter 百分比上报，不用于端点判断。
- **LEAVING 状态**：从全开或全关出发时，先进入 `LEAVING_OPEN`/`LEAVING_CLOSED` 状态，等待磁铁离开传感器（`hall=false`）后，才切换到 `OPENING`/`CLOSING` 正常运动状态，防止霍尔传感器误触发。
