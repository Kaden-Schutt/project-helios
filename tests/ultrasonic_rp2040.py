"""
Ultrasonic sensor test for RP2040 (MicroPython)
Supports 1-3 HC-SR04 sensors.

Wiring:
  Sensor 1 (Left):   TRIG=GP2,  ECHO=GP3
  Sensor 2 (Right):  TRIG=GP4,  ECHO=GP5
  Sensor 3 (Rear):   TRIG=GP6,  ECHO=GP7

Upload: copy to RP2040 as main.py
"""

import machine
import time

sensors = [
    {"name": "Left",  "trig": machine.Pin(2, machine.Pin.OUT), "echo": machine.Pin(3, machine.Pin.IN)},
    {"name": "Right", "trig": machine.Pin(0, machine.Pin.OUT), "echo": machine.Pin(1, machine.Pin.IN)},
    {"name": "Rear",  "trig": machine.Pin(6, machine.Pin.OUT), "echo": machine.Pin(7, machine.Pin.IN)},
]

# Set all triggers low
for s in sensors:
    s["trig"].value(0)

time.sleep(2)


def get_distance(trig, echo):
    trig.value(1)
    time.sleep_us(10)
    trig.value(0)

    # Wait for echo to go high
    t0 = time.ticks_us()
    while echo.value() == 0:
        if time.ticks_diff(time.ticks_us(), t0) > 20000:
            return None

    start = time.ticks_us()

    # Wait for echo to go low
    while echo.value() == 1:
        if time.ticks_diff(time.ticks_us(), start) > 20000:
            return None

    pulse = time.ticks_diff(time.ticks_us(), start)
    return round(pulse * 0.01715, 2)


print("Ultrasonic test — Ctrl+C to stop\n")

while True:
    parts = []
    for s in sensors:
        d = get_distance(s["trig"], s["echo"])
        if d is not None:
            parts.append(f"{s['name']}: {d} cm")
        else:
            parts.append(f"{s['name']}: --")
        time.sleep_ms(30)  # gap between sensors to prevent crosstalk

    print(" | ".join(parts))
    time.sleep_ms(50)
