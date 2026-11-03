#!/usr/bin/env python3
"""Merge small config fragments, or reject options dropped by Kconfig.

More than one fragment because two different questions are being answered. One
is "what does this port need of any kernel it runs on" -- Binder, ashmem, the
cgroups Android expects, SELinux -- and it is the same on every Kindle. The
other is "what is true of this machine": whether it has the Wi-Fi chip whose
firmware policy a patch here implements, how many bits of mmap randomisation its
address-space split allows, and which model the kernel should call itself. Those
belong to the model, and keeping them in the one list is how a kernel built for
another Kindle came out calling itself `-lab126-pw3-android81` while asking for a
chip that board does not have (docs/design/porting-to-other-models.md).

An option named by two fragments is refused rather than resolved by order. The
value would then depend on the order of the arguments, and the reason for it
would be in two places with nothing saying which one applies: whoever changes the
base would have no way to see that a model had quietly overridden it. A model
that needs a different value needs the base not to have one.
"""
import argparse
import re
from pathlib import Path


def setting(line):
    match = re.fullmatch(r"(CONFIG_[A-Z0-9_]+)=(.*)", line)
    if match:
        return match.group(1), match.group(2)
    match = re.fullmatch(r"# (CONFIG_[A-Z0-9_]+) is not set", line)
    return (match.group(1), "n") if match else None


def read_fragments(paths):
    requested = {}
    source = {}
    for path in paths:
        for key, value in filter(None, map(setting, path.read_text().splitlines())):
            if key in requested:
                raise SystemExit("%s is asked for twice in %s" % (key, path)
                                 if source[key] == path else
                                 "%s is requested by both %s and %s; one of them has to "
                                 "stop asking" % (key, source[key], path))
            requested[key] = value
            source[key] = path
    return requested


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("config", type=Path)
    parser.add_argument("fragment", type=Path, nargs="+")
    args = parser.parse_args()
    lines = args.config.read_text().splitlines()
    requested = read_fragments(args.fragment)
    if not requested:
        raise SystemExit("Empty config fragment")
    if args.check:
        actual = dict(filter(None, map(setting, lines)))
        missing = [f"{key}: requested {value}, got {actual.get(key, 'n')}"
                   for key, value in requested.items() if actual.get(key, "n") != value]
        if missing:
            raise SystemExit("Kconfig did not preserve required options:\n" + "\n".join(missing))
        print(f"Verified {len(requested)} requested Kconfig options")
        return
    kept = [line for line in lines if not setting(line) or setting(line)[0] not in requested]
    kept.extend(f"# {key} is not set" if value == "n" else f"{key}={value}"
                for key, value in requested.items())
    args.config.write_text("\n".join(kept) + "\n")


if __name__ == "__main__":
    main()
