#!/usr/bin/env python3
"""Generate model sysfs labels from exact readlink lines in a device probe."""
import argparse
import hashlib
import json
import re
from pathlib import Path


def generate(report):
    classes = {"/sys/class/backlight/max77696-bl": "sysfs_leds"}
    classes.update({"/sys/class/power_supply/max77696-" + name: "sysfs_batteryinfo"
                    for name in ("battery", "charger", "eh", "uic")})
    observed = {}
    for line in report.splitlines():
        if " -> " not in line:
            continue
        source, target = line.split(" -> ", 1)
        if source not in classes:
            continue
        if source in observed:
            raise ValueError("Duplicate device path evidence")
        if not re.fullmatch(r"/sys/devices/[A-Za-z0-9_.:/-]+", target):
            raise ValueError("Invalid physical sysfs path")
        if any(part in (".", "..") for part in target.split("/")):
            raise ValueError("Noncanonical sysfs path")
        if not target.endswith("/" + source.rsplit("/", 1)[1]):
            raise ValueError("Device class and target differ")
        observed[source] = target
    if observed.keys() != classes.keys():
        raise ValueError("Missing required backlight/power supply evidence")
    lines = ["# Model paths verified by exact device readlink evidence; no identifiers."]
    for source, kind in classes.items():
        lines.append(f"genfscon sysfs {observed[source][4:]} u:object_r:{kind}:s0")
    lines.append("genfscon sysfs " + observed["/sys/class/backlight/max77696-bl"][4:] +
                 "/bl_power u:object_r:sysfs_pw3_bl_power:s0")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args()
    raw = args.probe_report.read_bytes()
    result = generate(raw.decode()).encode()
    if args.output.exists() or args.receipt.exists():
        raise SystemExit("Refusing to overwrite existing generated evidence")
    args.output.write_bytes(result)
    args.receipt.write_text(json.dumps({"probe_sha256": hashlib.sha256(raw).hexdigest(),
        "generated_sha256": hashlib.sha256(result).hexdigest(),
        "scope": "Exact model sysfs paths only; no SELinux runtime claim"}, indent=2) + "\n")
    print("Generated five device labels and one frontlight power attribute label.")


if __name__ == "__main__":
    main()
