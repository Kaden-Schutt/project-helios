"""
Helios Test Harness — Automated Scenario Testing
==================================================
Sends test images + text queries to the server (bypasses STT)
and logs the LLM responses for review.

Usage:
    python tests/test_scenarios.py              # run all tests
    python tests/test_scenarios.py crosswalk    # run tests matching 'crosswalk'

Requires the server to be running (python server.py).
"""

import os
import sys
import json
import base64
import time
import requests
from pathlib import Path
from dotenv import load_dotenv

load_dotenv()

SERVER_URL = os.getenv("HELIOS_SERVER_URL", "http://localhost:5750/query")
CLEAR_URL = SERVER_URL.replace("/query", "/clear")
IMAGES_DIR = Path(__file__).parent / "images"
RESULTS_DIR = Path(__file__).parent / "results"

# ---------------------------------------------------------------------------
# Test scenario definitions
# ---------------------------------------------------------------------------
# Each scenario: (id, image_file, queries)
# queries is a list of dicts with "text" and optional "expects" keywords
SCENARIOS = [
    {
        "id": "crosswalk_safe",
        "name": "Crosswalk — Walk Signal",
        "image": "crosswalk_walk.jpg",
        "queries": [
            {
                "text": "Is it safe to cross the street?",
                "expects": ["safe", "walk", "cross", "go", "yes"],
            },
        ],
    },
    {
        "id": "crosswalk_timer",
        "name": "Crosswalk — Countdown Timer",
        "image": "crosswalk_timer.jpg",
        "queries": [
            {
                "text": "Is it safe to cross the street?",
                "expects": [],  # open-ended — see how it handles the timer
            },
        ],
    },
    {
        "id": "crosswalk_unsafe",
        "name": "Crosswalk — Stop Signal",
        "image": "crosswalk_stop.jpg",
        "queries": [
            {
                "text": "Is it safe to cross the street?",
                "expects": ["stop", "wait", "don't", "no", "hand", "not safe"],
            },
        ],
    },
    {
        "id": "menu_reading",
        "name": "Restaurant Menu — Read & Follow-up",
        "image": "menu_board.jpg",
        "queries": [
            {
                "text": "What's on the menu?",
                "expects": ["burger", "fries", "drink", "combo", "meal", "chicken", "sandwich"],
            },
            {
                "text": "What's the cheapest item?",
                "expects": ["$", "cheapest", "least", "lowest"],
            },
        ],
    },
    {
        "id": "navigation_door",
        "name": "Indoor Navigation — Find Exit",
        "image": "hallway_door.jpg",
        "queries": [
            {
                "text": "Where is the exit?",
                "expects": ["door", "left", "right", "ahead", "straight", "exit"],
            },
            {
                "text": "Is there anything blocking the path?",
                "expects": [],  # open-ended — just log the response
            },
        ],
    },
    {
        "id": "crowded_space",
        "name": "Crowded Space — Obstacle Detection",
        "image": "crowded_space.jpg",
        "queries": [
            {
                "text": "Is anything in my way?",
                "expects": ["people", "person", "crowd", "table", "chair", "obstacle"],
            },
            {
                "text": "Which direction has the most open space?",
                "expects": ["left", "right", "ahead", "behind", "forward"],
            },
        ],
    },
    {
        "id": "grocery_aisle",
        "name": "Grocery Store — Section Identification",
        "image": "grocery_aisle.jpg",
        "queries": [
            {
                "text": "What section of the store am I in?",
                "expects": ["aisle", "section", "shelf", "shelves"],
            },
        ],
    },
]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def load_image_b64(filename: str) -> str:
    path = IMAGES_DIR / filename
    if not path.exists():
        return ""
    with open(path, "rb") as f:
        return base64.b64encode(f.read()).decode()


def clear_conversation():
    try:
        requests.post(CLEAR_URL, timeout=5)
    except Exception:
        pass


def send_query(image_b64: str, transcript: str) -> dict:
    """Send a text query (no audio/STT) and return result dict."""
    payload = {
        "image": image_b64,
        "audio": "",
        "transcript": transcript,
        "timestamp": int(time.time()),
    }

    t0 = time.time()
    resp = requests.post(SERVER_URL, json=payload, timeout=60)
    elapsed = time.time() - t0
    resp.raise_for_status()

    return {
        "transcript": resp.headers.get("X-Transcript", ""),
        "response": resp.headers.get("X-Response-Text", ""),
        "turns": resp.headers.get("X-Conversation-Turns", "?"),
        "server_ms": resp.headers.get("X-Total-Ms", "?"),
        "round_trip_s": round(elapsed, 2),
        "audio_bytes": len(resp.content),
    }


