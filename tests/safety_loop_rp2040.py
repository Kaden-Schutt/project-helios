"""
Safety Loop — Smart obstacle alert for RP2040 (MicroPython)
=============================================================
Runs on RP2040 (Raspberry Pi Pico) as the forward safety system for
Project Helios. This is one of three independent safety modules — each
has its own sensor and buzzer, runs on battery, no network needed.

Design philosophy: "parking sensor" style — don't annoy the user with
constant beeping for static objects (walls, furniture). Only alert when
something is actually approaching or dangerously close.

State Machine:
  IDLE     — nothing in range (<100cm), buzzer silent
  NOTICED  — object entered 100cm AND closing at >15cm/s
             → plays 3 quick notification pulses
             → if object stops approaching, returns to IDLE
  TRACKING — object breached 70cm inner boundary
             → tiered beeping based on distance (parking sensor style)
             → if object holds still for 5s, goes quiet (STATIC mode)
             → if object moves again, beeping resumes

Distance Zones (TRACKING state only):
  <20cm  = DANGER  — solid buzzer (stop!)
  <40cm  = CLOSE   — 100ms rapid beep
  <70cm  = WARNING — 300ms moderate beep
  <100cm = FAR     — 600ms slow beep
  >100cm = CLEAR   — silent

Anti-false-trigger measures:
  - 10-sample rolling history for smoothed distance + velocity
  - Approach velocity threshold (-15cm/s) exceeds sensor jitter (~6cm/s)
  - 3 consecutive approaching readings required before NOTICED
  - Static suppression: objects still for 5s stop beeping
  - Distance-range stability check (not velocity) for static detection

Hardware:
  Sensor:  HC-SR04 ultrasonic — TRIG=GP0, ECHO=GP1
  Buzzer:  Active-low 3-pin buzzer — GP8 (LOW=beep, HIGH=silent)
  Power:   3.3V from Pico or external battery

Deploy: mpremote cp safety_loop_rp2040.py :main.py
        (auto-runs on boot from any power source)
"""

import machine
import time

# --- Pins ---
trig = machine.Pin(0, machine.Pin.OUT)
echo = machine.Pin(1, machine.Pin.IN)
buzzer = machine.Pin(8, machine.Pin.OUT)

trig.value(0)
buzzer.value(1)  # active-low: start silent

# --- Config ---
# --- Tunable parameters ---
# These control sensitivity. Adjust based on real-world testing.
NOTICE_DIST = 100       # cm — outer boundary. Objects beyond this are ignored.
TRACK_DIST = 70         # cm — inner boundary. Crossing this activates tiered beeping.
APPROACH_RATE = -15.0   # cm/s — how fast an object must approach to trigger NOTICED.
                        #         Negative = closing. Set above jitter floor (~6cm/s).
HISTORY_SIZE = 10       # Number of distance samples in rolling window.
                        #         Larger = smoother but slower to react.
APPROACH_COUNT = 3      # Consecutive approaching readings needed before triggering.
                        #         Prevents single-sample false positives.
NO_READING_TIMEOUT = 500  # ms — if sensor returns no reading for this long, reset to IDLE.
STATIC_TIMEOUT = 5000    # ms — if object holds still this long in TRACKING, go quiet.
STATIC_THRESHOLD = 8.0   # cm/s — unused (kept for reference). Static detection now uses
                         #         distance-range method instead of velocity.

# Tiered beep zones (only active in TRACKING state)
ZONES = [
    (20,  0),      # DANGER: solid on (<8in)
    (40,  100),    # CLOSE: rapid beep
    (70,  300),    # WARNING: moderate beep
    (100, 600),    # FAR: slow beep
]

# Notify beep pattern: 3 quick pulses
NOTIFY_PATTERN = [80, 80, 80, 80, 80]  # on, off, on, off, on (ms)

# States
IDLE = 0
NOTICED = 1
TRACKING = 2

state = IDLE
state_names = {IDLE: "IDLE", NOTICED: "NOTICED", TRACKING: "TRACKING"}

# History buffer: (distance_cm, timestamp_ms)
history = []
last_reading_time = 0
approach_streak = 0  # consecutive readings with vel < APPROACH_RATE
static_since = 0     # timestamp when object stopped moving in TRACKING
is_static = False    # True = object is stationary, suppress beeping
notify_start = 0
notify_step = -1

# Buzzer state
buzzer_on = False
last_toggle = time.ticks_ms()


def get_distance():
    """Measure distance using HC-SR04 ultrasonic sensor.

    Sends a 10us trigger pulse, then times how long the echo pin stays high.
    Sound travels at ~343m/s, so distance = (pulse_us * 0.0343) / 2 = pulse * 0.01715 cm.
    Returns None if no echo received within 25ms (~4.3m max range).
    """
    trig.value(1)
    time.sleep_us(10)
    trig.value(0)

    # Wait for echo pin to go HIGH (start of return pulse)
    t0 = time.ticks_us()
    while echo.value() == 0:
        if time.ticks_diff(time.ticks_us(), t0) > 25000:  # 25ms timeout
            return None

    # Measure how long echo stays HIGH
    start = time.ticks_us()
    while echo.value() == 1:
        if time.ticks_diff(time.ticks_us(), start) > 25000:
            return None

    pulse = time.ticks_diff(time.ticks_us(), start)
    return pulse * 0.01715  # Convert microseconds to centimeters


