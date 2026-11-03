# Security

## Reporting

Open a GitHub issue. If what you found could be used to read or destroy data
on somebody else's device, leave the details out: say in the issue that there
is a security problem to discuss privately, and a maintainer will answer with
a private channel.

This project has no server, collects nothing and reports nothing anywhere. Its
attack surface is three things: the two files the owner copies onto the Kindle,
the system that runs on the device afterwards, and this repository's releases.

## What the system does to the device, and what it does not

- **It writes no partition and changes no bootloader.** Android's system and
  data are two ordinary files on the Kindle's user area (FAT). Starting it is
  tapping an entry in the stock library; the stock kernel loads this project's
  kernel and jumps to it.
- **Hold the power button down** until the screen changes and you are back on
  the stock system. No computer, nothing to open.
- **It does not touch the stock system's files**, nor anything in the owner's
  library.
- The installer makes and maintains one directory in the user area: `android/`.

## Per-device data

Neither this repository nor any release contains Amazon's private files: the
panel waveform, the Wi-Fi firmware and calibration, the stock private kernel
modules. They can only be taken off the owner's own device at install time, and
what is taken stays on the device.

Nor does anything that differs from device to device: MAC, serial number, board
ID, ATAGs, eMMC layout, the data image's UUID. The entry generates them on the
machine at every start (`boot/devicedata/`).

## The privacy check before a commit

`tools/check-private-data.py` reads what would be published -- the files git
tracks or would add, leaving out what `.gitignore` excludes -- and returns 1 on
a finding:

- **General rules**: MAC addresses, private keys, adb keys, Kindle serial
  numbers, home folder paths, e-mail addresses, LAN addresses, Wi-Fi
  passphrases. Hits known to be harmless are listed in
  `tools/private-data-allow.txt`.
- **This developer's own values**: a device's serial number, its MAC, a network
  name and so on, kept as SHA-256 only, outside the repository in
  `private/denylist.json` (which git ignores). The values themselves are never
  written down. Add one with `--add mac` or `--add text`, from standard input.

It is both a pre-commit hook and an entry in the suite
(`run-tests.py --filter private-data`). `--history` runs the same two checks
over every version of every file in every commit, because that is the other
half of what making a repository public publishes.

## Signing

The system image is signed with this project's own set of AOSP keys
(`releasekey`, `platform`, `shared`, `media`). **The certificate fingerprints
are public**, written down in `docs/design/release-build.en.md`, so anybody can
check that the version they installed is the one this project signed.

The private keys are **not in this repository and not in the working tree**:
they live in a directory outside it and reach the build through a read-only
mount. `tools/build-aosp-system.sh` refuses two things -- a key directory
inside the source tree or the shared build directory, and AOSP's public
`testkey`, whose private half ships with AOSP, so that anybody could replace an
image signed with it.

**What rotating the keys costs**: on an upgrade, a system application with a
`sharedUserId` is judged to have the wrong signature and its data is cleared.
So the answer to a leaked key is "publish a new fingerprint and offer a clean
install", not a quiet swap. How they are kept is in
`docs/design/release-build.en.md`.

## Integrity of a release

Every release gives the SHA-256 of two files: the entry `Readom.sh` and the
system payload `readom-system-<version>.img.zst`. The installer checks the
payload's hash on the device before unpacking it and after; a mismatch is not
installed, and the reason is written into `android/logs/`.

## What the system does not do

- It asks for and stores no account credentials.
- It has no telemetry, no crash reporting and no update check.
- **SELinux is enforcing**, and the command line cannot change that. In the
  `user` build a release is made from, AOSP's init ignores
  `androidboot.selinux=permissive`; only userdebug and eng builds honour it. The
  handover entry also compares the whole kernel command line against the SHA-256
  recorded at build time and refuses to start on any difference
  (`device/amazon/pw3/preinit/TestFileInit.h`). The one permissive domain is
  `su`, which exists only in a `userdebug` build and is not in the `user` build
  a release is made from.
- The browser and the input method are third-party or AOSP components and
  behave under their own licences and their own terms; see `THIRD_PARTY.en.md`.