def check_expects(response_text: str, expects: list) -> tuple:
    """Check if response contains any expected keywords. Returns (pass, matched)."""
    if not expects:
        return (True, [])  # No expectations = auto-pass
    lower = response_text.lower()
    matched = [kw for kw in expects if kw.lower() in lower]
    return (len(matched) > 0, matched)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def run_tests(filter_str: str = ""):
    RESULTS_DIR.mkdir(exist_ok=True)
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    results_file = RESULTS_DIR / f"run_{timestamp}.json"

    # Check server
    try:
        health_url = SERVER_URL.replace("/query", "/health")
        r = requests.get(health_url, timeout=5)
        health = r.json()
        print(f"  Server: {health.get('status', '?')}  "
              f"LLM: {health.get('llm_model', '?')}")
    except Exception:
        print("  ERROR: Server not reachable. Start it first: python server.py")
        sys.exit(1)

    # Filter scenarios
    scenarios = SCENARIOS
    if filter_str:
        scenarios = [s for s in scenarios if filter_str.lower() in s["id"].lower()
                     or filter_str.lower() in s["name"].lower()]
        if not scenarios:
            print(f"  No scenarios matching '{filter_str}'")
            sys.exit(1)

    print(f"\n  Running {len(scenarios)} scenario(s)...\n")

    all_results = []
    total_pass = 0
    total_fail = 0
    total_skip = 0

    for scenario in scenarios:
        print(f"  ┌─ {scenario['name']}")

        image_b64 = load_image_b64(scenario["image"])
        if not image_b64:
            print(f"  │ SKIP — image not found: {scenario['image']}")
            print(f"  └─\n")
            total_skip += 1
            all_results.append({
                "scenario": scenario["id"],
                "status": "skipped",
                "reason": f"image not found: {scenario['image']}",
            })
            continue

        # Clear conversation before each scenario
        clear_conversation()

        scenario_results = []
        for i, query in enumerate(scenario["queries"]):
            result = send_query(image_b64, query["text"])
            passed, matched = check_expects(result["response"], query.get("expects", []))

            status = "PASS" if passed else "FAIL"
            if passed:
                total_pass += 1
            else:
                total_fail += 1

            turn_label = f"Turn {i+1}" if len(scenario["queries"]) > 1 else "Query"
            match_info = f" (matched: {', '.join(matched)})" if matched else ""

            print(f"  │ [{status}] {turn_label}: \"{query['text']}\"")
            print(f"  │        → \"{result['response'][:200]}\"")
            if query.get("expects"):
                print(f"  │        {status}{match_info}")
            print(f"  │        ({result['server_ms']}ms server, turn {result['turns']})")

            scenario_results.append({
                "query": query["text"],
                "response": result["response"],
                "expects": query.get("expects", []),
                "matched": matched,
                "passed": passed,
                "server_ms": result["server_ms"],
                "turn": result["turns"],
            })

        print(f"  └─\n")
        all_results.append({
            "scenario": scenario["id"],
            "name": scenario["name"],
            "image": scenario["image"],
            "results": scenario_results,
        })

    # Summary
    total = total_pass + total_fail
    print(f"  ══════════════════════════════════")
    print(f"  Results: {total_pass}/{total} passed", end="")
    if total_fail:
        print(f", {total_fail} failed", end="")
    if total_skip:
        print(f", {total_skip} skipped", end="")
    print()

    # Save results
    with open(results_file, "w") as f:
        json.dump({
            "timestamp": timestamp,
            "server_url": SERVER_URL,
            "summary": {"passed": total_pass, "failed": total_fail, "skipped": total_skip},
            "scenarios": all_results,
        }, f, indent=2)
    print(f"  Results saved: {results_file}")


if __name__ == "__main__":
    print()
    print("  ╔══════════════════════════════════════╗")
    print("  ║     HELIOS TEST HARNESS              ║")
    print("  ╚══════════════════════════════════════╝")

    filter_str = sys.argv[1] if len(sys.argv) > 1 else ""
    run_tests(filter_str)
