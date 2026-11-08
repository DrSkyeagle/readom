#!/bin/bash
# Build file images only. No USB commands, flashing, or partition operations.
set -euo pipefail
[[ "$(uname -s)" == Linux && "$(hostname)" == lima-pw3 ]] || exit 1
# Options come from the environment only. An argument of 1 once meant installclean and is now
# ignored silently by everything below; V49's first build lost its installclean that way.
(( $# == 0 )) || { echo 'No arguments: set PW3_AOSP_INSTALLCLEAN, PW3_BUILD_VARIANT, PW3_RELEASE_KEY_DIR, PW3_AOSP_PRODUCT.' >&2; exit 2; }
pw3_installclean=${PW3_AOSP_INSTALLCLEAN:-0}
[[ "$pw3_installclean" == 0 || "$pw3_installclean" == 1 ]] || exit 2
# userdebug: test images. user: release images (docs/design/release-build.md), signed with
# the keys in PW3_RELEASE_KEY_DIR, a path inside the build chroot (pw3.mk refuses a user build
# without them). Each variant keeps its own output tree: the one built last is out/, the other
# out-<variant>, swapped by renaming; the first build of a variant starts from a copy of the
# other's tree, so that it rebuilds only what the variant changes.
pw3_variant=${PW3_BUILD_VARIANT:-userdebug}
[[ "$pw3_variant" == user || "$pw3_variant" == userdebug ]] || exit 2
# Which product is built: the device directory's name under device/amazon/ and the
# product's in the AOSP tree (lunch "$product-$variant"), which is also the name of
# the output directory. Every model this project has is a product of its own that
# inherits pw3's (device/amazon/voyage/voyage.mk), so the default is the only one
# this project has run on a device.
pw3_product=${PW3_AOSP_PRODUCT:-pw3}
[[ "$pw3_product" =~ ^[a-z0-9][a-z0-9-]*$ ]] \
  || { echo 'PW3_AOSP_PRODUCT: the device directory name (lower-case letters, digits, "-")' >&2; exit 2; }
pw3_keys=${PW3_RELEASE_KEY_DIR:-}
# Two things the signing keys must not be, checked here rather than left to
# whoever sets the variable.
#
# Not inside this repository's working tree: a key that lives there is kept out
# of a commit by .gitignore alone, and a filter is a weaker protection than a
# location -- `git add -f`, an archive of the tree, or a copy of the directory
# all take it along. They live outside the tree and are mounted in.
#
# Not AOSP's own testkey: its private half is published with AOSP, so an image
# signed with it is one anybody can replace. The build does not notice, because
# as far as it is concerned the key is valid.
# The variable names a path as the chroot sees it, where /port is this
# repository and /build-share is the shared directory; from out here the same
# files are under /srv/aosp-x86.
# First, because everything below reads through the chroot. Its bind mounts do
# not survive a VM restart, and a chroot without them answers the key check with
# "no releasekey.pk8 in /release-keys" -- which sends the reader after the keys,
# which are where they always were (PW3 Android, 2026-10-28, one build lost to
# it). The later check of the same mount point guarded only the builds that pass
# no keys at all.
mountpoint -q /srv/aosp-x86/src || {
  echo 'The chroot is not provisioned: run tools/provision-aosp-chroot.sh first.' >&2
  echo 'Its bind mounts (/src, /port, /build-share, /release-keys) are gone after' >&2
  echo 'every VM restart.' >&2; exit 1; }
if [[ -n "$pw3_keys" ]]; then
  case "$pw3_keys/" in
    /port/*|/build-share/*)
      echo "PW3_RELEASE_KEY_DIR is $pw3_keys, which is inside the source tree or the" >&2
      echo 'shared build directory. Keep the keys outside both and mount them in' >&2
      echo '(docs/design/release-build.md).' >&2; exit 2 ;;
    /*) ;;
    *) echo 'PW3_RELEASE_KEY_DIR: an absolute path as the chroot sees it' >&2; exit 2 ;;
  esac
  pw3_keys_here=/srv/aosp-x86$pw3_keys
  for pw3_key in releasekey platform shared media; do
    [[ -f "$pw3_keys_here/$pw3_key.pk8" ]] ||
      { echo "PW3_RELEASE_KEY_DIR: no $pw3_key.pk8 in $pw3_keys" >&2; exit 2; }
  done
  if cmp -s "$pw3_keys_here/releasekey.pk8" \
            /srv/aosp-x86/src/build/target/product/security/testkey.pk8; then
    echo "PW3_RELEASE_KEY_DIR holds AOSP's published testkey. Its private half is" >&2
    echo 'public, so anybody could sign a replacement for this image.' >&2; exit 2
  fi
fi
# The moment the build stamps into ro.build.date.utc and the fingerprint built
# from it. Fixed here rather than left to the build so that the receipt can say
# which value the image carries, and so that a rebuild can be given the same
# one: the files in the image are then the same files.
#
# It does not make system.img byte-for-byte reproducible, and this comment used
# to say it did. The claim was that build_image.py hands the value to
# make_ext4fs -T; building twice with one value disproved it (docs/open-items.md
# section 12). The plain systemimage path passes neither -T nor -U, so
# make_ext4fs writes the time of the run into the inodes and derives the UUID
# itself. What a published image is checked against is the per-file sha256
# manifest, not the container's hash.
#
# It is not fixed to a constant either: the fingerprint it feeds has to differ
# between releases, or PackageManager does not treat a new image as a system
# update (see the comment on BUILD_NUMBER below).
pw3_datetime=${PW3_BUILD_DATETIME:-$(date +%s)}
[[ "$pw3_datetime" =~ ^[0-9]{9,11}$ ]] || { echo 'PW3_BUILD_DATETIME: seconds since the epoch' >&2; exit 2; }
# The one string in the system a reader can read out when they report a problem:
# Settings > About tablet > Build number, which is ro.build.display.id. Without
# it AOSP writes "$(BUILD_ID) $(BUILD_VERSION_TAGS)" there, which on this port
# is "OC dev-keys" and names no release (docs/open-items.md section 51). It is
# separate from BUILD_ID and from the fingerprint, so it changes neither the
# boot image nor whether PackageManager treats a boot as a system update; the
# only file it reaches is build.prop. Given here rather than left to the build
# so that the receipt can say which release the image calls itself.
pw3_version=${PW3_BUILD_VERSION:-}
[[ -z "$pw3_version" || "$pw3_version" =~ ^[A-Za-z0-9][A-Za-z0-9\ ._-]{0,30}$ ]] \
  || { echo 'PW3_BUILD_VERSION: letters, digits, space, dot, dash, underscore' >&2; exit 2; }
mkdir -p /home/builder/pw3-build
exec 9>/home/builder/pw3-build/aosp-build.lock
flock -n 9 || { echo 'Another AOSP build is active; source copy was not changed.' >&2; exit 1; }
bash /mnt/port/tools/provision-native-jack.sh
bash /mnt/port/tools/provision-native-clang.sh
bash /mnt/port/tools/prepare-aosp-device.sh
src=/home/builder/android-src
# out/.pw3-variant names the variant a tree belongs to; trees from before it were userdebug.
[[ -f "$src/out/.pw3-variant" ]] || echo userdebug > "$src/out/.pw3-variant"
current=$(cat "$src/out/.pw3-variant")
if [[ "$pw3_variant" != "$current" ]]; then
  [[ ! -e "$src/out-$current" ]] || { echo "out-$current exists; output trees are inconsistent" >&2; exit 1; }
  if [[ -d "$src/out-$pw3_variant" ]]; then
    mv "$src/out" "$src/out-$current"
    mv "$src/out-$pw3_variant" "$src/out"
  else
    cp -a "$src/out" "$src/out-$current"
    echo "$pw3_variant" > "$src/out/.pw3-variant"
  fi
fi
# A module that stops being installed leaves its file in the staged tree, and
# the image carries it (V49: etc/init/wifi-events.rc, deleted from this project
# a day earlier, whose 25 init commands all failed at every boot). installclean
# is the supported answer, and nothing made anyone pass it.
#
# There is no list in the tree of what the product means to install, so this
# cannot be checked after the fact: out/target/product/pw3/installed-files.txt
# is made by walking the staged directory ($(FILESLIST) $(TARGET_OUT), core
# Makefile line 1370), so it describes what is there, stale files included.
# What can be checked is the cause: the makefiles that decide the package list
# are stamped after a build that cleared the staged tree, and a later build
# that finds them different and was not asked to clear it stops here rather
# than packaging whatever is left over.
#
# Two directories, hashed with their paths, over the copies this build will
# actually read:
#   - device/amazon/pw3, rsynced out of this repository by
#     prepare-aosp-device.sh, and the model's own directory beside it when the
#     model is not pw3 (device/amazon/voyage, which inherits the pw3 product and
#     is therefore read by every build of it);
#   - vendor/amazon/pw3-hardware, generated by prepare-kernel-hardware.sh. Its
#     pw3-usb-component.mk is inherited by pw3.mk (line 407) and its
#     PRODUCT_COPY_FILES decide which init files are installed, so changing
#     export_component.py's list changes the install set without touching a
#     single device makefile.
#
# Three things this had wrong, all of which let a stale file through:
#   - it hashed /mnt/port, the host tree, although the build reads the copy.
#     Nothing stops the host tree from being edited between the copy and this
#     line, so the stamp could describe a configuration this build never
#     staged, and the next build would then find it unchanged;
#   - it concatenated contents only, so renaming a makefile -- which is what
#     decides whether AOSP finds a module at all -- left the fingerprint
#     identical, as did adding an empty one;
#   - it missed the generated component above.
#
# What this still does not cover: a package list that changes because an
# inherited AOSP makefile changed. That happens with tools/sync-aosp.sh, which
# replaces the source tree, and the first build after it is against a tree this
# stamp does not describe either -- it is written only by a clearing build.
pw3_config_stamp="$src/out/.pw3-package-config"
pw3_dirs=(device/amazon/pw3 vendor/amazon/pw3-hardware)
[[ "$pw3_product" == pw3 ]] || pw3_dirs=(device/amazon/pw3 "device/amazon/$pw3_product" vendor/amazon/pw3-hardware)
for pw3_dir in "${pw3_dirs[@]}"; do
  [[ -d "$src/$pw3_dir" ]] || { echo "$src/$pw3_dir is missing; the device synchronization did not run" >&2; exit 1; }
done
pw3_config_now=$(cd "$src" && find "${pw3_dirs[@]}" \
                 -name '*.mk' -print0 | sort -z | xargs -0 sha256sum \
                 | sha256sum | cut -d' ' -f1)
if [[ "$pw3_installclean" == 1 ]]; then
  # The stamp describes the staged tree, and this build is about to change it.
  # Taking it away first means a build that fails part way through -- or is
  # killed -- leaves no stamp claiming the tree still matches the configuration
  # it was staged from. Reverting the sources and building again without
  # installclean then stops below instead of packaging what the failed build
  # had already staged.
  rm -f "$pw3_config_stamp"
elif [[ ! -f "$pw3_config_stamp" ]]; then
  echo "No record of the package list this output tree was staged from." >&2
  echo "Build once with PW3_AOSP_INSTALLCLEAN=1; after that this is checked." >&2
  exit 3
elif [[ "$(cat "$pw3_config_stamp")" != "$pw3_config_now" ]]; then
  echo "The product makefiles changed since the staged tree was cleared." >&2
  echo "A module that no longer installs would leave its file behind and the" >&2
  echo "image would carry it. Build with PW3_AOSP_INSTALLCLEAN=1." >&2
  exit 3
fi
sudo chroot /srv/aosp-x86 /usr/sbin/runuser -u builder -- /bin/bash -lc '
  set -eo pipefail
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
  # Before lunch: it reads the product makefiles, and pw3.mk needs the keys for a user build.
  if [[ -n "$3" ]]; then export PW3_RELEASE_KEY_DIR="$3"; fi
  source build/envsetup.sh
  lunch "$6-$2"
  # Native host javac avoids the very slow emulated JVM bootstrap. Keep java
  # itself on the original x86 JDK for AOSP tools with x86 JNI dependencies.
  export PATH="/opt/pw3-java-tools:$PATH"
  # Every image gets its own build date and number, hence fingerprint. Two parts,
  # both needed (Test V42, three builds):
  # 1. They are given to kati here, before the first make. Kati computes them when
  #    it reads the makefiles, and main.mk and the core Makefile write them to
  #    out/build_date.txt, build_number.txt and build_fingerprint.txt with a $(shell)
  #    that kati runs again on every make to see whether to read the makefiles again,
  #    so files written by hand were put back. Kati notes the variables it reads: a
  #    new value makes it read the makefiles again (some minutes). Format of the
  #    AOSP default (version_defaults.mk).
  # 2. The files holding them are made again. Their rules read those values only
  #    when they run, so that a new build number alone rebuilds nothing (main.mk),
  #    and they run only when their other inputs change.
  # PackageManager handles a boot with a new fingerprint as a system update
  # (permissions and code caches of system apps), and logs tell builds apart by it.
  export BUILD_DATETIME="$4"
  export BUILD_NUMBER="eng.${USER:0:6}.$(date -d @$BUILD_DATETIME +%Y%m%d.%H%M%S)"
  # Only build.prop reads this one, so it is not part of either point above; it
  # is exported here with the others so that kati sees all three at once.
  if [[ -n "$5" ]]; then export BUILD_DISPLAY_ID="$5"; fi
  rm -f out/target/product/$6/obj/ETC/system_build_prop_intermediates/build.prop
  rm -f out/target/product/$6/root/default.prop
  # Package exclusions must not leave obsolete APKs in the staged system tree.
  # The supported target clears install outputs while retaining compiled objects.
  if [[ "${1:-0}" == 1 ]]; then make -j6 installclean; fi
  # Verify the boot entry, USB daemon/helper and full SELinux policy first.
  make -j6 init adbd pw3-testfile-init pw3-init-check pw3-usb-check sepolicy
  make -j6 systemimage
  # Android 8.1 mkbootfs reads the staged system fs_config_files, but the ramdisk
  # upstream dependency list does not include it. Regenerate this reproducible
  # output after staging system so root-entry permissions cannot stay stale.
  rm -f out/target/product/$6/ramdisk.img
  make -j6 ramdisk
  # Which module made each file of the image. A release has to hand out the source
  # of every copyleft file in it (collect-copyleft-sources.py), and the only thing
  # that says which module a file came from is this one; it has to come from the
  # build that made the image, and nothing else here asks for it. It describes the
  # build rather than taking part in it, so it changes neither image: it is made
  # after both, and the receipt below still names the images by their contents.
  #
  # Deleted first because its rule has no prerequisites (build/make/core/tasks/
  # module-info.mk): make calls the file up to date for as long as it exists, so
  # one left by an earlier build is never rewritten. The copy this found was a
  # week old, which is how collect-copyleft-sources.py came to refuse it.
  rm -f out/target/product/$6/module-info.json
  make -j6 out/target/product/$6/module-info.json
' pw3-aosp-build "$pw3_installclean" "$pw3_variant" "$pw3_keys" "$pw3_datetime" "$pw3_version" "$pw3_product"

# Written only after a build that cleared the staged tree, and only once that
# build succeeded: the stamp says "this tree was staged from these makefiles,
# with nothing left over from before them".
if [[ "$pw3_installclean" == 1 ]]; then
  printf '%s' "$pw3_config_now" > "$pw3_config_stamp"
fi
