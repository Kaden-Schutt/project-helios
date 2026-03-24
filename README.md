# Helios Test Bench

Local simulation of the Helios pendant ↔ Pi relay pipeline.
Validates the full API chain (STT → Vision LLM → TTS) without touching any hardware.

## Setup

```bash
pip install -r requirements.txt
cp .env.example .env
# Edit .env — add your CARTESIA_API_KEY and OPENROUTER_API_KEY
```

## Usage

1. Drop a test JPEG into `./input/`
2. Start the server (simulates Pi):
   ```bash
   python server.py
   ```
3. In a second terminal, run the client (simulates ESP32):
   ```bash
   python client.py
   ```
4. Hold **spacebar** and ask a question about the image
5. Release spacebar — hear the AI response

## Architecture

```
client.py (ESP32 sim)              server.py (Pi sim)
┌─────────────┐                    ┌──────────────────────┐
│ Load JPEG   │                    │ POST /query          │
│ Record mic  │──── HTTP POST ────→│  ├─ Cartesia STT     │
│ Play audio  │←── PCM bytes ─────│  ├─ Gemini (OpenRouter)│
└─────────────┘                    │  └─ Cartesia TTS     │
                                   └──────────────────────┘
```

## Notes

- Audio format: PCM s16le, 16kHz, mono throughout the pipeline
- The STT WebSocket protocol may need adjustment — check Cartesia docs
  if you get connection errors, the URL/auth format might differ
- Gemini system prompt is in server.py — tune it for your use case
- On macOS: grant Terminal mic + input monitoring permissions
