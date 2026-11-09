#!/bin/bash
# Linux VM only. Keep legacy x86 AOSP host tools separate from the ARM64 OS.
set -euo pipefail
[[ "$(uname -s)" == Linux && "$(hostname)" == ${READOM_BUILD_HOST:-lima-pw3} ]] || exit 1
ROOT=/srv/aosp-x86
[[ -x "$ROOT/bin/bash" ]] || { echo 'Bootstrap the pinned Ubuntu Jammy amd64 root first.' >&2; exit 1; }
for item in dev proc sys; do
  sudo mkdir -p "$ROOT/$item"
  if ! mountpoint -q "$ROOT/$item"; then
    sudo mount --rbind "/$item" "$ROOT/$item"
    sudo mount --make-rslave "$ROOT/$item"
  fi
done
# release-keys is the signing keys, mounted read-only and kept outside the
# source tree: a key inside it would be held out of a commit by .gitignore
# alone, and a filter is weaker than a location (docs/design/release-build.md).
# It is absent on a machine that has no keys, and a userdebug build does not
# want them, so a missing source is not an error here -- build-aosp-system.sh
# is what refuses a release build without them.
for pair in /home/builder/android-src:src /mnt/port:port /mnt/build-share:build-share \
            /mnt/release-keys:release-keys; do
  source_path="${pair%:*}"
  target_path="$ROOT/${pair##*:}"
  [[ -d "$source_path" ]] || continue
  sudo mkdir -p "$target_path"
  mountpoint -q "$target_path" || sudo mount --bind "$source_path" "$target_path"
done
sudo cp /etc/resolv.conf "$ROOT/etc/resolv.conf"
sudo tee "$ROOT/etc/apt/sources.list" >/dev/null <<'SOURCES'
deb https://archive.ubuntu.com/ubuntu jammy main universe
deb https://archive.ubuntu.com/ubuntu jammy-updates main universe
deb https://security.ubuntu.com/ubuntu jammy-security main universe
SOURCES
sudo chroot "$ROOT" /bin/bash -s <<'CHROOT'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential git python2.7 python3 \
  bison flex gperf zip unzip curl ca-certificates zlib1g-dev libc6-dev-i386 \
  lib32z1-dev lib32stdc++6 libncurses5 libncurses5-dev lib32ncurses-dev \
  libx11-dev x11proto-core-dev libgl1-mesa-dev libxml2-utils xsltproc \
  rsync bc cpio file patch procps locales ripgrep
[[ -e /usr/bin/python ]] || ln -s python2.7 /usr/bin/python
id builder >/dev/null 2>&1 || useradd -m -u 1000 -s /bin/bash builder
sed -i 's/^# en_US.UTF-8 UTF-8/en_US.UTF-8 UTF-8/' /etc/locale.gen
locale-gen en_US.UTF-8
CHROOT
