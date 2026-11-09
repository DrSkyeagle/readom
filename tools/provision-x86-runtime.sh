#!/bin/bash
# Run only in this project's ARM64 Lima guest. Needed by AOSP's pinned GCC 4.9.
set -euo pipefail
if [[ "$(uname -s)" != Linux || "$(hostname)" != ${READOM_BUILD_HOST:-lima-pw3} || "$(id -u)" != 0 ]]; then
  echo 'Run with sudo inside the lima-pw3 build VM.' >&2
  exit 1
fi
export DEBIAN_FRONTEND=noninteractive
export NEEDRESTART_MODE=a
python3 - <<'PY'
from pathlib import Path
p = Path('/etc/apt/sources.list')
s = p.read_text()
s = s.replace('deb http://ports.ubuntu.com/', 'deb [arch=arm64] http://ports.ubuntu.com/')
s = s.replace('deb-src http://ports.ubuntu.com/', 'deb-src [arch=arm64] http://ports.ubuntu.com/')
p.write_text(s)
Path('/etc/apt/sources.list.d/pw3-build-amd64.list').write_text(
    'deb [arch=amd64] https://archive.ubuntu.com/ubuntu jammy main universe\n'
    'deb [arch=amd64] https://archive.ubuntu.com/ubuntu jammy-updates main universe\n'
    'deb [arch=amd64] https://security.ubuntu.com/ubuntu jammy-security main universe\n'
)
PY
dpkg --add-architecture amd64
apt-get update
apt-get install -y --no-install-recommends qemu-user-static binfmt-support libc6:amd64 libstdc++6:amd64 zlib1g:amd64
