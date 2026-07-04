#!/bin/bash
# idf.sh — 自动初始化工具链后执行 idf.py
# 用法：./idf.sh build / flash / flash monitor 等
# 路径配置见 env.sh（可用 IDF_PATH / ESP_MATTER_PATH 环境变量覆盖）
set -e
source "$(dirname "$0")/env.sh"
exec idf.py "$@"
