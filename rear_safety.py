"""
Rear Safety Loop — Smart approach detection for Raspberry Pi 4B
================================================================
Runs on the Pi belt unit. Monitors a rear-facing HC-SR04 ultrasonic sensor
and drives a vibration motor via PWM proportional to obstacle proximity.

Uses the same state-machine logic as the RP2040 forward safety modules
(safety_loop_rp2040.py) but adapted for RPi.GPIO and a vibration motor
instead of a buzzer.

State Machine:
  IDLE     — nothing in range (<100cm), motor off
  NOTICED  — object entered 100cm AND closing at >15cm/s
             → short vibration pulse to notify user
  TRACKING — object breached 70cm inner boundary
             → vibration intensity proportional to distance
             → static objects (still for 5s) silence the motor
             → movement resumes → motor reactivates

Vibration Intensity (TRACKING state):
  <20cm  = 100% duty (danger — stop!)
  <40cm  = 75% duty (close)
  <70cm  = 50% duty (warning)
  <100cm = 25% duty (far)
  >100cm = off

Hardware:
  Sensor:  HC-SR04 — TRIG=GPIO5, ECHO=GPIO6
  Motor:   Vibration motor via MOSFET — GPIO12 (PWM, 1kHz)

Run:  sudo python3 rear_safety.py
Deploy as service:
  sudo cp rear_safety.service /etc/systemd/system/
  sudo systemctl enable rear_safety
  sudo systemctl start rear_safety

Note: AI assistance (Claude) was used to help develop this script.
"""

import RPi.GPIO as GPIO
import time

GPIO.setmode(GPIO.BCM)
GPIO.setwarnings(False)

# --- Pins ---
TRIG = 5
ECHO = 6
MOTOR = 12  # PWM-capable pin for vibration motor

GPIO.setup(TRIG, GPIO.OUT)
GPIO.setup(ECHO, GPIO.IN)
GPIO.setup(MOTOR, GPIO.OUT)

GPIO.output(TRIG, False)
GPIO.output(MOTOR, False)

# PWM for vibration motor (1kHz carrier frequency)
motor_pwm = GPIO.PWM(MOTOR, 1000)
motor_pwm.start(0)

# --- Tunable parameters ---
NOTICE_DIST = 100       # cm — outer detection boundary
TRACK_DIST = 70         # cm — inner boundary, activates proportional vibration
APPROACH_RATE = -15.0   # cm/s — closing speed threshold to trigger NOTICED
HISTORY_SIZE = 10       # Rolling window for smoothing + velocity calculation
APPROACH_COUNT = 3      # Consecutive approaching readings needed before NOTICED
NO_READING_TIMEOUT = 0.5  # seconds — reset to IDLE if sensor goes silent
STATIC_TIMEOUT = 5.0    # seconds — if object holds still this long, go quiet
POLL_INTERVAL = 0.01    # seconds — sensor poll rate (~100Hz)

# Vibration intensity by zone (duty cycle percentage)
ZONE_DUTY = [
    (20,  100),   # DANGER: full vibration
    (40,  75),    # CLOSE: strong vibration
    (70,  50),    # WARNING: moderate vibration
    (100, 25),    # FAR: light vibration
]

# Notify pulse: 3 quick vibrations (duration in seconds)
NOTIFY_PATTERN = [0.08, 0.08, 0.08, 0.08, 0.08]  # on, off, on, off, on

# States
IDLE = 0
NOTICED = 1
TRACKING = 2
state_names = {IDLE: "IDLE", NOTICED: "NOTICED", TRACKING: "TRACKING"}

# --- Sensor ---
def get_distance():
    """Measure distance using HC-SR04. Returns cm or None on timeout."""
    GPIO.output(TRIG, True)
    time.sleep(0.00001)  # 10us trigger pulse
    GPIO.output(TRIG, False)

    # Wait for echo to go HIGH
    timeout = time.time() + 0.02
    while GPIO.input(ECHO) == 0:
        pulse_start = time.time()
        if time.time() > timeout:
            return None

    # Measure echo HIGH duration
    timeout = time.time() + 0.02
    while GPIO.input(ECHO) == 1:
        pulse_end = time.time()
        if time.time() > timeout:
            return None

    # Convert to cm: sound speed ~343m/s, round trip / 2
    distance = (pulse_end - pulse_start) * 17150
    return round(distance, 2)


# --- History + velocity ---
history = []  # [(distance_cm, timestamp_s), ...]

def update_history(dist):
    now = time.time()
    history.append((dist, now))
    if len(history) > HISTORY_SIZE:
        history.pop(0)

