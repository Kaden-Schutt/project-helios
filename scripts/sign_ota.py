#!/usr/bin/env python3
"""Sign a firmware.bin with HMAC-SHA256 for OTA upload / SD recovery.

Produces   <input>.signed.bin   = <input> + 32-byte HMAC-SHA256 tag.

Usage:
  python3 scripts/sign_ota.py /tmp/helios-bins/camota/firmware.bin
  python3 scripts/sign_ota.py <in> --out <out>
"""

import argparse
import hashlib
import hmac
import pathlib
import sys

PRIV = pathlib.Path.home() / ".helios" / "ota_sign.key"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=pathlib.Path)
    ap.add_argument("--out", type=pathlib.Path, default=None)
    ap.add_argument("--key", type=pathlib.Path, default=PRIV)
    args = ap.parse_args()

    if not args.key.exists():
        print(f"no signing key at {args.key} — run gen_ota_key.py first", file=sys.stderr)
        return 2

    key = bytes.fromhex(args.key.read_text().strip())
    if len(key) != 32:
        print(f"bad key length at {args.key}: {len(key)}", file=sys.stderr)
        return 2

    data = args.input.read_bytes()
    tag = hmac.new(key, data, hashlib.sha256).digest()
    assert len(tag) == 32

    if args.out:
        out = args.out
    else:
        out = args.input.with_name(args.input.stem + ".signed.bin")
    out.write_bytes(data + tag)
    print(f"wrote {out}  ({len(data)} bytes + 32-byte sig = {len(data)+32} total)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
