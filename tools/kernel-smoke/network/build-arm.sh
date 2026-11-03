#!/bin/bash
# Standalone userspace compile only. Does not edit or build the kernel tree.
set -euo pipefail
[[ "$(uname -s)" == Linux && "$(hostname)" == lima-pw3 ]] || { echo 'Requires existing lima-pw3 build VM' >&2; exit 1; }
network_smoke_dir=$(cd -- "$(dirname -- "$0")" && pwd)
network_smoke_cc=${NETWORK_SMOKE_CC:-/home/builder/toolchains/musl-arm/bin/musl-gcc}
network_smoke_out=${NETWORK_SMOKE_OUT:-"$network_smoke_dir/build"}
mkdir -p -- "$network_smoke_out"
"$network_smoke_cc" -std=gnu99 -static -Os -Wall -Wextra -Werror \
  -idirafter /usr/arm-linux-gnueabi/include \
  "$network_smoke_dir/network-smoke.c" "$network_smoke_dir/netlink-wire.c" \
  -o "$network_smoke_out/network-smoke"
"$network_smoke_cc" -std=gnu99 -static -Os -Wall -Wextra -Werror \
  "$network_smoke_dir/wire-selftest.c" "$network_smoke_dir/netlink-wire.c" \
  -o "$network_smoke_out/network-wire-selftest"
file "$network_smoke_out/network-smoke" "$network_smoke_out/network-wire-selftest"
sha256sum "$network_smoke_out/network-smoke" "$network_smoke_out/network-wire-selftest"