def avg_distance():
    """Smoothed distance from history."""
    if not history:
        return None
    return sum(d for d, _ in history) / len(history)


def approach_velocity():
    """cm/s from oldest to newest in history. Negative = closing."""
    if len(history) < 3:
        return 0.0
    d_old, t_old = history[0]
    d_new, t_new = history[-1]
    dt = time.ticks_diff(t_new, t_old) / 1000.0  # seconds
    if dt < 0.01:
        return 0.0
    return (d_new - d_old) / dt


def set_buzzer(on):
    global buzzer_on
    buzzer_on = on
    buzzer.value(0 if on else 1)  # active-low: LOW = beep, HIGH = silent


def run_notify_beep(now):
    """Non-blocking 3-pulse notify pattern. Returns True while active."""
    global notify_step, notify_start
    if notify_step < 0:
        return False
    elapsed = time.ticks_diff(now, notify_start)
    # Walk through pattern
    total = 0
    for i, duration in enumerate(NOTIFY_PATTERN):
        total += duration
        if elapsed < total:
            set_buzzer(i % 2 == 0)  # even indices = on
            return True
    # Pattern done
    set_buzzer(False)
    notify_step = -1
    return False


def start_notify(now):
    global notify_step, notify_start
    notify_step = 0
    notify_start = now


def tiered_beep(dist, now):
    """Run tiered beep based on distance."""
    global buzzer_on, last_toggle
    period = None
    if dist is not None:
        for max_dist, p in ZONES:
            if dist < max_dist:
                period = p
                break

    if period is None:
        set_buzzer(False)
    elif period == 0:
        set_buzzer(True)
    else:
        half = period // 2
        if time.ticks_diff(now, last_toggle) >= half:
            set_buzzer(not buzzer_on)
            last_toggle = now


# --- Main loop ---
print("Safety Loop — smart approach detection")
print("  Sensor: GP0/GP1")
print("  Buzzer: GP8")
print("  IDLE → detect <100cm + approaching → NOTICED (one beep)")
print("  NOTICED → breaches 70cm → TRACKING (tiered beep)")
print()

print_throttle = 0

while True:
    now = time.ticks_ms()
    dist = get_distance()

    # Update history
    if dist is not None:
        last_reading_time = now
        history.append((dist, now))
        if len(history) > HISTORY_SIZE:
            history.pop(0)

    smooth = avg_distance()
    vel = approach_velocity()

    # No reading timeout → IDLE
    if time.ticks_diff(now, last_reading_time) > NO_READING_TIMEOUT:
        if state != IDLE:
            state = IDLE
            set_buzzer(False)
            history.clear()

    # --- State machine ---
    if state == IDLE:
        set_buzzer(False)
        if smooth is not None and smooth < NOTICE_DIST and vel < APPROACH_RATE:
            approach_streak += 1
        else:
            approach_streak = 0
        if approach_streak >= APPROACH_COUNT:
            state = NOTICED
            start_notify(now)
            approach_streak = 0

    elif state == NOTICED:
        # Run the notify beep pattern
        notifying = run_notify_beep(now)

        if smooth is None or smooth >= NOTICE_DIST:
            # Object left — back to idle
            state = IDLE
            set_buzzer(False)
            notify_step = -1
            is_static = False
            static_since = 0
        elif smooth < TRACK_DIST:
            # Breached inner boundary — go to tracking
            state = TRACKING
            notify_step = -1
            static_since = 0
            is_static = False
        elif not notifying and vel >= APPROACH_RATE:
            # Notify done, object stopped approaching — back to idle
            state = IDLE
            set_buzzer(False)

    elif state == TRACKING:
        if smooth is None or smooth >= NOTICE_DIST:
            # Object left
            state = IDLE
            set_buzzer(False)
            is_static = False
            static_since = 0
        else:
            # Check if distance is stable (compare oldest vs newest in history)
            dist_range = 0
            if len(history) >= 2:
                dists = [d for d, _ in history]
                dist_range = max(dists) - min(dists)

            if dist_range < 10:  # <10cm spread across whole window = stable
                if static_since == 0:
                    static_since = now
                if time.ticks_diff(now, static_since) >= STATIC_TIMEOUT:
                    if not is_static:
                        is_static = True
                        set_buzzer(False)
                elif not is_static:
                    tiered_beep(smooth, now)
            else:
                # Object is moving significantly
                static_since = 0
                if is_static:
                    is_static = False
                tiered_beep(smooth, now)

    # Print (~10Hz to not flood)
    if time.ticks_diff(now, print_throttle) >= 100:
        print_throttle = now
        if dist is not None:
            s = state_names[state]
            if is_static:
                s += "/STATIC"
            print(f"{dist:6.1f} cm  vel:{vel:+6.1f} cm/s  [{s}]")
        else:
            print(f"  --  cm  [{state_names[state]}]")

    time.sleep_ms(10)
