#!/bin/bash
# Rebuild the host ART compiler and its ARM boot image regression only.
# Does not prepare hardware, install firmware, or claim a full system build.
set -euo pipefail
[[ $(uname -s) == Linux && $(hostname) == lima-pw3 ]] || exit 77
exec 9>/home/builder/pw3-build/aosp-build.lock
flock -n 9 || { echo 'Another AOSP build is active.' >&2; exit 1; }
bash /mnt/port/tools/provision-native-clang.sh
python3 /mnt/port/tools/apply-patch-series.py /home/builder/android-src/art /mnt/port/patches/aosp-art/*.patch
sudo chroot /srv/aosp-x86 /usr/sbin/runuser -u builder -- /bin/bash -lc '
  set -eo pipefail
  ulimit -c 0
  cd /src
  export LC_ALL=C
  export JAVA_HOME=/src/prebuilts/jdk/jdk8/linux-x86
  export PW3_BUILD_GOROOT=/opt/go1.10.8
  if [[ -f /opt/pw3-native-clang-enabled.json ]]; then
    export PW3_NATIVE_CLANG_BIN=/opt/pw3-native-clang-prebuilts/linux-x86/clang-4053586/bin
  fi
  export JACK_SERVER_VM_ARGUMENTS="-Dfile.encoding=UTF-8 -XX:+TieredCompilation -Xmx3g"
  export PATH="$JAVA_HOME/bin:$PATH"
  bash /port/tools/start-native-jack.sh
  source build/envsetup.sh
  lunch pw3-userdebug
  export PATH="/opt/pw3-java-tools:$PATH"
  make -j6 out/host/linux-x86/bin/dex2oatd
  make -j6 out/target/product/pw3/dex_bootjars/system/framework/arm/boot.art
'
