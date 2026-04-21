#!/usr/bin/env python3
"""Package a firmware.bin as a signed SD-library artifact.

Produces:
  <outdir>/<name>-<tag>-<version>.signed.bin   (firmware + 32-byte HMAC sig)
  <outdir>/<name>-<tag>-<version>.yaml         (human-facing metadata)

Usage:
  python3 scripts/package_ota.py firmware.bin \
      --name camera_ota --tag debug --version b5e7bc6 \
      --out /tmp/helios-bins/staged/firmwares/

The HMAC key is the same one gen_ota_key.py produced
(~/.helios/ota_sign.key). Same signing algorithm as sign_ota.py.
"""

import argparse
import datetime
import hashlib
import hmac
import pathlib
import sys

KEY = pathlib.Path.home() / ".helios" / "ota_sign.key"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=pathlib.Path, help="unsigned firmware.bin")
    ap.add_argument("--name", required=True, help="e.g. camera_ota")
    ap.add_argument("--tag", required=True,
                    choices=["debug", "experimental", "prod"])
    ap.add_argument("--version", required=True,
                    help="e.g. b5e7bc6 or v1.0.0")
    ap.add_argument("--out", type=pathlib.Path, required=True,
                    help="output directory")
    ap.add_argument("--notes", default="", help="freeform notes for YAML")
    ap.add_argument("--key", type=pathlib.Path, default=KEY)
    args = ap.parse_args()

    if not args.key.exists():
        print(f"no signing key at {args.key}", file=sys.stderr)
        return 2
    key_bytes = bytes.fromhex(args.key.read_text().strip())

    data = args.input.read_bytes()
    tag_bytes = hmac.new(key_bytes, data, hashlib.sha256).digest()
    signed = data + tag_bytes

    stem = f"{args.name}-{args.tag}-{args.version}"
    args.out.mkdir(parents=True, exist_ok=True)
    bin_path = args.out / f"{stem}.signed.bin"
    yml_path = args.out / f"{stem}.yaml"

    bin_path.write_bytes(signed)

    sha = hashlib.sha256(data).hexdigest()
    yaml = (
        f"name: {args.name}\n"
        f"tag: {args.tag}\n"
        f"version: {args.version}\n"
        f"built: {datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')}\n"
        f"size_bytes: {len(data)}\n"
        f"signed_size_bytes: {len(signed)}\n"
        f"sha256: {sha}\n"
        f"hmac_sha256: {tag_bytes.hex()}\n"
    )
    if args.notes:
        yaml += f"notes: |\n  " + args.notes.replace("\n", "\n  ") + "\n"
    yml_path.write_text(yaml)

    print(f"wrote {bin_path}  ({len(signed)} bytes)")
    print(f"wrote {yml_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
