"""
Helios settings store — JSON-backed key/value config with per-key validation.

Session vs saved scope:
- scope="saved"   (default) — writes to settings.json, persists across reboots
- scope="session"           — in-memory only, reverts on process restart

Loaded on import. Thread-safe for single-process use (FastAPI async is single-threaded).
"""

from __future__ import annotations

import json
import logging
import os
import threading
from pathlib import Path
from typing import Any, Callable

log = logging.getLogger("helios.settings")

_PATH = Path(os.getenv("HELIOS_SETTINGS_PATH", "settings.json")).resolve()
_LOCK = threading.RLock()


# Schema: (key, type, default, validator-or-None, description)
# Validator returns True for accept, False or raises for reject.
_SCHEMA: dict[str, dict[str, Any]] = {
    "wake_enabled":          {"type": bool,  "default": True,                                   "desc": "Whether always-on wake word detection is active"},
    "wake_phrase":           {"type": str,   "default": "hey helios",                           "desc": "Phrase that triggers voice query (fuzzy-matched)"},
    "wake_rms_threshold":    {"type": int,   "default": 2000, "range": (0, 32767),              "desc": "Mic RMS required during wake phrase to accept as user voice"},
    "voice_id":              {"type": str,   "default": "f786b574-daa5-4673-aa0c-cbe3e8534c02", "desc": "Cartesia TTS voice UUID"},
    "verbosity":             {"type": str,   "default": "brief", "enum": ["brief", "normal", "detailed"], "desc": "Response length target"},
    "volume":                {"type": int,   "default": 80, "range": (0, 100),                  "desc": "TTS playback volume percent"},
    "conversation_memory":   {"type": bool,  "default": True,                                   "desc": "Whether to retain context across queries"},
    "conversation_timeout_s":{"type": int,   "default": 300, "range": (10, 3600),               "desc": "Seconds of silence before conversation history clears"},
    "privacy_mute":          {"type": bool,  "default": False,                                  "desc": "Session-only mute of wake word"},
    "sleep_mode":            {"type": bool,  "default": False,                                  "desc": "Pendant suspended; single tap wakes"},
}


class SettingsError(ValueError):
    pass


def _coerce(key: str, value: Any) -> Any:
    """Coerce a JSON/string value to the declared type for `key`. Raises SettingsError."""
    spec = _SCHEMA.get(key)
    if spec is None:
        raise SettingsError(f"unknown setting: {key!r}")

    t = spec["type"]
    try:
        if t is bool:
            if isinstance(value, bool):
                out = value
            elif isinstance(value, str):
                v = value.strip().lower()
                if v in ("true", "on", "yes", "1"):
                    out = True
                elif v in ("false", "off", "no", "0"):
                    out = False
                else:
                    raise SettingsError(f"{key}: cannot parse {value!r} as bool")
            elif isinstance(value, (int, float)):
                out = bool(value)
            else:
                raise SettingsError(f"{key}: expected bool, got {type(value).__name__}")
        elif t is int:
            out = int(value)
        elif t is float:
            out = float(value)
        elif t is str:
            out = str(value)
        else:
            out = value
    except (ValueError, TypeError) as e:
        raise SettingsError(f"{key}: {e}") from e

    if "range" in spec:
        lo, hi = spec["range"]
        if not (lo <= out <= hi):
            raise SettingsError(f"{key}: {out} outside range [{lo}, {hi}]")
    if "enum" in spec and out not in spec["enum"]:
        raise SettingsError(f"{key}: {out!r} not in {spec['enum']}")
    return out


# In-memory store. Loaded from disk at import. Writes go through set().
_state: dict[str, Any] = {k: spec["default"] for k, spec in _SCHEMA.items()}

# Hooks registered by consumers. Called after a successful set().
_hooks: dict[str, list[Callable[[Any], None]]] = {}


def _load_from_disk() -> None:
    if not _PATH.exists():
        log.info(f"[settings] no {_PATH.name}, using defaults")
        return
    try:
        raw = json.loads(_PATH.read_text())
    except Exception as e:
        log.error(f"[settings] failed to parse {_PATH}: {e} — using defaults")
        return
    applied = 0
    for k, v in raw.items():
        if k not in _SCHEMA:
            log.warning(f"[settings] {_PATH.name} has unknown key {k!r}, skipping")
            continue
        try:
            _state[k] = _coerce(k, v)
            applied += 1
        except SettingsError as e:
            log.warning(f"[settings] {e} — keeping default for {k}")
    log.info(f"[settings] loaded {applied}/{len(raw)} from {_PATH.name}")


def _save_to_disk() -> None:
    try:
        _PATH.write_text(json.dumps(_state, indent=2, sort_keys=True) + "\n")
    except Exception as e:
        log.error(f"[settings] failed to write {_PATH}: {e}")


def get(key: str) -> Any:
    with _LOCK:
        if key not in _SCHEMA:
            raise SettingsError(f"unknown setting: {key!r}")
        return _state[key]


def all_settings() -> dict[str, Any]:
    with _LOCK:
        return dict(_state)


def schema() -> dict[str, dict[str, Any]]:
    """Return a JSON-serializable view of the schema for UI / LLM tool-use."""
    out: dict[str, dict[str, Any]] = {}
    for k, spec in _SCHEMA.items():
        view = {"type": spec["type"].__name__, "default": spec["default"], "desc": spec["desc"]}
        if "enum" in spec:
            view["enum"] = list(spec["enum"])
        if "range" in spec:
            view["range"] = list(spec["range"])
        out[k] = view
    return out


def set(key: str, value: Any, *, persist: bool = True) -> Any:
    """Set a setting. Returns the coerced value. Raises SettingsError on invalid key/value."""
    with _LOCK:
        new_val = _coerce(key, value)
        old_val = _state.get(key)
        _state[key] = new_val
        if persist:
            _save_to_disk()
        log.info(f"[settings] {key}: {old_val!r} -> {new_val!r} ({'saved' if persist else 'session'})")

    # Fire hooks outside the lock to avoid re-entrancy issues.
    for hook in _hooks.get(key, []):
        try:
            hook(new_val)
        except Exception as e:
            log.error(f"[settings] hook for {key} raised: {e}")
    return new_val


def reset() -> None:
    """Reset all settings to defaults and persist."""
    with _LOCK:
        for k, spec in _SCHEMA.items():
            _state[k] = spec["default"]
        _save_to_disk()
    log.info("[settings] reset to defaults")


def on_change(key: str, hook: Callable[[Any], None]) -> None:
    """Register a callback fired whenever `key` is set. Hook receives the new value."""
    if key not in _SCHEMA:
        raise SettingsError(f"unknown setting: {key!r}")
    _hooks.setdefault(key, []).append(hook)


_load_from_disk()
