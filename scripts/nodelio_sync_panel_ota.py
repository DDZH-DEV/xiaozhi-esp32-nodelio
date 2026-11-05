#!/usr/bin/env python3
"""把当前构建号与 xiaozhi.bin 写入 Z-Y 面板 OTA 清单里对应的 board_id。

清单：D:/SITES/Z-Y/public/works/sila-panel-update.json
  boards.sila_86        …/iot-firmwares/esp32s3-sila-86/xiaozhi-{stamp}.bin
  boards.sila_p4c5_62   …/iot-firmwares/esp32p4-sila-p4c5-62/xiaozhi-{stamp}.bin

只改本次 --board 的条目，另一块板的 url / version 保留。
sila_86 另写一份旧文件 esp32s3-slia-86-update.json，给定居旧固件。
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

DEFAULT_ZY = Path(r"D:\SITES\Z-Y\public\works")
MANIFEST_NAME = "sila-panel-update.json"
LEGACY_86_NAME = "esp32s3-slia-86-update.json"

BOARDS = {
    "sila_86": {
        "chip": "esp32s3",
        "bin_dir": Path("iot-firmwares") / "esp32s3-sila-86",
        "cdn": "https://cdn-file.z-y.site/works/iot-firmwares/esp32s3-sila-86",
    },
    "sila_p4c5_62": {
        "chip": "esp32p4",
        "bin_dir": Path("iot-firmwares") / "esp32p4-sila-p4c5-62",
        "cdn": "https://cdn-file.z-y.site/works/iot-firmwares/esp32p4-sila-p4c5-62",
    },
}

NOTES = (
    "设备只读 boards[board_id]（sila_86 / sila_p4c5_62）。"
    "version 为构建号 YYYYMMDD.NNN；url 是 ota_0 用的 xiaozhi.bin，禁止两板互刷。"
    "sila_p4c5_62 对应 32m 变体；16m 槽更小，不要拿 32m 包去刷。"
    "P4 包未上传时 url 留空。"
    "已出厂的 86 旧固件仍请求 esp32s3-slia-86-update.json。"
)


def read_stamp(header: Path) -> str:
    text = header.read_text(encoding="utf-8")
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("#define NODELIO_BUILD_STAMP "):
            return line.split('"', 2)[1]
    raise SystemExit(f"NODELIO_BUILD_STAMP not found in {header}")


def empty_entry(chip: str) -> dict:
    return {"chip": chip, "firmware": {"version": "0.0.0", "url": "", "force": 0}}


def load_manifest(path: Path) -> dict:
    if path.is_file():
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            data = {}
    else:
        data = {}
    boards = data.get("boards")
    if not isinstance(boards, dict):
        boards = {}
        legacy = data.get("firmware")
        if isinstance(legacy, dict) and data.get("board") == "SILA-86":
            boards["sila_86"] = {"chip": "esp32s3", "firmware": legacy}
    for board_id, spec in BOARDS.items():
        entry = boards.get(board_id)
        if not isinstance(entry, dict) or not isinstance(entry.get("firmware"), dict):
            boards[board_id] = empty_entry(spec["chip"])
        else:
            entry["chip"] = spec["chip"]
    data["boards"] = boards
    data.pop("board", None)
    data.pop("chip", None)
    data.pop("firmware", None)
    data["notes"] = NOTES
    return data


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument("--zy-works", type=Path, default=DEFAULT_ZY)
    ap.add_argument("--board", choices=sorted(BOARDS), default="sila_86")
    ap.add_argument("--skip-bin", action="store_true", help="only refresh json")
    args = ap.parse_args()

    root: Path = args.root.resolve()
    header = root / "main" / "nodelio" / "nodelio_build_stamp.h"
    if not header.is_file():
        print("[panel-ota] missing stamp header — run nodelio_build_stamp.py first", file=sys.stderr)
        return 1
    stamp = read_stamp(header)
    spec = BOARDS[args.board]

    zy: Path = args.zy_works.resolve()
    zy.mkdir(parents=True, exist_ok=True)
    bin_dir = zy / spec["bin_dir"]
    bin_dir.mkdir(parents=True, exist_ok=True)

    bin_name = f"xiaozhi-{stamp}.bin"
    url = f"{spec['cdn']}/{bin_name}"
    manifest = load_manifest(zy / MANIFEST_NAME)
    manifest["boards"][args.board]["firmware"] = {"version": stamp, "url": url, "force": 0}

    man_path = zy / MANIFEST_NAME
    man_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"[panel-ota] wrote {man_path}")
    print(f"[panel-ota] board={args.board} version={stamp}")
    print(f"[panel-ota] url={url}")

    if args.board == "sila_86":
        legacy = {
            "board": "SILA-86",
            "chip": "esp32s3",
            "firmware": {"version": stamp, "url": url, "force": 0},
            "notes": "旧固件专用。新固件请用 sila-panel-update.json，按 board_id 取包。",
        }
        legacy_path = zy / LEGACY_86_NAME
        legacy_path.write_text(json.dumps(legacy, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"[panel-ota] wrote legacy {legacy_path}")

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
