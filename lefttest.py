"""
Pi GPIO Sensor Test — Ultrasonic + Buzzer + Rear Motor
========================================================
Runs on Raspberry Pi 4B. Tests up to 3 HC-SR04 ultrasonic sensors with
non-blocking buzzer control and PWM vibration motor for rear detection.

This is a standalone test script for Mohamed's Pi-side safety loop.
The production safety loop runs on RP2040 boards instead (safety_loop_rp2040.py).

Hardware:
  Left sensor:   TRIG=GPIO17, ECHO=GPIO27
  Right sensor:  TRIG=GPIO22, ECHO=GPIO23  (known issue: GPIO22/23 unreliable)
  Rear sensor:   TRIG=GPIO5,  ECHO=GPIO6   (known issue: erratic readings)
  Left buzzer:   GPIO20
  Right buzzer:  GPIO21
  Rear motor:    GPIO12 (PWM, proportional vibration intensity)

Run:  sudo python3 lefttest.py
Note: Requires root or gpio group for RPi.GPIO access on Bookworm.
"""
import RPi.GPIO as GPIO
import time

GPIO.setmode(GPIO.BCM)
GPIO.setwarnings(False)

# -------------------------
# Sensor pins
# -------------------------
LEFT_TRIG = 17
LEFT_ECHO = 27

RIGHT_TRIG = 22
RIGHT_ECHO = 23

REAR_TRIG = 5
REAR_ECHO = 6

# -------------------------
# Buzzer / motor pins
# -------------------------
LEFT_BUZZER = 20
RIGHT_BUZZER = 21
REAR_MOTOR = 12  # PWM-capable pin for vibration motor

# -------------------------
# Distance thresholds (cm)
# -------------------------
WARNING_DISTANCE = 100   # starts beeping
TOO_CLOSE_DISTANCE = 30  # stays on solid

# -------------------------
# Setup
# -------------------------
GPIO.setup(LEFT_TRIG, GPIO.OUT)
GPIO.setup(LEFT_ECHO, GPIO.IN)

GPIO.setup(RIGHT_TRIG, GPIO.OUT)
GPIO.setup(RIGHT_ECHO, GPIO.IN)

GPIO.setup(REAR_TRIG, GPIO.OUT)
GPIO.setup(REAR_ECHO, GPIO.IN)

GPIO.setup(LEFT_BUZZER, GPIO.OUT)
GPIO.setup(RIGHT_BUZZER, GPIO.OUT)
GPIO.setup(REAR_MOTOR, GPIO.OUT)

GPIO.output(LEFT_TRIG, False)
GPIO.output(RIGHT_TRIG, False)
GPIO.output(REAR_TRIG, False)

GPIO.output(LEFT_BUZZER, False)
GPIO.output(RIGHT_BUZZER, False)
GPIO.output(REAR_MOTOR, False)

# PWM for rear vibration motor (proportional intensity)
rear_pwm = GPIO.PWM(REAR_MOTOR, 1000)
rear_pwm.start(0)

time.sleep(2)

# -------------------------
# Distance function
# -------------------------
def get_distance(trig, echo):
    GPIO.output(trig, True)
    time.sleep(0.00001)
    GPIO.output(trig, False)

    pulse_start = time.time()
    pulse_end = time.time()

    timeout = time.time() + 0.02
    while GPIO.input(echo) == 0:
        pulse_start = time.time()
        if time.time() > timeout:
            return None

    timeout = time.time() + 0.02
    while GPIO.input(echo) == 1:
        pulse_end = time.time()
        if time.time() > timeout:
            return None

    pulse_duration = pulse_end - pulse_start
    distance = pulse_duration * 17150
    return round(distance, 2)

# -------------------------
# Non-blocking buzzer control
# -------------------------
# Track when each buzzer last toggled so we can beep without sleeping
buzzer_state = {LEFT_BUZZER: False, RIGHT_BUZZER: False}
buzzer_toggle_time = {LEFT_BUZZER: 0.0, RIGHT_BUZZER: 0.0}


def control_buzzer(distance, buzzer_pin):
    now = time.time()

    if distance is None or distance >= WARNING_DISTANCE:
        GPIO.output(buzzer_pin, False)
        buzzer_state[buzzer_pin] = False
        return

    if distance < TOO_CLOSE_DISTANCE:
        # Very close = solid on
        GPIO.output(buzzer_pin, True)
        buzzer_state[buzzer_pin] = True
        return

    # Warning zone (30-100cm): beep rate proportional to distance
    # Closer = faster beep. Map 100cm -> 0.4s period, 30cm -> 0.1s period
    t = (distance - TOO_CLOSE_DISTANCE) / (WARNING_DISTANCE - TOO_CLOSE_DISTANCE)
    period = 0.1 + t * 0.3  # 0.1s at 30cm, 0.4s at 100cm
    half_period = period / 2

    if now - buzzer_toggle_time[buzzer_pin] >= half_period:
        buzzer_state[buzzer_pin] = not buzzer_state[buzzer_pin]
        GPIO.output(buzzer_pin, buzzer_state[buzzer_pin])
        buzzer_toggle_time[buzzer_pin] = now


def control_rear_motor(distance):
    """Vibration intensity proportional to proximity."""
    if distance is None or distance >= WARNING_DISTANCE:
        rear_pwm.ChangeDutyCycle(0)
        return

    if distance < TOO_CLOSE_DISTANCE:
        rear_pwm.ChangeDutyCycle(100)
        return

    # Map 100cm -> 10% duty, 30cm -> 100% duty
    t = 1.0 - (distance - TOO_CLOSE_DISTANCE) / (WARNING_DISTANCE - TOO_CLOSE_DISTANCE)
    duty = 10 + t * 90
    rear_pwm.ChangeDutyCycle(duty)


# -------------------------
# Main loop
# -------------------------
try:
    while True:
        left_distance = get_distance(LEFT_TRIG, LEFT_ECHO)
        time.sleep(0.03)  # let ultrasonic pulse dissipate before next trigger
        right_distance = get_distance(RIGHT_TRIG, RIGHT_ECHO)
        time.sleep(0.03)
        rear_distance = get_distance(REAR_TRIG, REAR_ECHO)
        time.sleep(0.03)

        control_buzzer(left_distance, LEFT_BUZZER)
        control_buzzer(right_distance, RIGHT_BUZZER)
        control_rear_motor(rear_distance)

        # Print distances
        left_str = f"{left_distance} cm" if left_distance is not None else "No reading"
        right_str = f"{right_distance} cm" if right_distance is not None else "No reading"
        rear_str = f"{rear_distance} cm" if rear_distance is not None else "No reading"
        print(f"Left: {left_str} | Right: {right_str} | Rear: {rear_str}")

        time.sleep(0.05)

except KeyboardInterrupt:
    print("\nProgram stopped.")

finally:
    rear_pwm.stop()
    GPIO.output(LEFT_BUZZER, False)
    GPIO.output(RIGHT_BUZZER, False)
    GPIO.cleanup()
