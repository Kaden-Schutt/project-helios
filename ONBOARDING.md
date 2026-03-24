# Project Helios — Team Dev Environment

## Getting Started

1. Go to **https://helios.schutt.dev**
2. Enter your **ASU email** when prompted
3. Check your inbox for a one-time code, enter it
4. You'll be auto-redirected to your workspace (VS Code in the browser)

That's it. You're in.

## Your Workspace

Each team member has their own isolated workspace with:

- **Your own git branch** (named after you) — changes you make don't affect anyone else
- **The full project-helios codebase** already cloned
- **All tools pre-installed** — no setup needed

### Pre-installed Extensions

| Extension | What it does |
|-----------|-------------|
| **Claude Code** | AI coding assistant (Sonnet 4.6) — open from the sidebar or type `claude` in terminal |
| **PlatformIO** | ESP32 build system — compile and manage firmware |
| **clangd** | C/C++ IntelliSense for firmware code |
| **Python** | Python support for server.py and tests |
| **GitDoc** | Auto-commits your changes on save — no git commands needed |
| **Wokwi** | Circuit simulator — test the Safety Loop without real hardware |

**First run note:** If Claude Code shows a login prompt or doesn't appear in the sidebar:
1. Go to Extensions panel (Ctrl+Shift+X)
2. Find Claude Code → click **Disable**
3. Reload the page (F5 or browser refresh)
4. Go back to Extensions → click **Enable**

This only needs to be done once. After that it works automatically.

### Pre-installed Tools (Terminal)

| Command | What it does |
|---------|-------------|
| `claude` | AI assistant in the terminal — ask it anything about the code |
| `pio run` | Build the firmware |
| `pio run --target upload` | Flash to ESP32 (only works on a machine with the board plugged in) |
| `python3 server.py` | Run the AI query server |

## Project Structure

```
project-helios/
├── firmware/           # ESP32-S3 C code (PlatformIO project)
│   ├── src/
│   │   ├── main.c      # Entry point
│   │   ├── camera.c    # Camera capture
│   │   └── mic.c       # Microphone capture
│   ├── include/
│   │   └── helios.h    # Pin definitions and config
│   └── platformio.ini  # Build config
├── server.py           # Raspberry Pi server (receives JPEG + PCM, calls AI APIs)
├── client.py           # Test client
├── usb_receiver.py     # USB serial receiver
├── tests/              # Test images and scenarios
├── diagram.json        # Wokwi circuit simulator config
├── wokwi.toml          # Wokwi firmware pointer
└── .env                # API keys (not in git — auto-loaded)
```

## Using Claude Code

Claude is an AI that can read, edit, and run your code. It's pre-configured with full permissions.

**From the sidebar:** Click the Claude icon on the left sidebar.

**From the terminal:** Type `claude` and ask it anything:
```
claude "explain what main.c does"
claude "add a new sensor to the safety loop"
claude "fix the build error in camera.c"
```

## Using Wokwi Simulator

The Safety Loop circuit is pre-configured with:
- XIAO ESP32-S3 board
- 2x HC-SR04 ultrasonic sensors (left/right)
- 2x buzzers (left/right)
- Push button

To simulate:
1. Open `diagram.json`
2. Press **F1** → **Wokwi: Start Simulator**
3. Click the ultrasonic sensors to change distance values
4. Watch the buzzers respond

## Using GitDoc (Auto-Save to Git)

GitDoc automatically commits and pushes your changes when you save a file. You don't need to run any git commands.

- Your changes go to **your branch** (e.g. `kaden`, `jer`, `mohamed`, etc.)
- To get the latest code from main: open terminal and run `git merge origin/main`
- To see what changed: check the Source Control panel (Ctrl+Shift+G)

## Flashing the Real ESP32

The ESP32 lives in the classroom. When you're physically there:

```bash
git pull
pio run --target upload
```

Or test with the Wokwi simulator from anywhere.

## Need Help?

- Ask Claude (sidebar or terminal)
- Message the team Discord
- Check the [Seeed Studio XIAO ESP32-S3 Wiki](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
