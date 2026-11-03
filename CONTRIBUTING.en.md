# Contributing

This file is for people changing the code. To put the system on a Kindle, read
`docs/install.en.md`. Anything about security -- signing keys, the guarantee
that no partition is written, private data -- is in `SECURITY.en.md`.
Talking and contributing here means following `CODE_OF_CONDUCT.en.md`.

## The hard rules of this project

Said first, because they explain most of what follows:

1. **No partition is written and no bootloader is changed.** The whole system
   is two image files on the Kindle's user area (FAT), reached through a
   handover module inside the stock kernel. Holding the power button down gets
   you back to the stock system. Anything that needs a partition written is not
   accepted.
2. **Per-device data goes neither into the repository nor into a release.** The
   panel waveform, the Wi-Fi calibration and firmware, the MAC, the serial
   number, the ATAGs, the stock private kernel modules -- these are taken off
   the owner's own device at install time and nowhere else.
3. **A conclusion is backed by a measurement.** The documents here are full of
   "this was measured at N". A change that moves a number somebody wrote down
   either measures it again or says why it need not be measured again. Say
   which parts are inference and which are measurement.
4. **No stopgaps.** Make the premise of the problem a compile-time or test-time
   refusal rather than adding a special case to the code.

## Build environment

Today: macOS on Apple Silicon with a Lima VM -- ARM64 Ubuntu holding an x86
chroot, which is where AOSP 8.1's host tools run. None of it touches a device.

```bash
python3 tools/fetch-prebuilts.py      # third-party files (EinkBro, WebView, the monochrome emoji font), SHA-256 checked
bash tools/vm.sh start                # make and start the VM from config/build-vm.template.yaml
bash tools/vm.sh shell                # enter it; the commands below run inside
sudo bash /mnt/port/tools/provision-x86-runtime.sh  # qemu and the libraries AOSP's x86 host tools need
bash /mnt/port/tools/provision-aosp-chroot.sh
bash /mnt/port/tools/sync-aosp.sh     # fetch AOSP as source-lock.json pins it
bash /mnt/port/tools/build-test-libc.sh  # static ARM musl for the entry and the installer; put the archive source-lock.json's test_libc names in /mnt/build-share/toolchains/ first
bash /mnt/port/tools/build-kernel.sh android-core
PW3_AOSP_INSTALLCLEAN=1 bash /mnt/port/tools/run-aosp-build-job.sh  # required when a package list changed
python3 /mnt/port/tools/export-aosp-images.py --output /mnt/build-share/<export dir>
```

**A system build always goes through `tools/run-aosp-build-job.sh`**, never
straight to `build-aosp-system.sh`: the job shell binds the images it produced
into a receipt by content, and the export and the payload believe that receipt.
Going around it makes an export cite another build's receipt. It takes its
options from the environment only:

| Variable | Default | When it has to be given |
|---|---|---|
| `PW3_AOSP_INSTALLCLEAN` | `0` | Must be `1` after any `.mk` under `device/amazon/pw3` changed: a module that no longer installs would leave its file in the staging tree and the image would carry it. Without it, a build whose `.mk` files changed stops before entering the chroot |
| `PW3_BUILD_VARIANT` | `userdebug` | `user` for a release |
| `PW3_RELEASE_KEY_DIR` | none | Required for a `user` build: a path as the build chroot sees it, never inside the source tree or the shared build directory |
| `PW3_BUILD_VERSION` | none | Give it for a release. It becomes the Settings > About tablet > Build number line (`ro.build.display.id`) and goes only into `build.prop`, leaving `BUILD_ID`, the fingerprint and the boot image alone |
| `PW3_BUILD_DATETIME` | the current time | To rebuild a release's `build.prop` and fingerprint, pass the value its release notes record |

Not done yet: a build reproducible in one command, and building on a Linux
host.

## Tests

```bash
python3 tools/run-tests.py                       # what runs on the host
# In the VM, where a dozen more run, including the ones that need the AOSP tree and the build output
bash tools/vm.sh shell
cd /mnt/port && python3 tools/run-tests.py --cc gcc --cxx g++
```

The suite prints what it could not run here and what each of those is missing.
**Do not make something pass by loosening what it checks** -- this repository
has had a few "skipped because" lines that were not the real reason. The way
out is to make the missing thing exist.

A new check is verified in reverse: break the thing it checks on purpose and
confirm the check fails. A check that only ever passes proves nothing about
itself. And a test that is in the tree and in nobody's list of what to run is a
hole in a green suite -- `tools/tests/test_every_test_runs.py` refuses that.

## The patch series

Changes to AOSP and to the kernel are patch files, applied in numbered order
(`tools/apply-patch-series.py`).

That tool is **append-only**: it records the patches it applied and their
hashes in `.pw3-applied-patches.json` inside the source tree, and refuses a
change to one it has already recorded. To change one, put the tree back and
apply the series again:

```bash
git -C <source tree> checkout -- . && git -C <source tree> clean -fdq
```

Above the body of a patch, write prose saying **why** the change is made and
what it rests on. Those paragraphs are one of this project's main records.

## Two rules about framework resources

When overlaying a `frameworks/base` resource:

- **Change values, never add names.** A new resource name changes `R.java`,
  which changes the boot image.
- The comment above an overridden entry is copied **word for word from AOSP**
  (`aapt2` copies it into `R.java`).
- Compare the ART boot files under `/system/framework/arm` after every build. A
  change there means installing the new version recompiles the applications in
  `/data`, and the release notes have to say so.

## Commits

- Message: first line `scope: what changed`, lower case, no full stop; the body
  in whole sentences saying **why**, what it rests on, and how far it was
  verified. The commits already here are the examples.
- One commit does one thing; split changes that cross subjects.
- A pull request follows the template: what changed, why, and how far it was
  verified. Every pull request runs the host tests on GitHub
  (`.github/workflows/tests.yml`, on Linux, with `--zstd-archive`), and one
  that does not pass is not merged.
- Enable the hook once per clone:

```bash
git config core.hooksPath tools/git-hooks
```

It runs `tools/check-private-data.py` over what is staged. The same check is
also an entry in the suite (`run-tests.py --filter private-data`), because a
check that only runs in a hook depends on somebody having remembered to enable
it, and this repository is meant to be published. What it checks, and how to
add your own values, is in `SECURITY.en.md`.

## Documents

- Chinese is the first language, but every document written for a user or a
  contributor has an English half: the five a reader needs (`README`,
  `CHANGELOG`, `docs/install`, `docs/known-issues`, `docs/open-items`),
  `CONTRIBUTING`, `CODE_OF_CONDUCT`, `SECURITY`, `THIRD_PARTY`,
  `docs/README`, and every design record under `docs/design/`, with
  `tools/tests/test_doc_pairs.py` holding the two sides to the same
  structure (18 pairs today). Changing one side means changing the other.
  **A directory's own README (`boot/`, `device/`, `installer/`, `patches/`,
  `tools/`) is Chinese only**: those are signposts for somebody changing that
  part of the code, and are not in the pair list.
- References between documents are checked by
  `tools/tests/test_doc_links.py`: a `.md` path with a directory in it has to
  resolve.
- Which document answers what is in `docs/README.en.md`.
