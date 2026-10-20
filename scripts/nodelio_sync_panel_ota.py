#!/usr/bin/env python3
"""把当前构建号与 xiaozhi.bin 同步到 Z-Y 面板 OTA 清单（用户再上传到站点）。

清单：D:/SITES/Z-Y/public/works/esp32s3-slia-86-update.json
固件：…/iot-firmwares/esp32s3-sila-86/xiaozhi-{stamp}.bin
CDN： https://cdn-file.z-y.site/works/iot-firmwares/esp32s3-sila-86/xiaozhi-{stamp}.bin
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

DEFAULT_ZY = Path(r"D:\SITES\Z-Y\public\works")
MANIFEST_NAME = "esp32s3-slia-86-update.json"
BIN_DIR_REL = Path("iot-firmwares") / "esp32s3-sila-86"
CDN_PREFIX = "https://cdn-file.z-y.site/works/iot-firmwares/esp32s3-sila-86"


def read_stamp(header: Path) -> str:
    text = header.read_text(encoding="utf-8")
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("#define NODELIO_BUILD_STAMP "):
            return line.split('"', 2)[1]
    raise SystemExit(f"NODELIO_BUILD_STAMP not found in {header}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument("--zy-works", type=Path, default=DEFAULT_ZY)
    ap.add_argument("--skip-bin", action="store_true", help="only refresh json")
    args = ap.parse_args()

    root: Path = args.root.resolve()
    header = root / "main" / "nodelio" / "nodelio_build_stamp.h"
    if not header.is_file():
        print("[panel-ota] missing stamp header — run nodelio_build_stamp.py first", file=sys.stderr)
        return 1
    stamp = read_stamp(header)

    zy: Path = args.zy_works.resolve()
    zy.mkdir(parents=True, exist_ok=True)
    bin_dir = zy / BIN_DIR_REL
    bin_dir.mkdir(parents=True, exist_ok=True)

    bin_name = f"xiaozhi-{stamp}.bin"
    url = f"{CDN_PREFIX}/{bin_name}"
    manifest = {
        "board": "SILA-86",
        "chip": "esp32s3",
        "firmware": {
            "version": stamp,
            "url": url,
            "force": 0,
        },
        "notes": "version = 墙板构建号 YYYYMMDD.NNN；url 为 ota_0 用的 xiaozhi.bin（非整片 flash）",
    }
    man_path = zy / MANIFEST_NAME
    man_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"[panel-ota] wrote {man_path}")
    print(f"[panel-ota] version={stamp}")
    print(f"[panel-ota] url={url}")

    if not args.skip_bin:
        src = root / "build" / "xiaozhi.bin"
        if src.is_file():
            dst = bin_dir / bin_name
            shutil.copy2(src, dst)
            latest = bin_dir / "xiaozhi-latest.bin"
            shutil.copy2(src, latest)
            print(f"[panel-ota] copied {src} -> {dst}")
            print(f"[panel-ota] copied {src} -> {latest}")
            print("[panel-ota] 请上传清单与 bin 到站点 / CDN 后，板上「检查更新」才会下到新包")
        else:
            print(f"[panel-ota] WARN no {src} yet — json updated; rebuild then re-run to copy bin")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
