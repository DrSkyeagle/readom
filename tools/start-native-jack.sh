#!/bin/bash
# Invoked as the build user inside the project amd64 chroot.
set -euo pipefail
cd /src
test -x /usr/lib/jvm/java-8-openjdk-arm64/bin/java
export PATH="/usr/lib/jvm/java-8-openjdk-arm64/bin:$PATH"
export JACK_SERVER_VM_ARGUMENTS="-Dfile.encoding=UTF-8 -XX:+TieredCompilation -Xmx3g"
if [[ ! -d /home/builder/.jack-server ]]; then
  prebuilts/sdk/tools/jack-admin install-server \
    prebuilts/sdk/tools/jack-launcher.jar prebuilts/sdk/tools/jack-server-4.11.ALPHA.jar
fi
# Preserve an incomplete installation for diagnosis; never replace its keys.
test -s /home/builder/.jack-server/server.jks
test -s /home/builder/.jack-server/client.jks
python3 /port/tools/patch-jack-tls.py \
  prebuilts/sdk/tools/jack-server-4.11.ALPHA.jar \
  /home/builder/.jack-server/server-1.jar /build-share/logs/jack-tls-patch.json
# A persistent compiler daemon must not retain the enclosing build's lock.
prebuilts/sdk/tools/jack-admin start-server 9>&-
prebuilts/sdk/tools/jack-admin server-stat > /build-share/logs/jack-server-stat.txt
grep -q 'os.arch: aarch64' /build-share/logs/jack-server-stat.txt || {
  echo 'The existing Jack server is not native ARM64; stop that build service before retrying.' >&2
  exit 1
}
