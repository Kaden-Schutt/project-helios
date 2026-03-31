"""
Helios Server Pipeline Tests
==============================
Validates each stage of the server pipeline without BLE hardware.
Requires API keys in .env (CARTESIA_API_KEY, ANTHROPIC_API_KEY).

Run:  python tests/test_server_pipeline.py
"""

import sys, os, struct, time, math, asyncio

# Add project root to path
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

if os.path.exists("/opt/homebrew/lib"):
    os.environ.setdefault("DYLD_LIBRARY_PATH", "/opt/homebrew/lib")

from dotenv import load_dotenv
load_dotenv()

passed = 0
failed = 0

def test(name, fn):
    global passed, failed
    print(f"\n  TEST: {name}")
    try:
        fn()
        print(f"  PASS: {name}")
        passed += 1
    except Exception as e:
        print(f"  FAIL: {name} -- {e}")
        failed += 1


# --- Test 1: Opus roundtrip ---
def test_opus_roundtrip():
    import numpy as np
    from server_ble import pcm_to_opus
    import opuslib

    # Generate 1s 440Hz sine wave at 24kHz s16le
    sr = 24000
    t = np.arange(sr) / sr
    sine = (np.sin(2 * np.pi * 440 * t) * 16000).astype(np.int16)
    pcm_in = sine.tobytes()

    # Encode
    opus_data = pcm_to_opus(pcm_in, sr)
    assert len(opus_data) > 0, "Opus encode returned empty"
    assert len(opus_data) < len(pcm_in), f"Opus should compress: {len(opus_data)} >= {len(pcm_in)}"

    # Decode
    dec = opuslib.Decoder(sr, 1)
    frame_samples = sr // 50
    decoded = bytearray()
    offset = 0
    while offset + 2 <= len(opus_data):
        frame_len = struct.unpack('<H', opus_data[offset:offset+2])[0]
        offset += 2
        if frame_len == 0:
            break
        frame = opus_data[offset:offset+frame_len]
        offset += frame_len
        pcm = dec.decode(frame, frame_samples)
        decoded.extend(pcm)

    # Verify decoded audio is non-trivial
    samples = np.frombuffer(bytes(decoded), dtype=np.int16)
    rms = math.sqrt(np.mean(samples.astype(np.float64) ** 2))
    assert rms > 1000, f"Decoded audio too quiet (RMS={rms:.0f}), expected >1000"
    assert len(decoded) > 40000, f"Decoded too short: {len(decoded)}"
    print(f"    Encoded: {len(pcm_in):,} -> {len(opus_data):,} bytes, decoded RMS={rms:.0f}")


# --- Test 2: JPEG demux ---
def test_jpeg_demux():
    from server_ble import HeliosClient

    client = HeliosClient()

    # Simulate a minimal JPEG (just SOI + EOI markers)
    fake_jpeg = b'\xff\xd8' + b'\x00' * 100 + b'\xff\xd9'
    jpeg_len = len(fake_jpeg)

    # Notification 1: JPEG header
    hdr = b'\xFE\xFF\xFF\xFF' + struct.pack('<I', jpeg_len)
    client._on_mic_notify(None, bytearray(hdr))
    assert client.jpeg_receiving, "Should be in JPEG receiving mode"

    # Notification 2: JPEG data (all at once)
    client._on_mic_notify(None, bytearray(fake_jpeg))
    assert not client.jpeg_receiving, "JPEG should be complete"
    assert client.jpeg_b64 is not None, "JPEG base64 should be set"

    import base64
    decoded = base64.b64decode(client.jpeg_b64)
    assert decoded[:2] == b'\xff\xd8', "JPEG SOI missing"
    assert decoded[-2:] == b'\xff\xd9', "JPEG EOI missing"
    print(f"    JPEG demux OK: {len(decoded)} bytes, valid SOI+EOI")

    # Now simulate Opus start marker
    client._on_mic_notify(None, bytearray(b'\xFF\xFF\xFF\xFF'))
    assert client.mic_opus_mode, "Should be in Opus mode"
    assert client.mic_receiving, "Should be receiving"

    # Empty = end
    client._on_mic_notify(None, bytearray())
    assert not client.mic_receiving, "Should be done"
    assert client.query_event.is_set(), "Query event should be set"
    print("    Opus marker + end marker OK")


# --- Test 3: Anthropic API ---
def test_anthropic_api():
    api_key = os.getenv("ANTHROPIC_API_KEY")
    if not api_key:
        raise RuntimeError("ANTHROPIC_API_KEY not set")

    async def _test():
        from server_ble import llm_query
        result = await llm_query("What is 2 plus 2?", [])
        assert len(result) > 0, "Empty response"
        assert "4" in result or "four" in result.lower(), f"Expected '4' in response: {result}"
        print(f"    Response: \"{result[:80]}\"")
    asyncio.run(_test())


