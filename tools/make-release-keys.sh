#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Generates the release keys (I8, docs/design/release-build.md) in the build VM, as AOSP's
# development/tools/make_key makes them (RSA 2048, SHA-256 certificates valid 10000 days):
# releasekey, platform, shared, media. The keys carry no password of their own; they are kept
# as a whole in the maintainer's encrypted archive, and on the build VM only while releases are
# built. Prints each certificate's SHA-256, which is public (README: how to check a download).
# Usage (in the VM, as builder): make-release-keys.sh DIR    DIR must not exist yet.
set -euo pipefail
dir=${1:?directory for the keys}
[[ ! -e "$dir" ]] || { echo "$dir exists: keys are never overwritten" >&2; exit 1; }
# AOSP's own script, reached through the chroot's bind mount of the source tree. That mount
# does not survive a reboot of the VM, and without it every key "fails" with no reason given:
# make_key's own noise is discarded (see below), so a missing file read as a failing tool.
make_key=/srv/aosp-x86/src/development/tools/make_key
[[ -r "$make_key" ]] || { echo "$make_key is not there: run tools/provision-aosp-chroot.sh first" >&2; exit 1; }
umask 077
mkdir -p "$dir"
for key in releasekey platform shared media; do
  # make_key asks for a password (an empty line: none) and always ends with status 1 (its
  # EXIT trap), so success is read from the files it writes.
  (cd "$dir" && echo | bash "$make_key" "$key" "/O=Readom/OU=$key/CN=Readom" rsa) >/dev/null 2>&1 || true
  [[ -s "$dir/$key.pk8" && -s "$dir/$key.x509.pem" ]] || { echo "make_key failed for $key" >&2; exit 1; }
done
# system/sepolicy's keys.conf names the default key's certificate testkey.x509.pem (@RELEASE,
# read by insertkeys.py for mac_permissions.xml, though no stanza uses it): here that is the
# release key's certificate. Only the public certificate.
ln -s releasekey.x509.pem "$dir/testkey.x509.pem"
for key in releasekey platform shared media; do
  printf '%-10s %s\n' "$key" "$(openssl x509 -in "$dir/$key.x509.pem" -noout -fingerprint -sha256 | cut -d= -f2)"
done
