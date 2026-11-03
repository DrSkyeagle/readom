#!/bin/bash
set -euo pipefail
PORT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT_ROOT="$(cd "$PORT_ROOT/.." && pwd)"
export LIMA_HOME="$PROJECT_ROOT/work/vm"
LIMACTL="${LIMACTL:-/opt/homebrew/bin/limactl}"
case "${1:-status}" in
  start)
    if [[ -f "$LIMA_HOME/pw3/lima.yaml" ]]; then
      exec "$LIMACTL" start --tty=false pw3
    fi
    VM_CONFIG="$PROJECT_ROOT/work/android-port/build-vm.generated.yaml"
    python3 "$PORT_ROOT/tools/render-vm-config.py" --output "$VM_CONFIG"
    exec "$LIMACTL" start --name=pw3 --tty=false "$VM_CONFIG"
    ;;
  stop)
    exec "$LIMACTL" stop pw3
    ;;
  status)
    exec "$LIMACTL" list pw3
    ;;
  shell)
    shift
    exec "$LIMACTL" shell pw3 "$@"
    ;;
  *)
    echo 'Usage: vm.sh {start|stop|status|shell [command...]}' >&2
    exit 2
    ;;
esac
