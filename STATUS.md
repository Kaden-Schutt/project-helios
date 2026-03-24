# Project Helios — Current State

**Date:** 2026-03-09
**Status:** Test bench functional, core pipeline validated

## Architecture

```
client.py (ESP32 sim)                server.py (Pi sim)
┌──────────────────┐                 ┌───────────────────────────────┐
│ Load JPEG        │                 │ POST /query                   │
│ Hold spacebar    │                 │   1. Cartesia Ink-Whisper STT │
│   → record mic   │── HTTP POST ──→│      (WebSocket, ~0.8s)       │
│ Release spacebar │                 │   2. Gemini 3.1 Flash Lite    │
│   → send payload │                 │      via OpenRouter (~2-3s)   │
│ Play TTS audio   │←─ PCM bytes ──│   3. Cartesia Sonic 3 TTS     │
│ Double-tap space │                 │      (~1.5-2.5s)              │
│   → clear convo  │── POST /clear →│                               │
└──────────────────┘                 └───────────────────────────────┘
```

**Total round-trip: ~4-6s warm, ~5-8s cold start**

## What Changed from Initial Draft

The original code from Claude Desktop had several issues that were fixed:

### API Corrections (Cartesia)
- **STT WebSocket URL:** `/stt` → `/stt/websocket` (bare `/stt` is the batch endpoint)
- **STT auth:** API key moved from query param to `X-API-Key` header
- **STT version header:** Added `Cartesia-Version` header (was missing entirely — this was the main reason STT wasn't returning transcripts)
- **STT done signal:** `{"type": "done"}` JSON → plain text `"done"` (Cartesia expects text commands)
- **STT response parsing:** Removed non-existent `"final"` message type; filter on `is_final` boolean
- **TTS model:** Updated to `sonic-3-2026-01-12`
- **Cartesia-Version:** Updated to `2026-01-12`

### API Corrections (OpenRouter)
- **Content order:** Image and text swapped so text comes first (OpenRouter docs recommend this)
- **Model:** Updated to `google/gemini-3.1-flash-lite-preview:nitro`

### Features Added
- **Ephemeral conversation history:** Prior turns sent to Gemini for follow-up questions. Auto-expires after 5 minutes (configurable via `CONVERSATION_TIMEOUT` env var). Timer resets on each query.
- **Manual conversation clear:** Double-tap spacebar in client → `POST /clear` on server
- **Test harness bypass:** Server accepts `transcript` field in POST body to skip STT (used by test harness for text-only queries)
- **Conversation turn tracking:** `X-Conversation-Turns` response header

### Architecture Decision: Direct Audio to Gemini (Rejected)
We tested sending audio directly to Gemini (bypassing Ink-Whisper) to reduce latency. **Result: slower.** Gemini takes ~5s to process audio tokens natively vs ~0.8s for Ink-Whisper STT + ~2s for text-only Gemini. The dedicated STT step is faster because Gemini's audio tokenization (32 tok/sec) is expensive. Reverted to the STT pipeline.

| Pipeline | STT | LLM | TTS | Total |
|----------|-----|-----|-----|-------|
| Ink-Whisper → Gemini (text) | 0.82s | 2.08s | 2.51s | **5.42s** |
| Gemini (native audio) | — | 5.17s | 1.94s | **7.11s** |

## Pipeline Components

| Component | Service | Model | Notes |
|-----------|---------|-------|-------|
| STT | Cartesia (WebSocket) | `ink-whisper` | 16kHz PCM s16le mono, ~0.8s for 3s audio |
| LLM | OpenRouter → Gemini | `gemini-3.1-flash-lite-preview:nitro` | Vision + text, 300 max tokens |
| TTS | Cartesia (REST) | `sonic-3-2026-01-12` | 16kHz PCM s16le output |

## Environment

- **Python:** 3.12 (conda env `helios`)
- **Server port:** 5750
- **Audio device:** Logitech PRO X Wireless (default input + output)

## File Structure

```
project-helios/
├── server.py              # Pi relay server (STT → LLM → TTS)
├── client.py              # ESP32 simulator (mic + spacebar + playback)
├── requirements.txt       # Python dependencies
├── .env                   # API keys (CARTESIA_API_KEY, OPENROUTER_API_KEY)
├── input/                 # Drop a JPEG here for client.py
├── tests/
│   ├── test_scenarios.py  # Automated test harness (text queries, no STT)
│   ├── images/            # Test scenario images
│   │   ├── crosswalk_walk.jpg
│   │   ├── crosswalk_stop.jpg
│   │   ├── crosswalk_timer.jpg
│   │   ├── menu_board.jpg
│   │   ├── hallway_door.jpg
│   │   ├── crowded_space.jpg
│   │   └── grocery_aisle.jpg
│   └── results/           # JSON test run outputs
├── README.md
└── STATUS.md              # This file
```

## Test Results (2026-03-09)

**8/9 passed, 1 false negative**

| Scenario | Image | Query | Result | Response Summary |
|----------|-------|-------|--------|-----------------|
| Crosswalk safe | Walk signal | "Is it safe to cross?" | PASS | "White walking figure...safe to cross" |
| Crosswalk timer | Countdown timer | "Is it safe to cross?" | PASS | "Red hand...must wait" (correct — don't start crossing) |
| Crosswalk unsafe | Stop hand | "Is it safe to cross?" | PASS | "Red hand...must wait" |
| Menu read | In-N-Out board | "What's on the menu?" | PASS | Listed items with prices |
| Menu follow-up | Same image | "What's the cheapest item?" | PASS | "Milk, 99 cents" (conversation history working) |
| Navigation | Hallway/door | "Where is the exit?" | PASS | "Far end...straight ahead" |
| Path check | Same image | "Anything blocking the path?" | PASS | "Path appears clear, table to your left" |
| Obstacle detect | Mall walkway | "Anything in my way?" | FAIL* | "Path ahead is clear, stairs/escalator to left" |
| Open space | Same image | "Most open space?" | PASS | "Directly in front of you" |
| Grocery | Store aisle | "What section am I in?" | PASS | "Breakfast aisle" |

*False negative — model correctly identified the path was clear, but test expected obstacle keywords (people, crowd, etc.). The image is an open walkway. Model gave the right answer.

## Known Considerations

- **Cold start:** First query per session is ~1-2s slower (TLS handshake, model warmup). Could add a startup warmup ping.
- **Conversation history for audio queries:** When using mic input, the user's transcript is stored in history. For text-only test queries, the text is stored directly.
- **Persistent WebSocket for Pi:** On the actual Pi deployment, keeping the Ink-Whisper WebSocket open persistently (with reconnect on Cartesia's 3-min idle timeout) would eliminate per-query connection overhead.
