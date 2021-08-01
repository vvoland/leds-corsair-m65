#!/usr/bin/env python3
import time
import random
import argparse

from colorsys import hsv_to_rgb

def hexify(number: int) -> str:
    return hex(number)[2:]


def main(args: argparse.Namespace):
    progress = 0.0
    s = args.saturation
    v = args.value
    dt = 1.0 / args.frames
    speed = args.speed
    zone_offset = args.offset

    zones = {
        "wheel": 0,
        "dpi"  : zone_offset * 1,
        "logo" : zone_offset * 2
    }

    zones_fd = {zone: open(f"/sys/class/leds/corsair_m65::{zone}/brightness", "w") for zone in zones}
    while True:
        for zone in zones.keys():

            zone_h = zones[zone] + progress
            if zone_h > 1.0:
                zone_h -= 1.0

            (r, g, b) = hsv_to_rgb(zone_h, s, v)

            rgb =  int(0xFF * r) << 16
            rgb += int(0xFF * g) << 8
            rgb += int(0xFF * b) << 0

            print(f"{rgb}", file=zones_fd[zone])
            zones_fd[zone].flush()

        time.sleep(dt)
        progress += dt * speed
        while progress > 1.0:
            progress -= 1.0


def float01(value: str) -> float:
    f = float(value)
    if f < 0.0 or f > 1.0:
        raise ValueError("Value in range [0, 1] was expected")
    return f


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Make mouse leds do a rainbow-like effect")
    parser.add_argument("--speed", default=0.3, dest="speed", help="Hue change speed", type=float01)
    parser.add_argument("--frames", default=20, dest="frames", help="Color changes per second", type=int)
    parser.add_argument("--saturation", default=0.90, dest="saturation", help="Color saturation", type=float01)
    parser.add_argument("--value", default=0.99, dest="value", help="Color value", type=float01)
    parser.add_argument("--offset", default=0.07, dest="offset", help="Zones hue offset", type=float01)

    args = parser.parse_args()
    main(args)
