"""
Helios — Image Downscale Latency Test
======================================
Sends the same query at multiple image resolutions to measure
how image size affects end-to-end and per-step latency.

Uses the test harness transcript bypass (no mic needed).
"""

import base64
import io
import time
import requests
from PIL import Image

SERVER = "http://localhost:5750"
QUERY_URL = f"{SERVER}/query"
CLEAR_URL = f"{SERVER}/clear"

IMAGE_PATH = "input/Image.jpeg"
TRANSCRIPT = "What do you see in this image?"

# Scale factors to test (1.0 = original)
SCALES = [1.0, 0.75, 0.5, 0.35, 0.25]


def resize_image(img: Image.Image, scale: float) -> tuple[str, int]:
    """Resize image by scale factor, return (base64, byte_size)."""
    if scale == 1.0:
        buf = io.BytesIO()
        img.save(buf, format="JPEG", quality=85)
    else:
        new_w = int(img.width * scale)
        new_h = int(img.height * scale)
        resized = img.resize((new_w, new_h), Image.LANCZOS)
        buf = io.BytesIO()
        resized.save(buf, format="JPEG", quality=85)

    raw = buf.getvalue()
    return base64.b64encode(raw).decode(), len(raw)


def run_test(image_b64: str, label: str) -> dict:
    """Send a single query and return timing info."""
    # Clear conversation first
    requests.post(CLEAR_URL, timeout=5)

    payload = {
        "image": image_b64,
        "audio": "",
        "transcript": TRANSCRIPT,
    }

    t0 = time.time()
    resp = requests.post(QUERY_URL, json=payload, timeout=60)
    elapsed = time.time() - t0
    resp.raise_for_status()

    return {
        "label": label,
        "total_server_ms": int(resp.headers.get("X-Total-Ms", 0)),
        "round_trip_ms": int(elapsed * 1000),
        "response_text": resp.headers.get("X-Response-Text", "")[:120],
        "audio_bytes": len(resp.content),
    }


def main():
    img = Image.open(IMAGE_PATH)
    print(f"Original: {img.size[0]}x{img.size[1]}")
    print(f"Query: \"{TRANSCRIPT}\"")
    print(f"Scales: {SCALES}")
    print()
    print(f"{'Scale':<8} {'Resolution':<14} {'JPEG KB':<10} {'B64 KB':<10} {'Server ms':<12} {'Round-trip ms':<14} {'Response preview'}")
    print("-" * 120)

    results = []
    for scale in SCALES:
        new_w = int(img.width * scale)
        new_h = int(img.height * scale)
        label = f"{scale:.0%} ({new_w}x{new_h})"

        image_b64, raw_size = resize_image(img, scale)
        b64_size = len(image_b64)

        result = run_test(image_b64, label)
        results.append(result)

        print(
            f"{scale:<8.2f} {new_w}x{new_h:<10} {raw_size//1024:<10} {b64_size//1024:<10} "
            f"{result['total_server_ms']:<12} {result['round_trip_ms']:<14} {result['response_text'][:60]}"
        )

    # Summary
    baseline = results[0]["total_server_ms"]
    print()
    print("=== Summary ===")
    for r in results:
        diff = r["total_server_ms"] - baseline
        sign = "+" if diff >= 0 else ""
        print(f"  {r['label']:<24} {r['total_server_ms']}ms server  ({sign}{diff}ms vs baseline)")


if __name__ == "__main__":
    main()