# --- Test 4: Anthropic Vision API ---
def test_anthropic_vision():
    api_key = os.getenv("ANTHROPIC_API_KEY")
    if not api_key:
        raise RuntimeError("ANTHROPIC_API_KEY not set")

    # Create a tiny test JPEG (1x1 red pixel)
    import base64
    # Minimal valid JPEG: 1x1 pixel
    tiny_jpeg_b64 = (
        "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoH"
        "BwYIDAoMCwsKCwsM"
    )
    # Use a real test image if available
    test_images = [f for f in ["input/IMG_2091.jpg", "tests/test.jpg"] if os.path.exists(f)]
    if test_images:
        with open(test_images[0], "rb") as f:
            tiny_jpeg_b64 = base64.b64encode(f.read()).decode()
        print(f"    Using test image: {test_images[0]}")

    async def _test():
        from server_ble import llm_query
        result = await llm_query("Describe what you see in one sentence.", [], tiny_jpeg_b64)
        assert len(result) > 0, "Empty response"
        print(f"    Vision response: \"{result[:80]}\"")
    asyncio.run(_test())


# --- Test 5: STT ---
def test_stt():
    cartesia_key = os.getenv("CARTESIA_API_KEY")
    if not cartesia_key:
        raise RuntimeError("CARTESIA_API_KEY not set")

    import numpy as np
    # Generate 2s of silence with a brief tone (ensures non-empty audio)
    sr = 16000
    t = np.arange(sr * 2) / sr
    # Very quiet audio — STT may return empty, that's OK for connectivity test
    pcm = (np.sin(2 * np.pi * 440 * t) * 100).astype(np.int16).tobytes()

    async def _test():
        from server_ble import stt_transcribe
        result = await stt_transcribe(pcm)
        # STT on a sine wave likely returns empty — just verify no crash
        print(f"    STT result: \"{result}\" (empty OK for sine wave)")
    asyncio.run(_test())


# --- Test 6: TTS ---
def test_tts():
    cartesia_key = os.getenv("CARTESIA_API_KEY")
    if not cartesia_key:
        raise RuntimeError("CARTESIA_API_KEY not set")

    async def _test():
        from server_ble import tts_synthesize, TTS_SAMPLE_RATE
        pcm = await tts_synthesize("Hello world.")
        assert len(pcm) > 0, "TTS returned empty"
        # At 24kHz s16le, 1s = 48000 bytes. "Hello world" should be ~0.5-2s
        duration = len(pcm) / 2 / TTS_SAMPLE_RATE
        assert 0.3 < duration < 5.0, f"TTS duration suspicious: {duration:.1f}s"
        print(f"    TTS: {len(pcm):,} bytes ({duration:.1f}s)")
    asyncio.run(_test())


# --- Test 7: Full pipeline ---
def test_full_pipeline():
    """STT(silence) -> LLM -> TTS -> Opus encode. Verifies chain produces output."""
    async def _test():
        from server_ble import llm_query, tts_synthesize, pcm_to_opus, TTS_SAMPLE_RATE
        t0 = time.time()

        # Skip STT (would need real speech), use canned transcript
        transcript = "What color is the sky?"

        # LLM
        response = await llm_query(transcript, [])
        assert len(response) > 0, "LLM empty"

        # TTS
        tts_pcm = await tts_synthesize(response)
        assert len(tts_pcm) > 0, "TTS empty"

        # Opus encode
        opus = pcm_to_opus(tts_pcm, TTS_SAMPLE_RATE)
        assert len(opus) > 0, "Opus empty"

        total = time.time() - t0
        print(f"    Pipeline: {total:.1f}s | \"{response[:60]}\"")
        print(f"    TTS: {len(tts_pcm):,} bytes -> Opus: {len(opus):,} bytes")
    asyncio.run(_test())


if __name__ == "__main__":
    print("\n  =======================================")
    print("  Helios Server Pipeline Tests")
    print("  =======================================")

    test("Opus roundtrip encode/decode", test_opus_roundtrip)
    test("JPEG demux state machine", test_jpeg_demux)
    test("Anthropic Haiku API", test_anthropic_api)
    test("Anthropic Haiku Vision", test_anthropic_vision)
    test("Cartesia STT connectivity", test_stt)
    test("Cartesia TTS", test_tts)
    test("Full pipeline (LLM -> TTS -> Opus)", test_full_pipeline)

    print(f"\n  =======================================")
    print(f"  Results: {passed} passed, {failed} failed")
    print(f"  =======================================\n")
    sys.exit(1 if failed > 0 else 0)
