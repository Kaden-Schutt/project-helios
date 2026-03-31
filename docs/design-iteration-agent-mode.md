# Design Iteration: Voice-Driven Agent Mode

## Status: Proposed (Future Iteration)

This document explores a design iteration for the Helios AI query loop: adding an advanced voice-driven agent mode alongside the existing quick-query mode, powered by [Hermes Agent](https://github.com/NousResearch/hermes-agent) (MIT, Nous Research).

## Motivation

The current Helios pipeline is a single-shot query loop: button press, capture image, ask a question, get a 1-3 sentence spoken answer. This covers the immediate assistive case well ("what's in front of me?", "is it safe to walk?"), but a vision-impaired user's needs extend far beyond scene description.

Tasks that sighted people do trivially — browsing the web, reading and composing messages, managing a calendar, looking up a product, reading a document — require sustained, multi-step interaction with a capable AI agent. The wearable form factor makes Helios uniquely positioned to deliver this: the user already has a camera, microphone, and speaker on their body, connected to a compute hub (Raspberry Pi) that can run an agent.

## Multi-Tier Interaction Design

The button interaction maps to three tiers of AI capability:

| Gesture | Model | Mode | Latency | Cost |
|---------|-------|------|---------|------|
| Single click | Haiku 4.5 | Quick query (current) | ~2s | $1/$5 per 1M |
| Double click | Sonnet 4.6 | Voice agent | ~4s TTFT | $3/$15 per 1M |
| Triple click | Opus 4.6 | Full power agent | ~5-6s TTFT | $5/$25 per 1M |

**Single click** remains unchanged — fast, cheap, immediate answers. No tools, no memory, no persistence.

**Double/triple click** activates agent mode: the same voice capture pipeline, but routed through a Hermes Agent instance running on the Pi. The agent has access to tools, persistent memory, learned skills, and multi-turn conversation with context carried across sessions. The user interacts entirely by voice — each button press is a new "turn" in an ongoing conversation that the agent remembers.

### ESP32 Firmware Changes

Detect click patterns on the button GPIO:
- Single click: <500ms press, current behavior
- Double click: two presses within 500ms, send `BLE_CMD_AGENT_MODE` with model selector byte
- Triple click: three presses within 750ms, same command with different model byte

The ESP32 doesn't need to know what mode it's in beyond the initial trigger — audio capture, BLE streaming, and speaker playback are identical across modes. The Pi routes to the appropriate backend.

## Why Hermes Agent

Hermes Agent is an open-source (MIT) Python agent framework with 50+ built-in tools, persistent memory, a skills system, and multi-platform gateway support. It runs on hardware as modest as a $5 VPS.

Key properties that make it a fit for Helios:

### 1. Tool System (50+ Built-In Tools)

Hermes uses a singleton tool registry where tools self-register at import time. Each tool is a Python function with a name, schema, and handler. Adding a custom tool is four lines:

```python
registry.register(
    name="describe_scene",
    toolset="helios",
    schema={"type": "function", "function": {"name": "describe_scene", ...}},
    handler=describe_scene_handler,
)
```

Built-in tools that map directly to assistive use cases:

| Hermes Tool | Assistive Application |
|------------|----------------------|
| `browser_tool` | Browse the web by voice — agent navigates, reads content aloud, fills forms |
| `web_tools` | Look up information, check weather, find business hours |
| `file_tools` | Read/write documents, manage notes |
| `send_message_tool` | Send messages to contacts via integrated platforms |
| `homeassistant_tool` | Control smart home (lights, thermostat, locks, appliances) by voice |
| `todo_tool` | Manage task lists and reminders |
| `memory_tool` | Store and recall information across sessions |
| `session_search_tool` | Search past conversations ("what did I ask about yesterday?") |
| `cronjob_tools` | Schedule recurring tasks (daily briefings, medication reminders) |
| `vision_tools` | Image analysis (already integrated, maps to camera) |
| `transcription_tools` | Audio transcription (maps to mic pipeline) |
| `tts_tool` | Text-to-speech (maps to speaker pipeline) |
| `code_execution_tool` | Run computations, data processing |
| `delegate_tool` | Spawn sub-agents for parallel tasks |
| `skills_tool` | Execute learned procedures |

### 2. Browser for the Blind

The browser tool deserves special attention. It renders web pages as **text-based accessibility trees** rather than screenshots — interactive elements get ref selectors (`@e1`, `@e2`) that the agent can click, type into, and navigate. This means a vision-impaired user can:

- "Search for the nearest pharmacy and tell me their hours"
- "Go to my bank's website and check my balance" (with saved credentials)
- "Read me the top 3 headlines from NPR"
- "Order my usual from DoorDash"
- "Fill out this form — I'll dictate the answers"

The agent reads the page structure, narrates relevant content, and takes actions on the user's behalf. Pages exceeding 8,000 characters are automatically summarized by the LLM to extract task-relevant information. Sessions are isolated and auto-cleaned after 5 minutes of inactivity.

### 3. Persistent Memory

Hermes implements multi-layer memory:

- **Conversation memory**: Full session history with FTS5 full-text search and LLM summarization for cross-session recall
- **User modeling**: Progressive understanding of the user via Honcho dialectic modeling — learns preferences, habits, needs over time
- **Curated memory**: Agent periodically consolidates important information via "nudges" — the user doesn't have to explicitly say "remember this"
- **Skills**: Procedures learned from complex tasks, self-improving on subsequent use

For a vision-impaired user, this is transformative:
- "Remember that my pharmacy is Walgreens on Rural and Apache, my prescription number is 1234567"
- Agent learns the user's daily routine and proactively offers relevant information
- Frequently-asked questions become instant (agent recalls past answers)
- Personal context makes every interaction more efficient ("my usual coffee order", "my doctor's name", "where I parked")

### 4. Skills System

Hermes can learn procedures and replay them. After the user walks through a complex task once, the agent can abstract it into a reusable skill:

- "Order my prescriptions" — learned procedure: navigate to pharmacy site, log in, find prescription, request refill
- "Read my email" — learned procedure: connect to email, summarize unread messages, offer to read or reply
- "Morning briefing" — learned procedure: weather, calendar, news headlines, medication reminder

Skills follow the open `agentskills.io` standard and are stored locally in `~/.hermes/skills/`. They self-improve during use.

### 5. Smart Home Integration

The Home Assistant tool connects via REST API and provides:
- Entity discovery (list devices by room or type)
- State queries (is the front door locked? what's the thermostat set to?)
- Device control (turn on lights, adjust thermostat, lock doors)

High-risk domains are blocked (shell commands, scripts). All control goes through Hermes's safety layer.

For a vision-impaired user: "Turn on the kitchen lights", "Lock the front door", "Set the thermostat to 72", "Is the garage door open?"

### 6. Messaging & Communication

The existing `helios-bot` on the build server (`kaden@10.0.0.1:/home/kaden/ClaudeCode/helios-bot/`) already has an iMessage primitive:
- Polls for messages via a Mac watcher daemon
- Triggers on "helios" keyword mentions
- Sends responses via Sendblue API
- Runs through Claude Agent SDK

For the demo, the Hermes agent could call a `send_imessage` tool that hits the helios-bot API:
- "Send Jeremy a message saying I'll be late to the lab"
- "Read me my recent messages"
- "Reply to the last message from Mohamed saying yes"

Hermes's gateway system supports Telegram, Discord, Slack, WhatsApp, Signal, and email natively. Any of these could be voice-controlled through the pendant.

### 7. Cron Scheduling

Hermes has a built-in cron scheduler for recurring tasks with natural language specification and delivery to any platform:
- "Remind me to take my medication at 8 AM every day"
- "Give me a morning briefing at 7 AM with weather and calendar"
- "Check my email every hour and read me anything urgent"

Tasks run unattended and deliver results via the configured platform (in our case, spoken through the pendant via TTS, or sent as a message).

## Architecture on the Pi

```
ESP32 (Pendant)                    Raspberry Pi (Belt)
┌─────────────┐                   ┌──────────────────────────────────┐
│ Single click │ ─── BLE ───────> │ server_ble.py                    │
│ → Quick mode │                  │   └─ Haiku one-shot (current)    │
│              │                  │                                  │
│ Double click │ ─── BLE ───────> │ Hermes Agent (Sonnet 4.6)        │
│ → Agent mode │                  │   ├─ 50+ tools                   │
│              │                  │   ├─ Persistent memory           │
│ Triple click │ ─── BLE ───────> │   ├─ Skills system               │
│ → Full power │                  │ Hermes Agent (Opus 4.6)          │
│              │                  │   ├─ Same tools, deeper thinking │
│              │                  │   └─ Adaptive thinking enabled   │
└─────────────┘                   │                                  │
                                  │ Custom Helios Tools:             │
                                  │   ├─ describe_scene (camera)     │
                                  │   ├─ send_imessage (helios-bot)  │
                                  │   ├─ set_volume (BLE)            │
                                  │   ├─ get_status (BLE)            │
                                  │   └─ navigate (future: GPS)      │
                                  └──────────────────────────────────┘
```

### Hermes on Pi — What to Strip

The full Hermes install includes research tools (RL training, trajectory generation), multiple terminal backends (Docker, Singularity, Modal), and features unnecessary for an embedded device. A stripped deployment would keep:

**Keep:**
- Agent core (`/agent`) — the decision loop
- Tool registry + selected tools (browser, web, file, memory, todo, cron, HA, messaging, vision, TTS)
- Skills system
- Memory system (SQLite + FTS5)
- Honcho user modeling
- Config system

**Strip:**
- Gateway platforms (we route through BLE, not Telegram/Discord)
- CLI/TUI (no terminal on the pendant)
- RL training tools, batch runner, trajectory compression
- Docker/Singularity/Modal/Daytona backends
- Image generation (pendant has no display)
- CamoFox browser variant (standard headless Chromium suffices)

### Custom Gateway: BLE Pendant

Hermes's gateway is modular — each platform lives in `gateway/platforms/`. A `ble_pendant` platform adapter would:

1. Receive STT text from `server_ble.py` (already decoded from Opus)
2. Pass it to the Hermes agent as a user message
3. Stream the agent's text response back through Cartesia TTS → Opus → BLE
4. Handle multi-turn: each button press in agent mode is a new turn in the same Hermes session

The adapter is thin — Hermes handles conversation state, tool execution, and memory. The adapter just bridges voice I/O.

### Session Continuity

In agent mode, the conversation persists across button presses. The user can:
1. Double-click: "Find me a good Italian restaurant nearby"
2. (Agent browses, finds options, responds via TTS)
3. Double-click: "The second one sounds good. What are their hours?"
4. (Agent recalls the previous search, looks up hours)
5. Double-click: "Send Mohamed a message saying let's go there at 7"
6. (Agent calls send_imessage tool)

The session ends after a configurable timeout (e.g., 5 minutes of no interaction) or when the user says "done" / "exit agent mode." Single-click always bypasses agent mode and goes straight to Haiku quick query.

## Demo Scenario

For the April 21 demo day, a compelling agent mode demonstration:

1. **Quick query** (single click): "What's in front of me?" → Haiku: "A hallway with stairs ahead. Use the handrail on your right."

2. **Agent mode** (double click): "Hey Helios, I need to send a message to my team"
   - Sonnet: "Sure. Who should I message and what should I say?"
   - (button press) "Tell Jeremy and Mohamed that I'm running 10 minutes late to the lab"
   - Sonnet: calls `send_imessage` → "Done. I sent 'Running 10 minutes late to the lab' to Jeremy and Mohamed."

3. **Agent with memory** (double click): "What's on my schedule today?"
   - Sonnet: recalls user's calendar integration → "You have FSE 100 at 2 PM and a Helios team meeting at 4 PM."

4. **Smart home** (double click): "Turn off all the lights in the bedroom"
   - Sonnet: calls Home Assistant tool → "Done. All bedroom lights are off."

## Cost Considerations

| Mode | Typical Query | Est. Tokens | Est. Cost |
|------|--------------|-------------|-----------|
| Quick (Haiku) | "What's in front of me?" | ~500 in / ~50 out | $0.0008 |
| Agent (Sonnet) | 3-turn browsing task | ~5000 in / ~500 out | $0.0225 |
| Full (Opus) | Complex multi-tool task | ~5000 in / ~500 out | $0.0375 |

At ~50 agent queries per day, monthly cost would be ~$35 (Sonnet) or ~$56 (Opus). Haiku quick queries are essentially free at scale.

## Implementation Path

This is a future iteration — the current quick-query loop is the MVP for demo day. If pursued, the implementation order would be:

1. **Button gesture detection** — ESP32 firmware: detect single/double/triple click patterns
2. **Pi routing** — `server_ble.py`: route agent-mode BLE commands to Hermes
3. **Hermes deployment** — Strip and install Hermes on Pi, configure Anthropic provider
4. **Custom tools** — Register Helios-specific tools (camera, BLE commands, iMessage)
5. **BLE gateway adapter** — Bridge voice I/O to Hermes session
6. **Memory bootstrap** — Pre-load user profile, common locations, contacts
7. **Skills authoring** — Create initial skills for common tasks (morning briefing, message reading)
8. **Smart home** — Connect Home Assistant if available

## References

- [Hermes Agent GitHub](https://github.com/NousResearch/hermes-agent) — MIT license, Python
- [Hermes Agent Docs](https://hermes-agent.nousresearch.com/docs)
- [AgentSkills.io](https://agentskills.io) — Open skill standard
- [helios-bot](kaden@10.0.0.1:/home/kaden/ClaudeCode/helios-bot/) — Existing iMessage primitive
- [Anthropic Tool Use](https://platform.claude.com/docs/en/agents-and-tools/tool-use/overview) — Claude API tool calling
- [Cartesia TTS Streaming](https://docs.cartesia.ai/api-reference/tts/tts) — WebSocket TTS API