def avg_distance():
    if not history:
        return None
    return sum(d for d, _ in history) / len(history)

def approach_velocity():
    """cm/s from oldest to newest. Negative = closing."""
    if len(history) < 3:
        return 0.0
    d_old, t_old = history[0]
    d_new, t_new = history[-1]
    dt = t_new - t_old
    if dt < 0.01:
        return 0.0
    return (d_new - d_old) / dt


# --- Motor control ---
def set_motor(duty):
    """Set vibration motor duty cycle (0-100)."""
    motor_pwm.ChangeDutyCycle(duty)

def get_zone_duty(dist):
    """Return vibration duty cycle for distance. 0 = off."""
    if dist is None:
        return 0
    for max_dist, duty in ZONE_DUTY:
        if dist < max_dist:
            return duty
    return 0


# --- Main loop ---
print()
print("  Rear Safety Loop — vibration motor")
print(f"  Sensor: GPIO{TRIG}/{ECHO}")
print(f"  Motor:  GPIO{MOTOR} (PWM)")
print(f"  Zones: <20cm=100%, <40cm=75%, <70cm=50%, <100cm=25%")
print()

state = IDLE
approach_streak = 0
last_reading_time = time.time()
static_since = 0
is_static = False

# Notify state
notify_step = -1
notify_start = 0.0

# Let sensor settle
time.sleep(2)

try:
    print_throttle = 0

    while True:
        now = time.time()
        dist = get_distance()

        # Update history
        if dist is not None:
            last_reading_time = now
            update_history(dist)

        smooth = avg_distance()
        vel = approach_velocity()

        # No reading timeout → IDLE
        if now - last_reading_time > NO_READING_TIMEOUT:
            if state != IDLE:
                state = IDLE
                set_motor(0)
                history.clear()

        # --- State machine ---
        if state == IDLE:
            set_motor(0)
            if smooth is not None and smooth < NOTICE_DIST and vel < APPROACH_RATE:
                approach_streak += 1
            else:
                approach_streak = 0
            if approach_streak >= APPROACH_COUNT:
                state = NOTICED
                notify_step = 0
                notify_start = now
                approach_streak = 0

        elif state == NOTICED:
            # Run notify pulse pattern (non-blocking)
            notifying = False
            if notify_step >= 0:
                elapsed = now - notify_start
                total = 0
                for i, duration in enumerate(NOTIFY_PATTERN):
                    total += duration
                    if elapsed < total:
                        # Even indices = motor on, odd = off
                        set_motor(80 if i % 2 == 0 else 0)
                        notifying = True
                        break
                if not notifying:
                    set_motor(0)
                    notify_step = -1

            if smooth is None or smooth >= NOTICE_DIST:
                # Object left
                state = IDLE
                set_motor(0)
                notify_step = -1
                is_static = False
                static_since = 0
            elif smooth < TRACK_DIST:
                # Breached inner boundary
                state = TRACKING
                notify_step = -1
                static_since = 0
                is_static = False
            elif not notifying and vel >= APPROACH_RATE:
                # Stopped approaching after notify
                state = IDLE
                set_motor(0)

        elif state == TRACKING:
            if smooth is None or smooth >= NOTICE_DIST:
                # Object left
                state = IDLE
                set_motor(0)
                is_static = False
                static_since = 0
            else:
                # Check distance stability (same logic as RP2040 version)
                dist_range = 0
                if len(history) >= 2:
                    dists = [d for d, _ in history]
                    dist_range = max(dists) - min(dists)

                if dist_range < 10:  # <10cm spread = stable
                    if static_since == 0:
                        static_since = now
                    if now - static_since >= STATIC_TIMEOUT:
                        if not is_static:
                            is_static = True
                            set_motor(0)
                    elif not is_static:
                        set_motor(get_zone_duty(smooth))
                else:
                    # Object moving
                    static_since = 0
                    if is_static:
                        is_static = False
                    set_motor(get_zone_duty(smooth))

        # Print status (~10Hz)
        if now - print_throttle >= 0.1:
            print_throttle = now
            if dist is not None:
                s = state_names[state]
                if is_static:
                    s += "/STATIC"
                print(f"{dist:6.1f} cm  vel:{vel:+6.1f} cm/s  [{s}]")
            else:
                print(f"  --  cm  [{state_names[state]}]")

        time.sleep(POLL_INTERVAL)

except KeyboardInterrupt:
    print("\nStopping...")

finally:
    motor_pwm.stop()
    GPIO.output(MOTOR, False)
    GPIO.cleanup()
    print("Rear safety loop stopped.")
