#!/bin/bash
# Download the fixed public Android baseline inside the Linux build filesystem.
set -euo pipefail
if [[ "$(uname -s)" != Linux || "$(hostname)" != lima-pw3 ]]; then
  echo 'Run inside the lima-pw3 build VM.' >&2
  exit 1
fi
AOSP_ROOT=/home/builder/android-src
mkdir -p "$AOSP_ROOT" /mnt/build-share/logs
cd "$AOSP_ROOT"
export GIT_AUTHOR_NAME='PW3 local build'
export GIT_AUTHOR_EMAIL='pw3-build@localhost'
export GIT_COMMITTER_NAME="$GIT_AUTHOR_NAME"
export GIT_COMMITTER_EMAIL="$GIT_AUTHOR_EMAIL"
repo init -u https://android.googlesource.com/platform/manifest \
  -b android-8.1.0_r81 --depth=1 --no-clone-bundle -g default,-darwin \
  2>&1 | tee /mnt/build-share/logs/aosp-init.log
git -C .repo/manifests rev-parse HEAD > /mnt/build-share/logs/aosp-manifest-commit.txt
repo sync -c -j4 --no-tags --no-clone-bundle --fail-fast \
  2>&1 | tee /mnt/build-share/logs/aosp-sync.log
repo manifest -r -o /mnt/build-share/aosp-8.1.0_r81-resolved.xml
