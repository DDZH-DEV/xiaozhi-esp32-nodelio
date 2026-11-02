"""Re-apply the EspTcp disconnect fix inside managed 78/esp-ml307.

idf.py fullclean refuses a managed component whose files differ from
CHECKSUMS.json. Copy the patched source and refresh that checksum so
target switches can clean, then the next configure puts the fix back.
"""

import hashlib
import json
import sys
from pathlib import Path

REL = Path("src/esp/esp_tcp.cc")


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    src = root / "patches" / "78__esp-ml307" / REL
    dest_dir = root / "managed_components" / "78__esp-ml307"
    dest = dest_dir / REL
    checksums_path = dest_dir / "CHECKSUMS.json"
    if not dest_dir.is_dir() or not checksums_path.is_file():
        return 0
    data = src.read_bytes()
    if not dest.is_file() or dest.read_bytes() != data:
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        print("[esp-ml307] applied EspTcp disconnect patch")
    digest = hashlib.sha256(data).hexdigest()
    payload = json.loads(checksums_path.read_text(encoding="utf-8"))
    changed = False
    found = False
    for item in payload["files"]:
        if item["path"] != REL.as_posix():
            continue
        found = True
        if item.get("hash") != digest or item.get("size") != len(data):
            item["hash"] = digest
            item["size"] = len(data)
            changed = True
        break
    if not found:
        print("CHECKSUMS.json missing src/esp/esp_tcp.cc", file=sys.stderr)
        return 1
    if changed:
        checksums_path.write_text(
            json.dumps(payload, separators=(",", ":"), ensure_ascii=False),
            encoding="utf-8",
        )
        print("[esp-ml307] refreshed CHECKSUMS.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
