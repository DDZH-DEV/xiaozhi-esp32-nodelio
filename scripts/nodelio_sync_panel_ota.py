#!/usr/bin/env python3
"""把当前构建号与 xiaozhi.bin 写入 Z-Y 面板 OTA 清单。

清单键（ota_key）：
  sila_86
  sila_p4c5_62_32m / sila_p4c5_62_16m
  sila_p4c5_508_16m / sila_p4c5_508_32m

兼容旧固件：默认变体发版时同步写无后缀 alias
  sila_p4c5_62  ← 62_32m
  sila_p4c5_508 ← 508_16m

默认 --channel dev；--channel stable 写 firmware + xiaozhi-latest.bin。
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

# ota_key → chip / CDN 子目录
BOARDS = {
    "sila_86": {
        "chip": "esp32s3",
        "bin_dir": Path("iot-firmwares") / "esp32s3-sila-86",
        "cdn": "https://cdn-file.z-y.site/works/iot-firmwares/esp32s3-sila-86",
        "product_board": "sila_86",
        "variant": "none",
        "legacy_alias": None,
    },
    "sila_p4c5_62_32m": {
        "chip": "esp32p4",
        "bin_dir": Path("iot-firmwares") / "esp32p4-sila-p4c5-62-32m",
        "cdn": "https://cdn-file.z-y.site/works/iot-firmwares/esp32p4-sila-p4c5-62-32m",
        "product_board": "sila_p4c5_62",
        "variant": "32m",
        "legacy_alias": "sila_p4c5_62",
    },
    "sila_p4c5_62_16m": {
        "chip": "esp32p4",
        "bin_dir": Path("iot-firmwares") / "esp32p4-sila-p4c5-62-16m",
        "cdn": "https://cdn-file.z-y.site/works/iot-firmwares/esp32p4-sila-p4c5-62-16m",
        "product_board": "sila_p4c5_62",
        "variant": "16m",
        "legacy_alias": None,
    },
    "sila_p4c5_508_16m": {
        "chip": "esp32p4",
        "bin_dir": Path("iot-firmwares") / "esp32p4-sila-p4c5-508-16m",
        "cdn": "https://cdn-file.z-y.site/works/iot-firmwares/esp32p4-sila-p4c5-508-16m",
        "product_board": "sila_p4c5_508",
        "variant": "16m",
        "legacy_alias": "sila_p4c5_508",
    },
    "sila_p4c5_508_32m": {
        "chip": "esp32p4",
        "bin_dir": Path("iot-firmwares") / "esp32p4-sila-p4c5-508-32m",
        "cdn": "https://cdn-file.z-y.site/works/iot-firmwares/esp32p4-sila-p4c5-508-32m",
        "product_board": "sila_p4c5_508",
        "variant": "32m",
        "legacy_alias": None,
    },
}

PRODUCT_BOARDS = ("sila_86", "sila_p4c5_62", "sila_p4c5_508")
DEFAULT_VARIANT = {"sila_p4c5_62": "32m", "sila_p4c5_508": "16m"}

NOTES = (
    "设备读 boards[ota_key]。ota_key=board_id（86）或 board_id_16m|32m（P4）。"
    "firmware=稳定（xiaozhi-latest.bin）；dev=开发带号包。"
    "默认变体 alias：sila_p4c5_62→62_32m，sila_p4c5_508→508_16m（旧固件）。"
    "禁止跨板/跨变体互刷。首烧换容量须整包含分区表。"
)

CHIP_IMAGE_ID = {
    "esp32s3": 0x0009,
    "esp32p4": 0x0012,
}


def ota_key_for(product_board: str, variant: str) -> str:
    if product_board == "sila_86":
        return "sila_86"
    return f"{product_board}_{variant}"


def read_image_chip(path: Path) -> int:
    hdr = path.read_bytes()[:14]
    if len(hdr) < 14 or hdr[0] != 0xE9:
        raise SystemExit(f"[panel-ota] {path} is not an ESP app image")
    return int.from_bytes(hdr[12:14], "little")


def read_stamp(header: Path) -> str:
    text = header.read_text(encoding="utf-8")
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("#define NODELIO_BUILD_STAMP "):
            return line.split('"', 2)[1]
    raise SystemExit(f"NODELIO_BUILD_STAMP not found in {header}")


def empty_fw() -> dict:
    return {"version": "0.0.0", "url": "", "force": 0}


def empty_entry(chip: str) -> dict:
    return {"chip": chip, "firmware": empty_fw(), "dev": empty_fw()}


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
    for ota_key, spec in BOARDS.items():
        entry = boards.get(ota_key)
        if not isinstance(entry, dict) or not isinstance(entry.get("firmware"), dict):
            boards[ota_key] = empty_entry(spec["chip"])
        else:
            entry["chip"] = spec["chip"]
            if not isinstance(entry.get("dev"), dict):
                entry["dev"] = empty_fw()
    # ensure legacy alias shells exist
    for alias, chip in (
        ("sila_p4c5_62", "esp32p4"),
        ("sila_p4c5_508", "esp32p4"),
    ):
        entry = boards.get(alias)
        if not isinstance(entry, dict) or not isinstance(entry.get("firmware"), dict):
            boards[alias] = empty_entry(chip)
        else:
            entry["chip"] = chip
            if not isinstance(entry.get("dev"), dict):
                entry["dev"] = empty_fw()
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
    ap.add_argument("--board", choices=PRODUCT_BOARDS, default="sila_86")
    ap.add_argument(
        "--variant",
        choices=("none", "16m", "32m"),
        default=None,
        help="P4 required: 16m|32m；86 用 none",
    )
    ap.add_argument(
        "--channel",
        choices=("dev", "stable"),
        default="dev",
        help="dev=只发带号包；stable=写 latest + firmware",
    )
    ap.add_argument("--skip-bin", action="store_true", help="only refresh json")
    args = ap.parse_args()

    if args.board == "sila_86":
        variant = args.variant or "none"
        if variant not in ("none",):
            print("[panel-ota] sila_86 ignores flash variant; using none", file=sys.stderr)
            variant = "none"
    else:
        if args.variant not in ("16m", "32m"):
            print("[panel-ota] ERROR: P4 requires --variant 16m|32m", file=sys.stderr)
            return 1
        variant = args.variant

    ota_key = ota_key_for(args.board, variant)
    if ota_key not in BOARDS:
        print(f"[panel-ota] ERROR: unknown ota_key {ota_key}", file=sys.stderr)
        return 1
    spec = BOARDS[ota_key]

    root: Path = args.root.resolve()
    header = root / "main" / "nodelio" / "nodelio_build_stamp.h"
    if not header.is_file():
        print("[panel-ota] missing stamp header — run nodelio_build_stamp.py first", file=sys.stderr)
        return 1
    stamp = read_stamp(header)
    expect = CHIP_IMAGE_ID[spec["chip"]]

    stamped_name = f"xiaozhi-{stamp}.bin"
    stamped_url = f"{spec['cdn']}/{stamped_name}"
    latest_url = f"{spec['cdn']}/xiaozhi-latest.bin"
    src = root / "build" / "xiaozhi.bin"
    if not args.skip_bin:
        if not src.is_file():
            print(f"[panel-ota] missing {src}; manifest not updated", file=sys.stderr)
            return 1
        chip = read_image_chip(src)
        if chip != expect:
            print(
                f"[panel-ota] {src} chip_id=0x{chip:04x}, {ota_key} needs 0x{expect:04x}; "
                "manifest not updated",
                file=sys.stderr,
            )
            return 1

    zy: Path = args.zy_works.resolve()
    zy.mkdir(parents=True, exist_ok=True)
    bin_dir = zy / spec["bin_dir"]
    bin_dir.mkdir(parents=True, exist_ok=True)
    if not args.skip_bin:
        dst = bin_dir / stamped_name
        shutil.copy2(src, dst)
        print(f"[panel-ota] copied {src} -> {dst}")
        if args.channel == "stable":
            latest = bin_dir / "xiaozhi-latest.bin"
            shutil.copy2(src, latest)
            print(f"[panel-ota] copied {src} -> {latest}")

    fw_payload = {"version": stamp, "url": stamped_url, "force": 0}
    stable_payload = {"version": stamp, "url": latest_url, "force": 0}

    manifest = load_manifest(zy / MANIFEST_NAME)
    entry = manifest["boards"][ota_key]
    entry["dev"] = dict(fw_payload)
    if args.channel == "stable":
        entry["firmware"] = dict(stable_payload)

    alias = spec.get("legacy_alias")
    if alias:
        # 默认变体：同步 alias，供旧固件只认 board_id
        alias_entry = manifest["boards"].setdefault(alias, empty_entry(spec["chip"]))
        alias_entry["chip"] = spec["chip"]
        alias_entry["dev"] = dict(fw_payload)
        if args.channel == "stable":
            alias_entry["firmware"] = dict(stable_payload)
        print(f"[panel-ota] legacy alias boards.{alias} <- {ota_key}")

    man_path = zy / MANIFEST_NAME
    man_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"[panel-ota] wrote {man_path}")
    print(f"[panel-ota] ota_key={ota_key} channel={args.channel} version={stamp}")
    print(f"[panel-ota] bin_dir={bin_dir}")
    print(f"[panel-ota] dev.url={stamped_url}")
    if args.channel == "stable":
        print(f"[panel-ota] firmware.url={latest_url}")
    else:
        print("[panel-ota] firmware 未改；要发用户包请加 --channel stable")

    if args.board == "sila_86" and args.channel == "stable":
        legacy = {
            "board": "SILA-86",
            "chip": "esp32s3",
            "firmware": dict(stable_payload),
            "notes": "旧固件专用（仅 stable）。新固件请用 sila-panel-update.json。",
        }
        legacy_path = zy / LEGACY_86_NAME
        legacy_path.write_text(json.dumps(legacy, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"[panel-ota] wrote legacy {legacy_path}")

    if not args.skip_bin:
        print("[panel-ota] 请用 nodelio_upload_panel_ota.bat 上传清单与 bin")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
