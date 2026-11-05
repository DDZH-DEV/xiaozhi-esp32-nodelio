#!/bin/sh
set -eu
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
destination="$HOME/Applications/NodeLio Bridge.app"
mkdir -p "$HOME/Applications"
if [ -e "$destination" ]; then
  printf '%s\n' '目标应用已存在。请先退出旧桥，将旧应用移走作为回退版本，再安装。' >&2
  exit 1
fi
cp -R "$source_dir/NodeLio Bridge.app" "$destination"
open "$destination"
printf '%s\n' '已安装到 ~/Applications。登录自启动请在配置页选择。'
