#!/bin/sh
# 激活 ESP-IDF + esp-matter 工具链环境
# 用法：source env.sh
#
# 通过环境变量指定 SDK 安装位置（未设置时使用 $HOME/esp 下的惯例路径）：
#   IDF_PATH         ESP-IDF 安装目录（默认 $HOME/esp/esp-idf）
#   ESP_MATTER_PATH  esp-matter 安装目录（默认 $HOME/esp/esp-matter）
#
# 安装步骤见 README.md「环境准备」一节。

_IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
_ESP_MATTER_PATH="${ESP_MATTER_PATH:-$HOME/esp/esp-matter}"

if [ ! -f "$_IDF_PATH/export.sh" ]; then
    echo "错误：找不到 ESP-IDF（$_IDF_PATH/export.sh 不存在）。" >&2
    echo "请安装 ESP-IDF v5.5.2，或用 IDF_PATH 指向已安装目录后重新 source。" >&2
    echo "安装步骤见 README.md「环境准备」。" >&2
    return 1 2>/dev/null || exit 1
fi

if [ ! -f "$_ESP_MATTER_PATH/export.sh" ]; then
    echo "错误：找不到 esp-matter（$_ESP_MATTER_PATH/export.sh 不存在）。" >&2
    echo "请安装 esp-matter，或用 ESP_MATTER_PATH 指向已安装目录后重新 source。" >&2
    echo "安装步骤见 README.md「环境准备」。" >&2
    return 1 2>/dev/null || exit 1
fi

. "$_IDF_PATH/export.sh"
export ESP_MATTER_PATH="$_ESP_MATTER_PATH"
. "$ESP_MATTER_PATH/export.sh"

unset _IDF_PATH _ESP_MATTER_PATH
