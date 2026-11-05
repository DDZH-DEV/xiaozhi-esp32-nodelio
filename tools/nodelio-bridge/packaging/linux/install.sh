#!/bin/sh
set -eu
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
destination="$HOME/.local/bin/nodelio-bridge"
mkdir -p "$HOME/.local/bin"
if [ -e "$destination" ]; then
  printf '%s\n' '目标程序已存在。请先退出旧桥，将旧文件移走作为回退版本，再安装。' >&2
  exit 1
fi
install -m 755 "$source_dir/nodelio-bridge" "$destination"
printf '%s\n' "已安装：$destination" '桌面启动：nodelio-bridge serve --open' '无桌面启动：nodelio-bridge serve' '登录自启动：nodelio-bridge autostart-enable'
