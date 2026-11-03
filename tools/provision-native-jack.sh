#!/bin/bash
# Expose the VM's ARM64 Java 8 runtime read-only inside the amd64 build chroot.
# The Jack server, host javac and Doclava/javadoc use this runtime. Other AOSP Java tools retain
# the bundled x86 JVM and JNI libraries, with interpreted host bytecode.
set -euo pipefail
[[ "$(uname -s)" == Linux && "$(hostname)" == lima-pw3 ]] || exit 1
root=/srv/aosp-x86
jdk=/usr/lib/jvm/java-8-openjdk-arm64
test -x "$jdk/bin/java" || {
  echo 'Install the VM package openjdk-8-jdk-headless first.' >&2
  exit 1
}
test -x "$root/usr/sbin/runuser"
bind_ro() {
  local source="$1" target="$root$1"
  sudo mkdir -p "$target"
  if ! mountpoint -q "$target"; then
    sudo mount --bind "$source" "$target"
    sudo mount -o remount,bind,ro "$target"
  fi
}
bind_ro "$jdk"
bind_ro /etc/java-8-openjdk
bind_ro /usr/lib/aarch64-linux-gnu
if [[ "$(readlink -f /lib/aarch64-linux-gnu)" != /usr/lib/aarch64-linux-gnu ]]; then
  bind_ro /lib/aarch64-linux-gnu
elif [[ "$(readlink -f "$root/lib")" != "$root/usr/lib" ]]; then
  sudo mkdir -p "$root/lib"
  if [[ ! -e "$root/lib/aarch64-linux-gnu" ]]; then
    sudo ln -s /usr/lib/aarch64-linux-gnu "$root/lib/aarch64-linux-gnu"
  fi
fi
if [[ ! -e "$root/lib/ld-linux-aarch64.so.1" && ! -L "$root/lib/ld-linux-aarch64.so.1" ]]; then
  sudo ln -s /usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 "$root/lib/ld-linux-aarch64.so.1"
fi
mkdir -p /mnt/build-share/logs
sudo chroot "$root" /usr/sbin/runuser -u builder -- \
  "$jdk/bin/java" -XshowSettings:properties -version \
  > /mnt/build-share/logs/native-jack-jvm.txt 2>&1
grep -q 'os.arch = aarch64' /mnt/build-share/logs/native-jack-jvm.txt
dpkg-query -W -f='${Package} ${Version} ${Architecture}\n' \
  openjdk-8-jdk-headless openjdk-8-jre-headless \
  > /mnt/build-share/logs/native-jack-packages.txt
echo 'ARM64 Java 8 executes inside the build chroot; Jack has not been restarted.'
sudo mkdir -p "$root/opt/pw3-java-tools"
if [[ ! -L "$root/opt/pw3-java-tools/javac" ]]; then
  sudo ln -s "$jdk/bin/javac" "$root/opt/pw3-java-tools/javac"
fi
[[ "$(readlink "$root/opt/pw3-java-tools/javac")" == "$jdk/bin/javac" ]]
sudo install -m 0755 /mnt/port/tools/host-art/java "$root/opt/pw3-java-tools/java"
sudo install -m 0755 /mnt/port/tools/host-art/jar "$root/opt/pw3-java-tools/jar"
sudo install -m 0755 /mnt/port/tools/host-art/javadoc "$root/opt/pw3-java-tools/javadoc"
