# The release build: the research and the plan

This document says how a release is built and what has to be true before one. Before a release, walk the "release checklist" line by line; the other sections are the reasons and the evidence behind it. The research began on 2026-10-15.

## What has to change

| Item | The test build | A release | Why |
|---|---|---|---|
| Build type | `userdebug` | `user` | Drops `su`, `gdbserver` and the rest of the debug components, and `ro.debuggable=0` |
| Signing | AOSP's public `test-keys` | This project's own release keys | Everybody has the test keys, so anybody can sign an application with system privileges |
| adb | The debug build defaults to `mtp,adb` (the build adds adb itself) and requires authorisation | MTP only by default, with adb turned on in developer options | Security; file transfer is MTP (`usb-mtp.en.md`) |
| Diagnostic switches | None | None | The development test entry (diagnostic switches, holding the framework until released, test helper scripts) was removed as a whole before v0.1.0; `build_entry.py` builds only the release entry |

## Facts already established

- **Software rendering does not depend on a debug build** (verified by reading the patches, 2026-10-15):
  - Every application window uses the software Canvas, by `frameworks/base` 0006 and the property `ro.pw3.software_canvas`, with no debug-build condition; a `user` build gets it too.
  - There used to be a per-package switch effective only on debug builds (`debug.pw3.canvas_pkg`); 0006 covers what it did. Since 2026-10-22 that property is no longer written in `pw3.mk`: the code has a `Build.IS_DEBUGGABLE` condition so it never applied in a `user` build anyway, and leaving it meant an application's package name written into the system image's `build.prop` -- and the image has no business knowing what the reader will install. The patch that provided it was removed before v0.1.0, together with two patches that switched to software drawing separately and duplicated 0006. Removing them changes `framework.jar`, so all 45 ART boot files change and installed applications are recompiled once on the first boot after the upgrade (frameworks/base 0026, 0047 and 0051: from their usage profile where there is one, otherwise quickened only; about 1-3 minutes for WeRead alone); doing it before the public release means no public user pays for it.
- **Changing the signing key wipes the data of the system packages that share an identity** (`PackageManagerService.java`, lines 11120-11140):
  - When a system application's signature changes, 8.1 accepts the new one and keeps the data;
  - but if the application belongs to a shared UID (such as `android.uid.system`, which the framework itself is in), a new signature that does not match the one recorded for the shared UID throws "Signature mismatch for shared user" outright, that package fails to load, and its data is wiped.
  - Measured on the device on 2026-10-22: data made with `test-keys` upgraded straight to the release keys, and the 13 packages with a shared identity lost their data, the platform package `android` among them, so system_server exited once on its first start and came up normally on the second. The cost is about 20 seconds more on that one boot and system settings back to default; the applications and books the reader installed are untouched (`known-issues.en.md`, section 11).

## The plan

1. **A release uses the release keys from its first version**, so a reader in the wild never meets a key change.
2. **A device whose data was made with `test-keys` during development** upgrades directly, with no migration, and the installer does not tell the two kinds of data apart either (`data_format` is still 1: it does not move for a key change, and the move to 2 prepared on 2026-11-02 went with shrinking the data image, which the owner turned down on 2026-11-03). Two approaches were considered:
   - a new empty data image, with applications and sign-ins done again;
   - a very narrow migration: a shared UID may change signature only when the old one is AOSP's public test platform key and the new one is this project's release key, with the installer treating the two kinds of data as different `data_format`s.
   - Neither was taken: the measurement above shows that upgrading directly loses one set of system settings and about 20 seconds, and only on the one upgrade that changes the key; the first would make the reader reinstall applications and sign in again, and the second means changing `PackageManagerService` and verifying it on the device.
3. **The `user` build**: `lunch pw3-user` (`build-aosp-system.sh` picks it from `PW3_BUILD_VARIANT`); check whether this project's policy refers to any SELinux rule that only exists in debug builds (the `su` domain, say); `adbd`'s `--root_seclabel` has no effect in a `user` build and can stay.
4. **adb off by default**: a `user` build does not add adb to `persist.sys.usb.config=mtp`; "USB debugging" in developer options still works, and the Kindle asks for authorisation the first time a computer connects (`usb-mtp.en.md`).

## Source alongside the release

`tools/collect-copyleft-sources.py`, run in the build VM after the images are exported (`export-aosp-images.py`), makes the source files each Release has to carry.

**Which files need source is read from the image itself**: it parses `/etc/NOTICE.html.gz` out of the image and, for every licence text containing GPL, LGPL, MPL, EPL, CDDL or EUPL, traces each file:
- File to module: `module-info.json` (`make out/target/product/pw3/module-info.json`, which only writes the table and compiles nothing); a static library is looked up by module name.
- Module to source:
  - AOSP projects: `config/aosp-8.1.0_r81-resolved.xml` (the manifest at sync time);
  - the kernel: `vendor/amazon/pw3-hardware` and `/kernel`;
  - the third-party applications in this repository: `device/amazon/pw3/apps/sources.json`.
- Not one file that cannot be traced is let through (a prebuilt binary under `prebuilts/`, say): the script lists all of them and stops, writing nothing.

**The source is the source that was built**, and the script checks every item and stops on a mismatch:
- every AOSP project's HEAD equals the revision in the manifest;
- the patches this project applied to it match the patches recorded when the image was built (`build-result.json` in the export directory);
- the build tree has no change or extra file beyond those patches (`git status`);
- the kernel build the image's kernel modules came from used the kernel archive in `source-lock.json` and this repository's current kernel patches.

**The rehearsal of 2026-10-19**: run over that day's test image, it stopped twice, and both were understood:

- `/system/vendor/lib/modules/g_android.ko`: "no module installs it". **Not a missing source, out-of-date tracing metadata**: `module-info.json` was generated on 10-15 and the USB gadget module was renamed from `g_ffs.ko` to `g_android.ko` on 10-17. So the script does not rely on a "generate it first" in its documentation and checks for itself: a `module-info.json` older than the image stops it, with what to run. Metadata describing a different build takes away the basis for "this source is this image's source", which is the one thing the whole script exists to guarantee.
- `/system/bin/gdbserver`: an AOSP prebuilt (under `prebuilts/`) whose source is not in the AOSP tree. Its build tag is `debug`, a `user` build does not install it, and a release image does not have the file. **Stopping on a test image is correct**: a test image was never meant to be released.

With a fresh `module-info.json`, nothing but `gdbserver` failed to trace.

**The output** (the rehearsal of 2026-10-15):

| File | Size | Contents |
|---|---|---|
| `<name>-aosp-sources.tar.xz` | 28.5 MB | 14 AOSP projects: art, libcore, and under external: e2fsprogs, iptables, iproute2, selinux, libnl, libcap, libexif, fec, dnsmasq, f2fs-tools, gptfdisk, mmc-utils. Taken at the manifest's revisions and patched with this project's patches; the same input twice produces byte-identical output |
| `linux-3.0.35-amazon-5.16.2.1.1.tar.gz` | 119 MB | Amazon's published kernel source as it is; the patches, the configuration and the build scripts are in the repository at the tag |
| `einkbro-16.7.1-source.tar.gz` | 14.5 MB | EinkBro's upstream source (GPL-3.0-or-later) |
| `rime-ice-da1fbe6-source.tar.gz` | 16.8 MB | Where the pinyin dictionary came from (GPL-3.0-only) |
| `SOURCES.md`, `sources.json` | -- | Where the source of each of the image's 54 copyleft files is; the written offer |

- **Chromium (WebView's LGPL part) is handled by a written offer**: the source is 6.7 GB, past GitHub's 2 GiB per release file. The offer states at least three years from publication, at no more than the cost of copying, through the channel given by `--contact`, and gives the address and MD5 of the same source Google publishes.
- It took about 1 minute 45. Tests: `tools/tests/test_collect_sources.py` (12 items).

**2026-11-01: it was run for real for v0.1.0, and all six files are attached to the release.** One thing about it is not obvious, and it comes up every time:

The script only looks at `out/`, and **each variant keeps its own output tree** -- the one built last is `out/`, the other `out-<variant>`, swapped by renaming (see the comment at the top of `build-aosp-system.sh`). The last build of 2026-10-31 was `userdebug`, so v0.1.0's `user` tree was still whole in `out-user/`: `system.img` there is `08a4d46f…`, 469,244,836 bytes, the same byte for byte as `files["system.img"].sha256` in `export-v010-20261031/build-result.json`, and `module-info.json` is a minute newer than it. Both of the script's content-based refusals therefore pass.

**The way through is to shadow `out` with `out-user` for the run, renaming nothing and writing nothing into the tree** (a rename moves the `.pw3-variant` bookkeeping with it; a shadow is undone by unmounting):

```sh
SRC=/home/builder/android-src                      # in the build VM, not inside the chroot
sudo mount --bind "$SRC/out-user" "$SRC/out"       # always undo this afterwards
sudo runuser -u builder -- env PATH=/usr/sbin:/usr/bin:/bin HOME=/home/builder \
  python3 /mnt/port/tools/collect-copyleft-sources.py \
    --export /mnt/build-share/export-v010-20261031 \
    --entry /mnt/build-share/entry-v010-20261101 \
    --name Readom --tag v0.1.0 \
    --contact https://github.com/hbyxabc/readom/issues \
    --output /home/builder/sources-v0.1.0
sudo umount "$SRC/out"
```

It runs as `builder` because the tree is `builder`'s and `git` does not accept a tree owned by somebody else (`safe.directory`). `PATH` carries `/usr/sbin` because the script calls `debugfs` to read the `NOTICE` out of the image. The output is written into `builder`'s home first and copied into `/mnt/build-share` after. Two things were checked when it finished: `out/.pw3-variant` still says `userdebug`, and `system.img` in `out-user` is still `08a4d46f…` -- the tree was not touched.

The output is the table of 2026-10-15; the sizes are: the AOSP archive 28,506,024, the kernel archive 119,420,718, EinkBro 14,534,339, rime-ice 16,782,935, `SOURCES.md` 5,517, `sources.json` 13,742. Each archive's SHA-256 is under `assets` in `sources.json`. `SOURCES.md` is **byte for byte** the one from the rehearsal of 2026-10-29 (neither the file list nor the offer changed); `sources.json` and the AOSP archive differ, because they are bound to this build.

**The next `user` build overwrites `out-user`**, and after that v0.1.0's source bundle can only be made by reproducing that build byte for byte first (with the `PW3_BUILD_DATETIME` its release notes record). So the order is fixed: make the source bundle **after the release image is exported and before the next `user` build**.

**Run again on 2026-11-03, and the answer was "no replacement needed".** A new image means this step is run again; it does not necessarily mean a re-upload. That day's last candidate (with 0072) had its `user` tree in `out/` itself -- it was the last build, so nothing had to be shadowed: `system.img` is `e7f574be…`, byte for byte what the export receipt says, and `module-info.json` is a minute newer, so both content-based refusals pass. The six files that came out are **byte for byte identical** to the ones attached to the release on 11-01. Which makes sense: that day's patches are all in `frameworks/base` (Apache-2.0), so the set of copyleft files in the image did not move. So **the GPL obligation holds for the new image and the source attachments need no re-upload** -- but that sentence may only be said after running it and comparing byte for byte, never deduced from "what changed was Apache-licensed code".

## The release pipeline rehearsed (2026-10-19)

Before doing any release engineering, every step of a release was run dry on that day's development build purely to find defects, publishing nothing: `build_payload.py` -> `render_screens.py` -> `build_entry.py --installer`, then `test_unpack.py` over the payload it produced. It found three defects; this is how each is handled now:

1. **The empty data image was not empty** (the one that mattered most). See `installer/README.md`: a data template handed out earlier in development carried 4.2 MB of AOSP test programs, and making one the same way again would have added a `/data/app/PW3FormatTest.apk` as well. The template is now made by `build_payload.py` from an empty directory and checked by its superblock.
2. **A screen did not fit**, and the arithmetic was wrong. See "区域高度按每个画面真正要画的算" (the region's height follows what each screen really draws) in `installer/README.md`. The rendering and `test_screen.py` both run in `tools/run-tests.py`.
3. **The export receipt could describe a different build**. `export-aosp-images.py`'s `built_utc` and `build_log_sha256` come from `logs/aosp-system-job.json` and `logs/aosp-system-build.log`, and only the job shell `run-aosp-build-job.sh` writes those two files. Several builds during development called `build-aosp-system.sh` directly, so all of those exports' receipts attested the build of 10-15: the payload manifest the rehearsal produced says `built` is `2026-10-15T16:05:09Z`. That time goes into the installer's "the device clock may not be earlier than the build" test (earlier than that and TLS cannot be checked).
   - "The receipt may not be older than the image" was considered as the guard, and **that does not hold** (found by an adversarial review on 2026-10-19): switching build variants in this project means `mv`-ing the whole output tree, and `mv` keeps the timestamps; restoring from a copy does the same. "The receipt is newer than the image" therefore proves nothing about it being this batch's receipt, and this project's own build workflow goes around it.
   - What is done now: **the build job records the two images it produced into the receipt by content** (the `images` field of `logs/aosp-system-job.json`), the export checks each SHA256 and refuses a mismatch. The build log is written by the same run and is covered by the same binding.
   - **So a release build must be started through `tools/run-aosp-build-job.sh`** and not by calling `build-aosp-system.sh`.

Measured (2026-10-19, a 6-core build machine): the payload 46 seconds (a 196 MB system image and a 59 KB data template), the screens 10 seconds, the entry 17 seconds, `Readom.sh` 7.8 MB. `test_unpack.py` passes 15 items, among them a real payload unpacked under QEMU -- 25 seconds for the system image, 23 for the data image.

Uploading to and downloading from GitHub were not part of this rehearsal: whether a Release's address works is a fact about the repository and cannot be verified on the build machine, so it is done after the release (steps 13 and 14 of the release checklist).

### The fourth defect: the download address wrote the file name twice (found 2026-10-21)

`build_launcher.py` takes `--release-url` as a **directory** and appends the file name itself. During development it was once given the complete file address, so the entry held

```
.../releases/download/<ver>/readom-system-<ver>.img.zst/readom-system-<ver>.img.zst
```

which is bound to 404. Nobody noticed because the path tested on the device already has the package on the Kindle drive and never fetches that address -- and it failed for **exactly the one person who needs it**: whoever can only download. `build_launcher.py` now **settles the authority before it classifies anything** (lower case, no userinfo, no port, no percent-encoding, no empty label, no trailing dot, at least two labels, no label over 63 bytes and nothing over 253), and **refuses percent-encoding in the path outright** -- not decoding and then comparing, because one decode cannot say what a server will do (`%2e%2e` decodes to `..` and is caught, `%252e%252e` decodes to `%2e%2e` and is not), and a release path is a version and a folder name with no need of the mechanism. With that settled it **holds two kinds of address to different standards** (changed on 2026-10-30, once the owner settled the route):

- **An address on github.com has to be exactly** `https://github.com/<owner>/<repo>/releases/download/<tag>`. GitHub has a shape, and inside that shape are two traps that have each cost a release -- the asset's own address and the tag page -- so an address there is held to the shape.
- **Anywhere else there is no shape to hold it to**, so what is checked is what is true of any address: https, no query or fragment, no empty or `.`/`..` segment in the path, and **no path segment equal to the payload's name**. That last one is the V65 guard; it does not care about the host and still catches an asset address passed where a directory belongs. It asks segment by segment rather than counting substrings: counting `/<name>` inside the path also counts the segments that merely begin with the name -- a folder called `<name>-backup`, a GitHub tag called `<name>-rc`, each a legitimate segment that was being refused (a Codex review, 2026-10-30).
- **It cuts the other way too**: the GitHub release shape on another host (`githubb.com/...`) is a typo in the host, and is refused.

A mismatch fails the build (`tools/tests/test_release_url.py`: 9 accepted, 43 refused).

### The download route: an ordered list of addresses in the entry, plus the routing on the server

**Changed to several addresses on 2026-11-01, because one address was ruled out by measurement.**
Before (settled 2026-10-30) the entry carried only `https://readom.app/dl/<version>` and left the
routing to the site's Worker. That rested on reachability being the server's to decide -- and the
acceptance run of 2026-10-31 showed it is not: this project's own device got 515,554 of 191 MB in
26 minutes and its last two attempts had no HTTP answer at all, while the same bytes came back at
390 KB/s over another network. `readom.app` sits on a Cloudflare plan with no mainland presence,
so visitors there are served from PoPs outside it, and no server-side change reaches that.
**Reachability is not a property of the release but of the path between one reader and one
provider**, so the list has to be in the device's hands.

`RELEASE-PLAN.md` settled "several addresses tried in order, plus the manual copy" (G2) back on
2026-10-24. It was not built then, the reason being "it reworks the installer's failure taxonomy".
That reason was right, and the reworked taxonomy is **more** right than the one it replaced:

- A 404, 410, 416 or a wrong length used to conclude that the release file was wrong. It now means
  **this origin does not have this file**, and the next one is asked; only when every origin says
  the same is the release really missing (E303). This is not only for mirrors: on 2026-10-31 the
  Worker's GitHub fallback still held the previous release's payload, 497 bytes shorter, and the
  one-address taxonomy would have let one stale mirror condemn a good release.
- Nothing arriving used to count toward a stop after two such attempts. It now **advances the
  origin first**, so an origin that cannot be reached costs one attempt instead of the whole budget.
- **Under the speed floor counts against the origin too** (added 2026-11-01). The floor the
  download is given is `--speed-limit 1024 --speed-time 60`: under 1 KB/s for a whole minute and
  curl exits 100. "Any bytes are progress" counted such an attempt as progress and kept the same
  origin -- which is how the 26 minutes of 2026-10-31 went: four attempts, a few hundred KB each,
  every one of them "more than before", and the rate was never the question. At 1 KB/s the payload
  needs more than two days and the screen's own advice is to use a computer, so a minute under the
  floor is an answer about this route: the next origin is asked, and it **counts as idle as well**,
  so a single origin stops at E201 after two such attempts instead of six. The part is kept either
  way and tapping again continues it.
- `refused` -- the count of origins saying "this origin does not have this file" -- is reset by
  bytes arriving **whatever their rate**. What it counts is origins saying the file is not there,
  and bytes are an origin saying the opposite; folded into the rule above, one origin serving
  slowly while another answered 404 reached E303.
- Both budgets are per origin: `DOWNLOAD_ATTEMPTS * origins` and
  `DOWNLOAD_IDLE_ATTEMPTS * origins`. The installer's suite now passes 171 of 171, and each of the
  three new cases has its mutation: removing the speed-floor switch fails two, removing the
  `refused` reset fails one.

**The `.part` is not dropped when the origin changes, which is the opposite of what was assumed on
2026-10-24, and it is deliberate.** Every origin holds the same object (the same bytes uploaded to
each at release time), and the payload is admitted by `payload.sha256` once the whole of it has
arrived, so a part assembled across origins meets the same check as one fetched from a single
origin. An origin that serves different bytes is refused by that hash (E302, the payload deleted,
the next tap downloads again) -- the same path as any corrupt download. The cost, plainly: against
a poisoned mirror the reader downloads once for nothing. Dropping the part on every switch instead
would make an install over a slow network impossible, which is a larger cost and is paid every day.

The routing stays on the server as well; the two do not conflict. The Worker still serves from R2
and redirects to the GitHub Release when the object is not there, and **that whole chain is one
entry in the list**. The device's curl follows redirects anyway, so adding a mirror or splitting by
region on the server still needs no new entry -- while the list gives "one provider unreachable
end to end" a way out too.

Putting the routing on the server needs no C change at all, because the curl that downloads already follows redirects:

```
curl --fail --location --max-redirs 5 --proto =https --proto-redir =https
```

(`installer/install/kindle.c`). `--proto-redir =https` keeps the whole redirect chain off http. So the Worker can serve from R2 and redirect to the GitHub Release when the object is not there, and **a later change of route needs no new entry** -- the one already in a reader's hands follows along. Each attempt asks the original address again (`install.c` passes `rel->payload_url` every time and `kindle.c` builds a fresh curl), so nothing caches the redirect's destination.

The cost, said plainly: every added origin is one more piece of release engineering -- it must hold the same object -- and an unreachable origin costs one attempt (about 90 seconds at worst: a 30-second connect timeout plus the 60-second slow-transfer floor). That is why the ceiling is four and not a list that grows. When every route is down the fallback is still "download the payload with a computer and copy it into `documents/`", which has been implemented all along and whose file name the failure screen gives.

**On the fallback route, a slow network may need several attempts.** The whole chain is `readom.app 302 → github.com 302 → release-assets.githubusercontent.com 206`: two hops, inside `--max-redirs 5`, https throughout, and **resuming survives that cross-host 302** (measured by the website session, 2026-10-30). GitHub's signed asset address carries an expiry about **30 minutes** out (same measurement, read off the URL's `se=` parameter), and 190 MB over a Kindle's Wi-Fi at 100 KB/s takes more than thirty.

**The half that was not measured, said plainly**: whether a transfer already under way is cut at the moment of expiry was never measured -- that run fetched 100 bytes. S3's documentation says expiry is checked when the request is made and a download already running may continue past it, which shows "it is certainly cut" is not a general rule, though it does not establish what GitHub's current backend does (a Codex review, 2026-10-30, where this passage asserted it as settled).

Either outcome is caught here, so **no code is needed for signature expiry**: if it is cut, that is a failed attempt that made progress, `idle` in `install.c` only counts up when an attempt added no bytes at all or stayed under the speed floor, and resets on progress that was not a slow-transfer exit, and the next attempt walks the 302 again for a fresh signature and carries on from where it stopped. A signature expiry cuts a stream that was moving at a normal rate, so curl's exit is not 100 and this relay is untouched by the rule above. **The six attempts are therefore a relay, not six tries at the same thing** -- but "one attempt succeeds" is not the baseline to estimate retries from: if every attempt were cut at 1800 seconds, the slowest speed that still finishes 190 MB in six of them is about **17.7 KB/s**; below that the six run out, E201 is reported, **the part is kept**, and another tap is another six attempts carrying on from it. The R2 hit has no signature, no second hop and `cache-control: immutable`, and that is the route a device normally takes.

One division of work follows from it. Since a 416 now means only one thing on the device side -- the object on the server is shorter than the manifest -- and is **reported as E303 without retrying**, **uploading a truncated object shows up on the device as a release fault that is not real**. So reading each uploaded object's `content-length` back with HEAD and comparing it against the local byte count is the step that stops that, not a smoke test, and is not to be skipped to save time (the website session has written this down on its own side). The other half is a cross-artifact agreement: the Worker must answer 416 for a range starting exactly at the object's length, as the RFC says (`>=`, not loosened to `>`), and must not answer an out-of-range request with the whole object and a 200. What that would do was described here as the device appending the whole body to the part it already had and reporting another E303 that is not real, and **that is wrong** (a Codex review, 2026-10-30, against curl 7.86's `http.c`): with a part shorter than the manifest and the server answering 200 with the correct whole object, curl returns `CURLE_RANGE_ERROR` (33) -- a non-zero resume offset against an answer carrying no valid `Content-Range` -- and the installer answers 33 by deleting the part and starting over. So the cost is 190 MB downloaded again, not a fault that is not real. (This is that case, not a rule: when the answer's length happens to equal the offset already held, curl can report success instead.) The agreement still stands; only the reason for it needed to be right.

A wrong version number gets no separate check: the payload is identified by SHA256 (`install.h`), so an address pointing at another version is refused rather than installing an old system.

**The half that cannot be found at build time** has to be done after the release: once the Release's attachments are uploaded, fetch `installer.payload_url` from the entry receipt for real and check `installer.payload.sha256` and the byte count. That step is in the release checklist and is not optional -- whether the address is right is a fact about the repository, and the build machine cannot be asked.

## The `user` build rehearsed (2026-10-19)

Run as `pw3-user` with throwaway keys ("PW3 rehearsal only" in the certificate subject, kept on the build machine, and nothing signed with them is ever released), to find defects at the build level, with nothing to do with how the release keys are kept.

**The first thing it stuck on**: `insertkeys.py` wants `$PW3_RELEASE_KEY_DIR/testkey.x509.pem`, and the build stopped before compiling anything.

The cause is that `@RELEASE` in `system/sepolicy/private/keys.conf` names `testkey.x509.pem` outright -- AOSP signs with `build/target/product/security/testkey` itself. This project's `user` build uses its own release keys (`pw3.mk`, which errors out with no keys), where the name in that directory is `releasekey`, so the file was not found.

The fix leaves AOSP alone: `BOARD_PLAT_PRIVATE_SEPOLICY_DIR`'s (`device/amazon/pw3/sepolicy-private`) `keys.conf` is appended after AOSP's, and a later section of the same name overrides the earlier. Only the `USER` line is overridden, to `releasekey.x509.pem`; `eng` and `userdebug` still use AOSP's directory, which has `testkey` and not `releasekey`.

**The cost**: the signing happens at build time with the release key (`PRODUCT_DEFAULT_DEV_CERTIFICATE`), so **the key is on the build machine for the whole build**, not only for the few minutes of signing. AOSP's other route is to build with the testkey and re-sign with `sign_target_files_apks`, where the key is needed only for the re-signing. That route was not taken: it means moving to the target-files flow (`make target-files-package`, `img_from_target_files`) and changing the export path with it, and this project depends on dexpreopt and on a byte-identical boot image, and what re-signing does to the odex has not been verified.

**After the fix the build passes** (2026-10-19 17:10, not one FAILED). Checking the image:

| Item | Result |
|---|---|
| `ro.build.type` / `ro.debuggable` / `ro.secure` / `ro.adb.secure` | `user` / `0` / `1` / `1` |
| `persist.sys.usb.config` | `mtp` in `default.prop` (a `user` build does not append `,adb`). Read on the machine after boot it is empty -- `UsbDeviceManager` strips `mtp` -- and `getDefaultFunctions()` then answers `mtp`, so the gadget is MTP with no adb, as `usb-mtp.en.md` says |
| `su`, `gdbserver`, `strace`, `logpersist.start` | None of them in the image |
| This project's system programs | All of those listed at the time present. What to check against is `PRODUCT_PACKAGES` in `initialization.mk` and `pw3.mk`, which now lists `pw3-init-check`, `pw3-boot-report`, `pw3-fg-state`, `pw3-system-verify` and `pw3-testfile-init` (installed in the ramdisk), plus the `pw3-usb-check` the USB component brings |
| File count | 1616 (userdebug is 1669; the difference is all debug tools and the `libchrome` and `libevent` only they use) |
| The 45 ART boot image files (`boot*.art`, `.oat`, `.vdex`) | **Byte-identical** to userdebug's -- so upgrading from a test build to a release does not trigger a recompile |
| system.img | 463 MB (userdebug 472 MB) |
| Signing | `SystemUI.apk`'s `META-INF/CERT.RSA` fingerprint `13:E2:88:...`, subject "PW3 rehearsal only"; userdebug's is `C8:A2:E9:...`, subject `O=Android` (AOSP's public platform key). **The key change really took effect** -- which is the whole reason a `user` build exists: AOSP's platform key is public, and an application signed with it gets system privileges |

Two things to remember:

- **`ro.build.tags=dev-keys`**. AOSP writes `test-keys` only when the default certificate is exactly `build/target/product/security/testkey`, and `dev-keys` otherwise; `release-keys` comes from the `sign_target_files_apks` route. This project does not take that route, so the value is the truth and is left alone.
- **A release build must carry `PW3_AOSP_INSTALLCLEAN=1`**. This one did not, and the image kept `etc/init/wifi-events.rc` -- a file removed from this project on 10-17 (this kernel has no ftrace, so init failed on all 25 of its commands at every boot), dated 10-16, left behind by the last thing installed into that output tree. **The module is no longer installed, a file already staged does not disappear by itself, and the image carries it.** The build job receipt (`logs/aosp-system-job.json`) records the variant and whether it was installclean, and the export receipt copies that, so "which kind of build is this image" has an answer.
  - Rebuilt with it: `wifi-events.rc` is gone, the file count is 1615 (exactly one fewer), **not one file in the staged tree predates this build**, and the 45 boot files are still byte-identical. It costs **5 minutes** (the objects are kept, only the install is done again), which for a release is nothing.

## The release checklist (in order, each step with something to check)

Written as one list, it is for two things: **a release can be repeated**, and **somebody else can make one** -- the second being one of the preconditions for open source. After each step is its evidence, not "it was done".

| # | What | Evidence |
|---|---|---|
| 1 | No device-side problems left | `docs/open-items.en.md` has nothing left; the previous version was installed on the device and `tools/verify-device.sh` was run and exited 0 -- exit 2 means some checks could not be answered, which is not an acceptance pass; the ones marked `~~` are the ones this image cannot answer by design (the readings that need root, on a release build) and they do not block |
| 2 | The keys are in place | All four, outside the repository (the build finds them through `PW3_RELEASE_KEY_DIR`), **and an offline backup exists** (only a person can confirm this one) |
| 3 | A clean working tree and a green suite | `git status` shows nothing; `tools/run-tests.py --cc gcc --zstd-archive <zstd archive>` passes on the build machine: 0 failed, 0 not run (steps 3a and 3b exist so that the items needing a QEMU kernel and a userdebug export can run too) |
| 3a | Both QEMU test kernels are from **this** patch series | `tools/build-kernel-smoke.sh` then `tools/run-kernel-smoke.sh` (the kernel interface tests: network, process-vm, binder, filecaps, aslr...), then `tools/build-loader-stage-kernel.sh`. The last has its own gate: the smoke result has to describe the same patch series or it stops. `handoff-in-qemu` and `handoff-client` read that loader-stage kernel, so those two being able to run is the evidence this step was done |
| 3b | The three QEMU smoke tests (ART / zygote / system services) | They cannot run on a release image: three of AOSP's `neverallow` rules close off "init copies a payload into `/data` and runs it in the shell domain" (the details are in `tools/aosp-runtime-smoke/README.md` and in the two files of the same name for zygote and service). So build a userdebug once more from **the same source tree**, export it, make its file listing, and run the suite against that, and these three run (`PW3_EXPORT_DIR` pointing at that export, or naming it `aosp-images-device-test-*` so the suite finds it). Evidence: measured on 2026-10-22, the variant switch took 28 seconds (the tree was hot), all three passed, and **the 45 ART boot files of the two variants are byte-identical** -- so the "an upgrade triggers no recompile" judgement made on the user variant holds for the userdebug one too. And `data-image` wants the seed image from this build's own payload, so it runs only after step 6 |
| 4 | Build | **Through `tools/run-aosp-build-job.sh`** (the receipt binds the images by content; calling `build-aosp-system.sh` directly makes the export cite another build's receipt): `PW3_BUILD_VARIANT=user`, `PW3_AOSP_INSTALLCLEAN=1`, `PW3_RELEASE_KEY_DIR=/release-keys`, `PW3_BUILD_VERSION="Readom v0.1.0"` (goes into `ro.build.display.id`, the Settings > About tablet > Build number line; without it that line is AOSP's `OC dev-keys` and names no release) |
| 5 | Export | `export-aosp-images.py`: `variant=user` and `installclean=true` in the receipt, and both images' SHA256 matching the build receipt. **Then run `sudo tools/inspect-system-image.py --image <export>/system.raw.img --output <export>/filesystem-inspection.json` as well**: the export does not write that file and step 11 reads it |
| 6 | Payload | `build_payload.py`: manifest schema 4, and `built` is this build's time |
| 7 | Screens | `render_screens.py --profile boot/profiles/<model>.json`: the model name matches the entry's |
| 8 | Entry | `build_entry.py --installer` (**without `--test`**; the two are mutually exclusive); **built twice, with `Readom.sh` byte-identical** |
| 9 | No recompile triggered | Every `boot*.art`/`.oat`/`.vdex` under `/system/framework/arm/` (45 of them today, a figure for orientation only -- count the set, do not go by memory) compared byte for byte with the previous version: `boot_image.sha256` in the export receipt `build-result.json` is computed from every file's name and content, and has to equal the previous version's; if they changed, the release notes have to say "the first boot recompiles" |
| 10 | No private data | `tools/check-private-data.py`: no denylist hit, and nothing outside machine-generated bytes (the summary reports those two numbers separately, see below); `Readom.sh` contains no waveform, calibration, ATAGs, serial number or adb public key |
| 10b | **The file to be uploaded, decompressed, equals what the entry has baked in** | No upload and no device, and a few seconds: `zstd -dc <release directory>/readom-system-<version>.img.zst \| sha256sum` has to equal the `.system` byte array in the entry's `launcher/release.h`, and the byte count it decompresses to has to equal the length there (512 MiB from 2026-11-03); then compare the uploaded file's own SHA-256 against `.payload`. **What it stops is "the entry was built against a different payload"** -- this project has already made that mistake once (the build missing `--release-url` happened to be the one called `ship`) -- and on a device that class of error only shows after a full 190 MB has come down. `release.h` holds decimal byte arrays, so convert to hex to compare. **Four candidates were built on 2026-11-03** -- patches 0069, 0070 and 0071 each needed a run on the device to find the next thing -- and this step was done on every one. **The one online now** (build h of 2026-11-04: f plus the installer fix; f is the 11-03 build with 0073): `.payload` = 190824820 / `c82dbe9f…`, `.system` = 536870912 / `e2bf999c…`, `Readom.sh` 7,759,418 bytes / **`7319e813…`**; the f it replaced was 190824793 / `3b292432…`, `9f268e2c…`, `a5827a94…` (all recorded under `superseded` in `readom-site/site-deploy/releases/v0.1.0.json`, which is the authority). f was, built twice identical, two origins with GitHub first, `build_incremental` = `eng.builde.20261103.110643`, and the boot image still `1cdfe692…` (0073 touches only `services.jar`, not the ART boot image, so the upgrade does not invalidate the compiled code in /data). The copyleft source bundle was regenerated on this tree too and its six files are **still byte for byte** what the release carries, so the source attachments need no re-upload. The version before it (with 0072, which was published for a while): `.payload` = 190823293 / `6376f975…`, `.system` = 536870912 / `d3d15306…`, entry 7,759,418 bytes / `249dd8a7…`. **This time it was checked in two steps**: first the rebuilt `release.h` was compared line by line with the one before it, and **only `payload_urls` and `payload_origins` differ** (1 -> 2), with `.payload`, `.system`, `.template_file` and `.built` identical byte for byte -- so step 10b's conclusion carries over unchanged; then `zstd -dc | sha256sum` and `zstd -dc | wc -c` were run as two separate passes (**not as one pass through `tee >(sha256sum)`**: the process substitution's output lands in the same pipe, so `wc -c` counts those 88 bytes too and 536,870,912 becomes 536,871,000), giving 536,870,912 / `d3d15306…`, identical to the `.system` byte array. The entry before the rebuild is `ac8a4db0…`; its origin list has one entry and it is not being released. The one before it (with 0071): `.payload` = 190823888 / `673b7a7c…`, `.system` = 536870912 / `05f7560c…`, entry 7,759,418 bytes / `24c9f6d6…`. **There is a trap here**: all four entries are **7,759,418 bytes**, and so is the one online -- not one byte apart, and a different content every time. So which one was uploaded can only be checked by content (R2's ETag, which is the MD5; the GitHub copy fetched back and rehashed); the length proves nothing. What follows is the first of that day's: `.payload` = 190830823 / `6ca56d38…` and `.system` = 536870912 / `b3213388…` (`zstd -dc | sha256sum` equals the byte array baked into the entry), with `Readom.sh` 7,759,418 bytes / `acfc1a3a…`, built twice and identical. The boot image is still `1cdfe692…`, so the upgrade does not invalidate the compiled code in /data. **2026-11-03 14:20, both origins were replaced with this version and checked by content**: the three GitHub assets were fetched back and rehashed -- entry `249dd8a7…`, payload `6376f975…`, `SHA256SUMS` with the right content, and `shasum -a 256 -c` says OK for both lines against the two downloaded files; the release body read back is identical to what was uploaded, with both hashes replaced. The three R2 objects match locally by **ETag, which for a single-PUT R2 object is its MD5** (`22ba1a46…` / `279c9ec7…` / `6b3239aa…`) -- the 190 MB payload is **not** fetched back from `readom.app` and rehashed, because that route measured 515 KB in 26 minutes from this network. What follows is the record of the version before it: **For the version now published** (payload built 2026-10-31, entry 2026-11-01): `.payload` = 190816894 / `ff432b02…`, `.system` = 603979776 / `d3e68e62…` — the latter is, byte for byte, the value the device's own `state` recorded, and it is what admitted the clean install of 11-01. (The 2026-10-30 version checked out at `.payload` = 190816397 / `32813650…` and `.system` = 603979776 / `367fa7ef…`, the website session doing it first and this side recomputing independently and agreeing; those two are no longer online and are kept here to show what the step looks like.) One thing that reads as a contradiction and is not: the export receipt's `system.img` hash (`51b48b9e…`) and `.system` (`367fa7ef…`) differ because the first is AOSP's build output and the second is that image padded to 576 MiB, which is what the device actually writes -- the manifest records the second |
| 11 | The source package | `tools/collect-copyleft-sources.py --export <export> --entry <entry directory> --name <name> --tag <repository tag> --contact <where to ask> --output <directory>`: the GPL parts' source matches the binaries in the image. It reads this build's `module-info.json` (which module installed which file), and that file's make rule has no dependencies and counts as up to date merely by existing, so `build-aosp-system.sh` deletes it before making it; during development the script once picked up a week-old one and refused outright. The kernel is bound by content in three things -- the entry receipt's `kernel_sha256` (the zImage inside `Readom.sh`), its `epdc_module_sha256`, and every `.ko` in the image (read from step 5's `filesystem-inspection.json`) -- all of which have to match this kernel build's output, which was itself checked against the kernel build receipt first. It does not compare a hash of the kernel receipt file itself: the receipt holds a compile time, so recompiling the kernel would judge it a different build even with a byte-identical zImage and modules |
| 11b | **The bytes being uploaded are the ones checked above** | Steps 8 to 11 all check the **build directory**, and the bytes `release.py --upload` sends are exactly the two paths named on its command line by `--entry` / `--system` (`--out` only decides where `SHA256SUMS` is written). So what this step has to stop is not "staging was linked wrong" but **the whole build-directory pair being wrong**. There are far too many candidates on the disk for that to stay hypothetical: on 2026-10-30 there were 10 copies of `readom-system-v0.1.0.img.zst`, their sizes crowded between 190,808,562 and 190,817,497, and 12 copies of `Readom.sh` at 7,697,433 bytes across 6 builds (the numbers go stale, the way to recount does not: `for f in */launcher/Readom.sh; do printf "%s %s %s\n" "$(stat -f %z "$f")" "$(shasum -a 256 "$f" \| cut -c1-12)" "$f"; done \| sort`). And the most misleading name of them all is `ship`. `release.py` now has three checks built in, which run even without `--upload` (reporting without blocking -- otherwise it would be a check only an irreversible act can exercise); any one of them failing is `exit 1` and not a byte goes out:<br>(1) **The entry checks itself**: decode the embedded helper, compare against the script's own `[ "$hash" = … ]` line, and confirm it is an ELF. The equivalent by hand is `sed '1,/^__PW3_FIRST_KERNEL_HELPER_BASE64__$/d' Readom.sh \| base64 -d \| shasum -a 256`, which still works away from the build machine -- so the GitHub Release copy deserves the same check, as it does not go through R2.<br>(2) **Does the payload belong to this entry**: against the payload receipt's `installer.payload.sha256` (falling back to `.payload` in `release.h`). **Identify it from the receipt, not from its name.**<br>(3) **Is the entry itself the right one**: `--expect-entry <the published SHA-256>`, required with `--upload`. **This one cannot be dropped, and can only come from outside.** Measured: `entry-v010-ship` with its own `payload-v010-ship` passes the first two **outright** -- a self-consistent old pair, which no internal comparison can tell apart, and only the external anchor stops. Putting the hash on the command line is safe because the script **checks** it rather than trusting it: a stale value can only refuse the upload. That is the opposite of a number written into a document -- that kind goes stale unnoticed, this kind blocks on the spot.<br>Four real violations were each tried and stopped: substituting `payload-rc-v0.1.0-final`, giving a stale expected value, the self-consistent old pair, and an entry truncated to 7,000,000 bytes; with `--upload` the exit code was 1 and the bucket stayed empty, and omitting `--expect-entry` is also exit 1.<br>The two copies handed over by hand (the one copied into the device's `documents/`, and the GitHub Release one) do not pass through `release.py`, so they need `cmp <my copy> <entry directory>/launcher/Readom.sh` separately. Checked on 2026-10-30: the staged file shares an inode with `entry-v010-rc/launcher/Readom.sh`, the helper is 5,695,724 bytes and `0a305222…` matching what the script pins, and the entry is 7,697,433 bytes and `46577424…`. One more source-to-artifact binding checked in passing, which did not exist before: `boot/launcher/launcher-template.sh` plus the release arguments reproduces the shipped script's head **byte for byte** (3172 bytes, up to the base64 separator).<br>**The entry's hash is already in a receipt** (corrected 2026-11-04: this used to say "left for the next version: the entry receipt lacks `launcher_sha256`", which is wrong). In the top-level `build-result.json` (schema 3) `launcher` is indeed only a path, but the entry directory it points into has `launcher/build-result.json` (written by `boot/build_launcher.py`), which has always carried `launcher_sha256` and `helper_sha256`; build h's says `7319e813…` for `launcher_sha256`, the same as `shasum -a 256 launcher/Readom.sh`. It still cannot stand in for check (3)'s external anchor: the receipt and the bytes are written by the same run into the same directory, so it answers "did this build produce these bytes" and not "are these the bytes to ship" |
| 12 | Upload to **both places** | **R2** (the one the device uses): `<version>/Readom.sh`, `<version>/readom-system-<version>.img.zst`, `<version>/SHA256SUMS`, in one the site's own upload script (**`--expect-entry` is required with `--upload`**, see step 11b; without it the exit code is 1). **The GitHub Release** (what people use, and where the Worker falls back to): `Readom.sh`, `readom-system-<version>.img.zst` and the source package; the release notes carry both files' SHA256, the certificate fingerprints and "which two files to copy". **It is not released until both are done** -- with only one, either the site's button works and the device cannot install, or the other way round |
| 13 | **After the release, without fail**: both branches really work | Download `installer.payload_url` from the entry receipt (`https://readom.app/dl/<version>/…`) for real and check `installer.payload.sha256` and the byte count. **Walk both branches**: the hit, with the object in the bucket, and the fallback, with that object renamed for a moment (302 to GitHub). At least one of them with `--continue-at` from a non-zero offset, to see a 206 and the right `Content-Range` -- resuming is the path taken after a dropped connection, and nothing exercises it otherwise. **Asking GitHub for that copy directly replaces neither**: it shows the object can be fetched, not that the 302, the `Location` and the resumed Range the device gets from the original address hold together |
| 14 | **After the release, without fail**: walk a clean install | The preconditions have to be **observable**, and "only `Readom.sh` in `documents/`" is not enough to make it download: an install already in place with matching state does not download, nor does a complete payload in `android/download/<name>`, and a complete `.part` skips curl entirely. So before starting, confirm: no `android/` folder (or no `system=ready` in `state`), no complete payload and no `.part` under `android/download/`, and only the entry in `documents/`. The evidence has to be observable too: **the log shows a download that really happened, a `data.img` that was really created, and a first Android initialisation that really ran through** -- "it reached the home screen" does not show that those branches were taken |

> **Step 12's tooling is not in this repository.** The site -- the pages, the upload script,
> the release record `releases/<tag>.json`, the Worker -- was moved out of this tree on
> 2026-10-31 and is its own project: the
> upload and its read-back are run by the repository's owner from the site's tooling. The
> published repository does not have them and does not need them: what it gives is how these
> two files are built, and where they are then put is the publisher's business.

## Steps 4 to 8, as commands

Every one of them runs in the build VM. Apart from the lines that say `sudo` (starting the build job under systemd, taking the filesystem inventory), they run as `builder`, **not under `sudo`**: the first thing `export-aosp-images.py`, `build_payload.py`, `render_screens.py` and `build_entry.py` do is refuse root. `<ver>` is the release tag, for example `v0.1.0`; `PW3_BUILD_VERSION` is written `Readom <ver>`.

```sh
# 4 Build (environment in the table above; PW3_BUILD_VERSION needs aosp-build-make/0004)
# If the previous job of this name failed, clear its state first; with nothing to clear this says "not loaded", which is harmless
sudo systemctl reset-failed pw3-aosp-system.service
sudo systemd-run --unit=pw3-aosp-system --service-type=exec \
  --uid=builder --gid=builder --setenv=HOME=/home/builder \
  --setenv=PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  --setenv=PW3_AOSP_INSTALLCLEAN=1 --setenv=PW3_BUILD_VARIANT=user \
  --setenv=PW3_RELEASE_KEY_DIR=/release-keys \
  "--setenv=PW3_BUILD_VERSION=Readom <ver>" \
  /bin/bash /mnt/port/tools/run-aosp-build-job.sh
# The build log is /mnt/build-share/logs/aosp-system-build.log;
# go on only once state in /mnt/build-share/logs/aosp-system-job.json is completed

# 5 Export, then take the filesystem inventory the export does not write and step 11 reads
python3 /mnt/port/tools/export-aosp-images.py --output /mnt/build-share/export-rc-<ver>
sudo python3 /mnt/port/tools/inspect-system-image.py \
  --image /mnt/build-share/export-rc-<ver>/system.raw.img \
  --output /mnt/build-share/export-rc-<ver>/filesystem-inspection.json

# 6 Payload
python3 /mnt/port/installer/build_payload.py --export /mnt/build-share/export-rc-<ver> \
  --data-format 1 --version <ver> --output /mnt/build-share/payload-rc-<ver>

# 7 Screens (the fonts are in the AOSP tree)
S=/home/builder/android-src
python3 /mnt/port/installer/screens/render_screens.py \
  --manifest /mnt/build-share/payload-rc-<ver>/manifest.json \
  --profile /mnt/port/boot/profiles/pw3-5.16.2.1.1.json \
  --cjk-font $S/external/noto-fonts/cjk/NotoSansCJK-Regular.ttc \
  --latin-font $S/external/roboto-fonts/Roboto-Regular.ttf \
  --latin-title-font $S/external/roboto-fonts/Roboto-Medium.ttf \
  --output /mnt/build-share/screens-rc-<ver>

# 8 Entry, built twice and compared
python3 /mnt/port/boot/build_entry.py \
  --export /mnt/build-share/export-rc-<ver> --kernel /mnt/build-share/android-core-kernel \
  --system-path android/system.img --data android/data.img:1879048192 \
  --name Readom.sh --title Readom \
  --installer /mnt/build-share/payload-rc-<ver> --screens /mnt/build-share/screens-rc-<ver> \
  --release-url https://github.com/hbyxabc/readom/releases/download/<ver> \
  --release-url https://readom.app/dl/<ver> \
  --ca-bundle /mnt/build-share/third-party/ca-bundle-2026-09-25/cacert-2026-09-25.pem \
  --zstd-archive /mnt/build-share/third-party/zstd-1.5.6/zstd-1.5.6.tar.gz \
  --output /mnt/build-share/entry-rc-<ver>
# Run the same command again with --output /mnt/build-share/entry-rc-<ver>-second, then:
cmp /mnt/build-share/entry-rc-<ver>/launcher/Readom.sh \
    /mnt/build-share/entry-rc-<ver>-second/launcher/Readom.sh
```

> **`--release-url` is given twice, and the order is the order the device tries.**
> Before 2026-11-03 this line carried only `readom.app`, while the release
> description (`releases/v0.1.0.json`, in the website's own tooling) records
> **GitHub first and `readom.app` second**. The four entries built that day from
> this command each held a list of one address -- the "ask the next origin when
> one will not answer" that was finished on 11-01, silently gone -- and **nothing
> downstream notices**: every hash is right, the payload is the right payload,
> and the reader's Kindle simply asks a host that will not answer. What stops it
> is the `origins` check in the website tooling's `release.py`, which compares
> the entry receipt's `installer.payload_urls` against the release description
> item by item. So **step 11b is not a formality**: this is the run where it said
> `BAD origins: the entry was built for https://readom.app/...`.

Three ways to get it wrong:

- **`--installer` is the payload directory, not the repository's `installer/`.** It reads that
  directory's `manifest.json`, which `installer/` does not have, and the error is a bare
  `FileNotFoundError`.
- **`--ca-bundle` wants the file**, and the file is called `cacert-<date>.pem`, not
  `ca-bundle.crt`.
- **Do not hand `check-private-data.py` the compressed payload.** Random bytes in a 190 MB zstd
  blob match the e-mail pattern; four of them did in one run. Step 10 is about the repository
  and the entry, both run from the repository directory: `python3 tools/check-private-data.py`
  (no arguments, the repository) and `find <entry directory> -type f -print0 | xargs -0 python3 tools/check-private-data.py`.

  **That entry run reports an "e-mail address" every so often; that is expected, and not a reason
  to wave it through.** The entry carries about 7.7 MB of compressed bytes (`Readom.sh` has the data
  image and the screens inside it, `public-initramfs.cpio.gz` is gzip), and the pattern rules run
  over raw bytes on purpose -- a key or a MAC hidden in a blob is exactly what they are for -- so
  the price is that every re-compression can roll a false positive. The test therefore has two
  layers:

  **Since 2026-11-03 the tool draws that line itself rather than leaving it to the reader.** Each
  finding carries `[in machine-generated bytes]` where it applies, and the summary line says how many
  of them did. The test is on the bytes -- NUL bytes or invalid UTF-8 (`is_text()`) -- and a launcher
  splits at the helper marker: the script above it is text, the program below it is not. A denylist
  hit **never** carries the mark.
  Before that the tool did the opposite: it skipped the raw bytes when the tail was all base64. That
  hid things -- a serial number is sixteen alphanumeric characters, every one of them in the base64
  alphabet, so a serial behind the marker disappeared and took the exact denylist comparison with it
  (measured the same day). And it bought almost nothing: **an e-mail shape needs `@` and `.`, and
  neither character is in the base64 alphabet**, so the encoded region could not produce an e-mail
  hit in the first place. With the suppression gone, `entry-v010-20261103e2` still comes back 32
  files, 0 findings.

  - **A denylist hit must be zero**, with no exceptions. That is an exact SHA-256 comparison against
    this machine's real values: it cannot be a false positive, and the allow list does not suppress
    it (it runs over the whole file either way).
  - **A pattern hit is triaged by decompressing**, and once triaged, **do not add an entry to
    `private-data-allow.txt`**. Entries there are a path and a text, and the entry is rebuilt every
    release with different bytes rolling out, so such an entry stops matching next version and only

  Triage looks at what is inside the archive, not at the few bytes that rolled:

  ```sh
  gzip -dc <entry directory>/public-initramfs.cpio.gz > /tmp/x.cpio
  mkdir -p /tmp/initrd && cd /tmp/initrd && cpio -idm --quiet < /tmp/x.cpio
  find . -type f -print0 | xargs -0 python3 <repository>/tools/check-private-data.py
  ```

  **What the 2026-10-30 triage found, written down so it is not redone.** The entry run reported
  **three** findings -- one each in `Readom.sh`, `launcher/helper` and
  `public-initramfs.cpio.gz` -- all with the same value (eight bytes of nonsense; they are not
  copied out here, because the next version rolls a different string and copying it would only
  make this document trip the check itself).

  **The three are one occurrence of those bytes, reported through three layers**, and the offsets
  prove it: the bytes sit at offset 275534 inside `public-initramfs.cpio.gz`, that whole gzip is
  embedded in `launcher/helper` at offset 3322336, and the two add up to exactly 3597870, which is
  where they are in `helper`. The `Readom.sh` finding is the tool's own doing: `check_file()` runs
  `decoded_helper()` over every file, which recovers the `helper` embedded in the entry script and
  scans that. (`helper` likewise embeds the whole of `zImage` and `screens.bin`, so any compressed
  byte anywhere in the entry is reported once in each of the three files.)

  **The bytes are a product of the gzip, not of its content**: unpack that cpio, scan every file in
  it, and the string is nowhere. What the real content does hold is three of `appro@openssl.org`,
  the upstream OpenSSL author's address compiled into AOSP's `init` binary -- a third party's public
  attribution, not the reader's data. The same gzip came back with 0 findings in the five builds
  before this one; only the compressed bytes differed. Both layers pass: zero denylist hits.

  **The entry rebuilt on 2026-10-31 hit it again, in exactly the same shape, so this is the normal
  case and not an accident.** This time there were **two** (`launcher/helper` at offset 3606832 and
  `public-initramfs.cpio.gz` at 275540, the difference of 3331292 being where that gzip sits inside
  the helper), the value again eight bytes of noise, with only three in ten of the surrounding 400
  bytes printable. **One test is enough to triage it**: dump a few dozen bytes either side of the
  hit -- a real address has readable text around it and compression noise does not. **Do not add it
  to `tools/private-data-allow.txt`**: those bytes change with every build, so an entry there would
  only grow a rule that never matches again. What establishes that the entry carries no device data
  is `contains_device_data: false` in the receipt, plus the separate checks for waveform,
  calibration, ATAGs, serial numbers and adb public keys -- not this regular expression.

## The release keys (regenerated under the Readom name, 2026-10-24)

All four (`releasekey`/`platform`/`shared`/`media`, RSA 2048, SHA-256 certificates, 10000 days) were generated on the build machine with `tools/make-release-keys.sh`, subject `O=Readom`. The private keys live **outside the repository**, are handed to the build read-only, and `PW3_RELEASE_KEY_DIR` names where at build time.

**Why they were replaced once** (2026-10-24): the project took the name Readom and the old one was written into the certificate subject. Replacing a key costs one upgrade in which the system packages that share an identity lose their data (settings back to default, about 20 seconds more, third-party applications untouched), so it is only cheap while nobody else has installed anything -- at that point the only machine in the world running this was the developer's, and not yet signed in to anything.

**Why not in the repository** (moved out 2026-10-23): inside the working tree, the only thing keeping it out of a commit is `.gitignore`, and a filter is weaker than a location -- `git add -f`, archiving the working tree, or copying the directory wholesale all carry it along. `build-aosp-system.sh` refuses outright a key directory under `/port` or `/build-share`, and refuses AOSP's public `testkey` as well (its private key is public, so anybody can replace an image signed with it and the build itself would not notice).

**Keeping them**: encrypted at rest, with the passphrase kept apart from the keys; plus at least one offline copy, because a copy held only in the cloud can be recovered by anybody who gets into any one signed-in device.

**What rotating costs**: change a key and a system application with a `sharedUserId` on the upgrade path is judged a signature mismatch and its data is wiped. So the answer to a leak is "publish a new fingerprint and offer a clean install", not a silent rotation. The certificate fingerprints are **public** and go into the README at release time, so that anybody can check that what they installed is signed by this project:

| Key | Certificate SHA-256 |
|---|---|
| releasekey | `0F:61:83:01:7D:E5:17:B7:E7:4E:BD:8D:90:03:F2:A7:64:4E:80:9E:16:85:DD:55:2D:8D:6D:AC:4E:CF:8C:C0` |
| platform | `7A:31:A4:EA:1B:24:31:80:C0:1F:5B:AB:7E:70:D9:BA:DF:2E:46:13:B5:3C:B4:6F:58:57:51:3B:C8:08:B4:8E` |
| shared | `FA:6A:B5:7A:BB:CF:8A:D3:6A:28:DF:A6:63:B3:B0:D4:73:75:91:D7:47:6A:91:A6:B5:67:1A:77:2D:8F:80:75` |
| media | `FB:CE:D1:B0:8E:06:90:59:A2:41:48:5C:F2:E9:58:C9:DD:D8:57:29:3A:F2:4C:D7:99:48:93:48:20:0C:D5:C6` |

**Offline backup**: lose the keys and no new version can be made for a version already published (readers could only start over with new data), so a copy has to go onto offline media. A build cannot do that for anybody, and step 2 of the release checklist asks a person to confirm it.

## The security review

A review of this system before a release (from 2026-10-17): the kernel, the SELinux policy, the system image, and the programs this project added.
Each item below says **what was examined, what was concluded, and what was not done**.

### 1. Where the kernel comes from

- The baseline: the `linux-3.0.35` in the `5.16.2.1.1` source package Amazon published for the Kindle Paperwhite 3.
- **5.16.2.1.1 is the last firmware for this model**, so Amazon has no newer source to compare against and "diff the vendor's newer version to see what they fixed" is not available.
- Two things spot-checked:
  - Dirty COW (`CVE-2016-5195`): Amazon patched it; `mm/memory.c` has the `FOLL_COW` test.
  - RFC 5961's challenge ACK: this kernel does not have it. So `CVE-2016-5696` (inferring through the challenge ACK counter as a side channel) does not apply; the cost is that it has no RFC 5961 protection against blind injection either. Adding it would mean changing the state tests in TCP's receive path, a medium-sized change to an old stack; the content itself is protected by HTTPS and what blind injection can do is mostly break a connection, so it is **recorded as a known limit and not changed**.

### 2. What was taken out of the kernel (from 2026-10-17)

SELinux refuses these interfaces anyway, but "compiled in" and "not there at all" are two different things:

| Option | Change | Reason |
|---|---|---|
| `CONFIG_DEVKMEM` | Off | `/dev/kmem` maps kernel virtual memory as a file, and nothing here uses it |
| `CONFIG_USER_NS` | Off | Android creates no user namespaces, and 3.0's implementation was half-finished anyway |
| `CONFIG_SYSCTL_SYSCALL` | Off | The obsolete binary `sysctl()` interface, which bionic does not use |
| `CONFIG_STRICT_DEVMEM` | On | `/dev/mem` is left with device registers only (the tool that reads the i.MX6 clock registers still works) and can no longer read memory |

Android also asks a newer kernel for `CC_STACKPROTECTOR` and `DEBUG_RODATA`, and **3.0's ARM has neither** (`HAVE_CC_STACKPROTECTOR` was added to `arch/arm` later), so they cannot be had.

Kernel patch `1054` also added 802.11 hole-196 protection (anybody on the same wireless network can forge unicast frames with the shared broadcast key) and ARP/NA spoofing filters, which wpa_supplicant turns on when the screen goes off.

### 3. SELinux

- Measured on the device: `getenforce` is `Enforcing`.
- The policy was exported from the running kernel and examined with `sepolicy-analyze`:
  - there is one permissive domain: `su`. It exists only in a `userdebug` build and a release `user` build does not contain it.
  - no domain has the `unconfined_domain_type` attribute.
- This project's own policy (`device/amazon/pw3/sepolicy/*.te`) was read line by line: no `permissive`, no wildcards, and capabilities given in three places, each with an explanation (`init`'s `sys_module` to load a fixed set of drivers, `wake_alarm` for `healthd` and `pw3-init-check`, and `sys_admin` for `pw3-init-check` only because 3.0 wants it to read the DM table too, with the ioctls narrowed by `allowxperm` to two command numbers).
- The per-device calibration and waveform files have a `neverallow`: neither an application domain nor the shell may open them.

### 4. The system image

Examined on the exported `system.raw.img` (2026-10-17, a `userdebug` image):

- No world-writable file.
- The only setuid file is `/system/xbin/su` (04750, standard for a `userdebug` build). **It does not exist in a release `user` build**, and that is on the release checklist.
- `/system/xbin` also holds a batch of AOSP debug tools (`micro_bench`, `librank`, `procmem` and so on), likewise only in `userdebug`.
- No executable in it is one this project does not recognise.

### What the boot check does and does not guarantee

The entry starts Android from image files on the Kindle's user area (`pw3-testfile-init`), and it does not recompute the image's SHA-256 at every boot. After the first check passes, `/data/pw3/system-verified` records a stamp: the image's size, mtime and cluster allocation (extents), and the expected hash from that entry's configuration. The next boot skips the recomputation when all four match (`TestFileInit.h`, `SystemStamp.h`). It costs 512 MiB / 27.6 MiB/s = about 19 seconds (extrapolated from the 20.9 s measured at 576 MiB, not measured again), so this cache is worth one version update's wait.

The stamp compares only metadata and does not recompute the content, so by itself it cannot answer "the content was changed while those three stayed the same". For that there are two additions, neither of which costs boot time:

- **At boot**: look for a second thing on this disk claiming the image's clusters (`FatCrossLink.h`). There are two ways to claim them and neither sees the other, so both are checked:
  - **The FAT table**: the chain is a linked list, so a cluster crossed by two chains is pointed at twice, and a cluster taken as another chain's first is pointed at once (a first cluster should have no table entry pointing at it).
  - **Directory entries**: another directory entry only has to write one of our clusters as its first cluster and it shares our whole chain from there, **without one byte of the FAT table changing**. Only reading the directory sees it. So the whole directory tree is walked at every boot and any entry whose first cluster falls inside ours stops the machine there and then; our own two entries should each appear exactly once, and one more or one fewer stops it too.
  This disk is 3.3 GB with 8 KiB clusters, so the table is 1.6 MB -- about 60 ms at the eMMC's 29 MB/s -- and the directory tree a few hundred KB more. A cross-link is the most realistic source of "the content changed while the size, the mtime and the allocation did not" -- a write interrupted by pulling the drive, or a repair tool that guessed wrong, both produce one -- and it also means that writing to `/data` writes into the reader's books, and deleting that book frees the chain Android is running on.

  **This check does not cover the write the install itself makes** (a Codex review, 2026-10-30, since verified). It runs in the new kernel's preinit, which is *after* the handover, while the installer runs inside the stock system, rewrites `android/system.img` in place (`open(O_WRONLY|O_CREAT)` then `write()` in `installer/unpack/unpack.c`) and **does not read the FAT at all** -- everything FAT-shaped in the installer is cluster arithmetic over sizes. So **where the range the installer is about to overwrite shares clusters with a book**, that book's content is overwritten during the install, and preinit refusing the start afterwards is too late -- that range, and no wider. The precondition is a filesystem that is already damaged -- on a sound FAT the allocator does not hand one cluster to two files -- so this is not a reproduced loss of books, but **the edge of the protection has to be said plainly: the install has no such check**. See `docs/open-items.en.md`.
- **After boot**: a boot that skipped the full check leaves `/pw3-system-recheck` (the device node, the length, the expected hash). `pw3-system-verify` waits 90 seconds after `boot_completed` and reads the whole of dm-1 at the lowest scheduling and I/O priority, dropping each block from the page cache as it goes. On a match it sets `sys.pw3.system_verified=1`; on a mismatch it deletes the stamp, and the next boot does the full check, fails it and refuses to start -- which is the path the installer's "the system image is damaged" screen describes.

So the accurate statement is:

- **Guaranteed**: this version of the image was fully checked at least once on this machine; and at boot time there was no second chain and no second directory entry on this disk claiming any of its clusters.
- **Not guaranteed: that every boot re-checks it** (found by a Codex review, 2026-10-30). The background re-check starts only after `boot_completed`, then waits another 90 seconds, runs at low CPU and I/O priority, and has no deadline; it may not finish at all if the file it asks for is missing, unreadable or fails to parse, or if the device read fails. In the ordinary case it reads the whole image through within a few minutes of the boot, but that is **what is measured, not what the code promises**.
- **Not guaranteed**: that during the minute or two between boot and the end of the background recheck, a modified image is not running.

Against the threat of "somebody can run code on the Kindle side", none of this changes the attack surface -- they could equally replace the launcher itself, and the launcher is the anchor of the whole hash chain. These are aimed at damage. The other route is porting dm-verity for lazy per-block verification, and it was not chosen: 3.0.35 would need the driver backported, and with 512 MB of memory here code pages are evicted and read back over and over, with a recomputation on every read, which averages out more expensive than reading the whole image once at boot.

### 5. The programs this project added

- `WeReadInstaller`: downloads only from Tencent's own address (HTTPS, with the whole redirect chain HTTPS), and checks the package name and Tencent's signing certificate SHA-256 before installing. It holds `INSTALL_PACKAGES`, granted explicitly by a `privapp-permissions` file. Two rules stop it, each about a different thing (written accurately after a Codex review, 2026-10-30):
  - **Installed once, never installed automatically again** (the `DONE` latch). After the reader uninstalls it, nothing automatic brings it back; but "download now" on the home-screen entry **clears that latch on purpose** (`Installer.scheduleNow`) -- that is the reader asking, which the rule was never about.
  - **Three downloads that were not Tencent's, and it stops trying by itself** (`MAX_REJECTED`). It counts only the identity check failing; a network or storage failure is not counted and keeps retrying on the back-off that starts at ten minutes.
- The power HAL's new write touches one file (`scaling_min_freq`), with a SELinux type of its own that only it may write.
- The composer, gralloc, lights and the rest read and write no per-device identifier.
- The logs the entry writes into the user area (`android/logs/<time>/`) are what the person hands over as they are, so **the kernel log in them is filtered by an allow list** (`boot/launcher/userlog.c`): the stock kernel log really does carry the device's MAC (40 places in 201 snapshots from this machine, 20 of them `usb0: MAC ...`), and one spelling missed by a deny list is one leak, so what is kept is the subsystems this project uses, the lines from a kernel that went wrong, and the lines this project prints itself with a `pw3-` prefix (see the comment in `userlog.c`: a list of tags alone would silently drop diagnostics added later -- which really happened on the device during development, where what went missing was exactly kernel patch 1046's cable and pull-up state), and nothing else is written. Letting a prefix through costs **classifying our own tags**: `pw3-boot-report` goes in `DENY`, which is checked before the allow list. It went there because it relayed whole logcat warning and error lines into the kernel log, text an application wrote and where file names and addresses may be; since 2026-10-29 it reports counts against a fixed list of thirteen tag names (`Relevant()` in `BootReport.cpp`), and the kernel-log sink it wrote to went with `androidboot.pw3.diag`, so the tag reaches no kernel log at all. **It stays in DENY because nothing has to be let in, not because its content is still private** (the comment in `userlog.c` says so). And `tools/tests/test_klog_tags.py` makes sure every `pw3-` tag in the tree has been classified and that a new one fails the build until it is; after filtering, the MAC, long hexadecimal and `ssid`/`serial`/`command line` checks run again and anything caught is not written either. Measured, 10.2% is kept, and of 96,545 filtered lines, 0 carry an identifier. The test entry does not filter -- those folders only travel by cable.

### 6. The privileged permission allow list (examined, left off)

Android 8.1 can require that "a privileged application taking a `signature|privileged` permission must be listed in
`/system/etc/permissions/privapp-permissions-*.xml`" (`ro.control_privapp_permissions=enforce`).
This machine does not set that property, so it is not enforced: an unlisted permission is granted anyway, with one warning logged.

On 2026-10-19 the APKs of all 21 privileged applications in the `user` image were unpacked, the permissions they request read out, and compared with the platform's 178 `signature|privileged` permissions and the allow list. **Only two were not covered, and both are AOSP's own oversights**:

| Application | Permission | Why |
|---|---|---|
| `android.ext.services` | `PROVIDE_RESOLVER_RANKER_SERVICE` | The allow list says `com.google.android.ext.services` while the AOSP build installs `android.ext.services`, so the package names do not match |
| `com.android.systemui` | `com.android.permission.WRITE_EMBEDDED_SUBSCRIPTIONS` | eUICC, and this machine has no baseband |

**Why `enforce` is left off**: that allow list guards against "a privileged application quietly taking more permissions", and the privileged applications here are either AOSP's or this project's own, `/system` is read-only and this image has been checked in full on this device at least once (not recomputed at every start -- see "What the start-up check does and does not guarantee" above), and a third party has no way to make itself privileged. What enforcing would buy in detection can be had from one static check instead (the table above is one), while the cost is possibly refusing a permission at runtime and breaking an application in a way nobody can explain. **Detection belongs on the build side, not at runtime.**

What would change this: the day a privileged application somebody else wrote is brought in, or a CDD submission.

### 7. What to check before every release

1. Build as the `user` variant and confirm: no `su`, no debug tools in `/system/xbin`, `ro.debuggable=0`, adb off by default. How it was done and what it showed on 2026-10-19 is in "The `user` build rehearsed" above.
2. Run sections 3 and 4 again on the `user` build.
3. Publish the GPL source package (`tools/collect-copyleft-sources.py`) with the image.
4. `tools/check-image-references.py`: every program an init `.rc` in the image names is there, and so is every shared library. On the `user` image of 2026-10-19: 59 programs, 4311 library references, nothing missing.
5. `"entry"` in the entry receipt is `release`, and the command line holds only the arguments `CMDLINE_ANDROID` in `boot/build_entry.py` lists. The test entry and its scaffolding have been removed and `build_entry.py` no longer accepts `--test`; check that the command line has no argument beginning with `androidboot.pw3.`.

### 8. What is in the folder the reader hands over (measured 2026-10-20)

The release entry leaves `android/logs/<time>/` at every tap, and by design that is the thing to send when something goes wrong. Only half of it is filtered: the kernel log goes through `userlog.c`'s allow list and the identifier check, the report area through the identifier check, and **`launcher.log` (helper's standard output, redirected by the shell) and the records written by `write_result()` are free text from our own code, not filtered at all**. The safety rests on "we never print an identifier ourselves", and an invariant like that rots silently as code is added -- the `pw3-` prefix is exactly how it leaked once before (see `test_klog_tags.py`).

So it was checked twice, both by measurement rather than inference:

1. **The privacy check was run over log folders really fetched off the device** (`tools/check-private-data.py` with this machine's denylist, which is to say with the reader's own serial number, MAC and network name as the test): two folders, 26 files, **0 findings**. One of them is a test entry's folder containing **an unfiltered raw kernel ring buffer** (`previous-printk-buffer.txt`), which was clean too -- and that does not contradict `userlog.h`'s record of "40 of 189 logs carry a MAC": that rule guards the ones that do, and this one happened not to.
2. **The device data failure texts were read one by one** (every `fail(why, ...)` in `devicedata.c`, written into `launcher.log` verbatim as `PW3_DEVICE_DATA %s`): all of them are fixed words plus a path, a byte count and an errno. Not one interpolates a value -- the board number and MAC lines say only "not 16 alphanumerics" and "not 12 hexadecimal digits" and never what was read (`devicedata.c:236`, `237`). The download line has only curl's return code, the HTTP code and a byte count, and no address.

The conclusion: the folder handed over on 2026-10-20 was clean. That is a measurement, not a mechanism; the only real mechanisms are the two gates on the kernel log and the report area, so **anything new that writes into that folder is either fixed words or goes through `pw3_report_keep`**.

### Appendix: the errors a release still logs, and why each one stays

These still appear in a release's log. All of them are understood and each has a reason not to change it:

| Error | Cause | Why it stays |
|---|---|---|
| `Netd: Error destroying sockets` | `SOCK_DESTROY` is Linux 4.5; netd uses it to close connections bound to a network the moment that network goes away | Without it a connection waits for its own timeout (a quarter of an hour at worst) rather than failing at once. Backporting means touching `inet_diag` and TCP's abort path, a medium-sized change to an old stack; a reading device rarely changes network, so it is recorded as known |
| `SupplicantStaIfaceHal: setSuspendModeEnabled / setBtCoexistenceMode / stopRxFilter failed` | ath6kl's supplicant does not support those three commands | There is no Bluetooth here; power saving with the screen off is done by unloading the Wi-Fi driver about 60 seconds after the screen goes off, which is more thorough than suspend mode; and the receive filters are done at the IP layer by kernel patch 1054's four switches |
| `UsbPortManager: usb hal service not found` | There is no `android.hardware.usb` HAL | That HAL manages USB-C role switching (host or device, which way power flows); this is micro-USB and device only, and running a HAL process to silence three log lines is not worth it |
| `hw-IPCThreadState: attemptIncStrongHandle: Not supported` | Known behaviour of HIDL passthrough mode | An implementation detail of AOSP itself, not a problem here |
| `wificond: No Offload Service available` | There is no Wi-Fi offload HAL and the chip cannot do it either | The offload HAL lets the chip scan by itself while the host sleeps; ath6kl has no such ability |
| `avc: denied { getattr } ... mediaprovider ... cache_recovery_file / cache_private_backup_file` | The download manager's idle maintenance (`DownloadIdleService`; the line just above it in the log is `Removed 0 stale downloads`) walks the download cache directories and calls `stat` on `/data/cache/recovery` and `/data/cache/backup`. Those two belong to the recovery partition's log and the backup staging area, each with its own SELinux label, and `mediaprovider` cannot read them | Not changed. Neither directory can hold a download, so skipping them loses nothing; loosening the policy to silence three lines, or hiding them with a `dontaudit`, would both make a real denial harder to find later. Three lines per idle maintenance, measured 2026-10-19 as once in a whole day |

One more, `iptables-restore: unable to initialize table 'nat'`, no longer appears: netd sets up the IPv4 nat table every time, the stock kernel configuration had none, and the kernel configuration (`config/android-core.config`) now has `CONFIG_NF_NAT` and `CONFIG_IP_NF_TARGET_MASQUERADE` (2026-10-17).

## The two pieces of data the system carries that expire: root certificates and time zone rules

Researched and generated on 2026-10-16. Checked in the image on 2026-10-19: `/system/usr/share/zoneinfo/tzdata` is `tzdata2026e` (it was 2017b) and `/system/etc/security/cacerts` has exactly 143 roots, as this document expects. The times in the 10 zones below have not been checked one by one on the device -- the data being in the image is a fact, and it displaying correctly is an inference.

Android 8.1's own root certificates stop in 2017 and its time zone rules are tzdata2017b. Neither ages with the code; both expire with time:
- Root certificates: expired and distrusted roots are still trusted, and newly issued roots are not known.
- Time zones: countries have changed daylight saving and zone rules, and this machine still computes by 2017, showing a local time an hour out.

Both are data files read at runtime, and replacing them does not touch the boot image, so installed applications need no recompile.

### Root certificates

**What 8.1 shipped**: `/system/etc/security/cacerts` has 135 roots, from 2017. Against Android's current list:
- 72 are missing, among them GTS Root R1-R4 (Google), ISRG Root X2 (Let's Encrypt), Microsoft 2017, Sectigo R46/E46, DigiCert G5, and the Chinese CAs BJCA, GDCA, TrustAsia, UCA and vTrus;
- 64 are present that Android has removed, including the distrusted Symantec family (VeriSign, GeoTrust, thawte), TÜRKTRUST, E-Tugra and Certinomis.

**What it costs**:
- Verifying 31 common sites' certificate chains against this set with OpenSSL, all of them pass, WeRead, Baidu, Zhihu, Bilibili, GitHub, Google, Microsoft, Wikipedia, Let's Encrypt and MobileRead included. The reason is that sites carry cross-signatures for old Android. So this is not "web pages do not open" today.
- The problem is at both ends: distrusted roots are still valid, which is a security risk; and as old roots expire (GlobalSign R1 in 2028, say) and sites drop their cross-signatures, a 2017 list will start failing. For long use and an open-source release, it should follow Android's current list.

**What is done**: `patches/aosp-ca-certificates/0001`, generated by `tools/sync-ca-certificates.py`.
- The source: the newest public update of `system/ca-certificates` (`c5434e0a8490`, "Update certificates to NSS_3_112_RTM", android16-qpr2-release, 143 roots).
- Pinned: the script pins that commit and checks the content by SHA-256 (a listing sorted by file name), depending on nothing about how Gitiles packs it.
- Compatible with 8.1: the file names are the certificates' OpenSSL `subject_hash_old`, which is what 8.1's Conscrypt `TrustedCertificateStore` looks up; all 143 names were checked against their certificates with no collision; and the file format is 8.1's (PEM plus a text description).
- With the new list, the same 31 sites all verify.
- A build note: deleted certificate files stay behind in `out/`'s staging directory (207 = 143 + 64 old ones, measured), so the build that changes the certificates has to carry `PW3_AOSP_INSTALLCLEAN=1` and the image checked for exactly 143 afterwards.

### Time zone rules

**What 8.1 shipped**: the `/system/usr/share/zoneinfo/tzdata` that bionic and libcore read, and ICU's `icudt58l.dat`, are both 2017b. Running a checking program on the device (a development build still carrying the 2017b data) with `app_process` reading libcore and ICU at once, 7 of 10 cases are an hour out:

| Zone | Instant (UTC 12:00) | Here (2017b) | Should be |
|---|---|---|---|
| America/Sao_Paulo | 2026-01-15 | -2 h | -3 h (daylight saving abolished in 2019) |
| America/Mexico_City | 2026-07-15 | -5 h | -6 h (abolished in 2022) |
| Asia/Almaty | 2026-07-15 | +6 h | +5 h (unified in 2024) |
| America/Asuncion | 2026-07-15 | -4 h | -3 h (2024) |
| Africa/Cairo | 2026-07-15 | +2 h | +3 h (daylight saving restored in 2023) |
| Asia/Amman | 2026-01-15 | +2 h | +3 h (2022) |
| America/Godthab | 2026-07-15 | -2 h | -1 h (2023) |
| Asia/Shanghai, Europe/Dublin, America/New_York | -- | Correct | Unchanged (the controls) |

China's zone is unaffected; a reader in the countries above would see a time an hour out.

**What is done**: `patches/aosp-timezone/0001` and `patches/aosp-icu/0001`, generated on the build machine by `tools/update-timezone-data.py`, following 8.1's own `system/timezone/update-tzdata.py` flow with the inputs changed into a form today's tools can handle:
- **IANA tzdb 2026e**: verified against Paul Eggert's signing key (`7E3792A9...62AA7E34`), with the script pinning the SHA-256.
- **bionic and libcore**:
  - generated with the zic of the same release, in rearguard form (daylight saving never negative, which 8.1's code assumes), with all of backzone, as in 8.1;
  - `zic -b fat`: 8.1's libcore reads only each zone's 32-bit section;
  - packed at the end by 8.1's own ZoneCompactor.
- **ICU**:
  - the time zone resources ICU published for 2026e (unicode-org/icu-data `bae5ca64`, the four `.res` files in `tzdata/icunew/2026e/44/le`; format 44 applies to ICU 4.4 and later), with the git blob checksums pinned;
  - put into `icudt58l.dat` with the `icupkg` of ICU 58 from the 8.1 source. This is what ICU's user guide says to do to update time zone data.
  - Building `icupkg` turns off ICU 58's dependency on `xlocale.h` (gone since glibc 2.26, fixed only in ICU 60). That affects the host tool only; the ICU on the device is unchanged.
- **Checks while generating**:
  - the tzdata header is `tzdata2026e`;
  - every zone ID 8.1 had is still there. The one exception is the alias `Canada/East-Saskatchewan`, deleted by IANA in 2017c, which no Android since has had and which 8.1's settings do not offer either;
  - `icudt58l.dat`'s entry listing is unchanged, and the four resources put in are byte-identical to the inputs.
- **What is not regenerated**:
  - `tzlookup.xml`: maintained by hand in 8.1 and used only to pick a zone by country from the telephone network, which this machine does not have;
  - ICU's text source files: the build uses only the packed `.dat`;
  - the time zone update distro files: this machine has no component to install a distro.

### Two things checked in passing and left alone

**The Wi-Fi regulatory domain**:
- With Wi-Fi off, cfg80211 is in the world domain `00`, where channels 12 and 13 can only be scanned passively.
- Once Wi-Fi is up the kernel log says `Regulatory domain changed to country: CN`: the region in the wireless chip's EEPROM takes effect, channels 12 and 13 open by China's rules, as in the stock system.
- 8.1 takes the country code from the SIM's mobile country code, and this machine has no telephony, so `dumpsys wifi` says "CountryCode was not initialized". The line above already covers it and nothing more is needed.

**The browser engine (WebView)**:
- Currently AOSP's official prebuilt 128.0.6613.88. The requirement is to use the prebuilt AOSP itself publishes (settled 2026-10-13).
- Checking `external/chromium-webview` on 2026-10-16: the newest on main is still 128.0.6613.88 (2024-10). What was written from that then: "after that the official builds jump straight to 140 and 141 (android16-qpr2-release, 2025-08), and Chromium has required Android 10 since 139, so 128 is still the newest official build usable on 8.1, and the decision stands".
- **Checked again on 2026-11-04, and that premise does not hold.** On the same repository's `android16-qpr2-release` branch there are official prebuilts of 133, 134, 136 and 138 after 128 and before 140. The newest that 8.1 can install is **138.0.7204.67**: commit `8bf71e6228` (2025-07-17), whose message says "This build IS suitable for preview or public release", with all four APKs (arm, arm64, x86, x86_64) at `sdkVersion=26` (minSdk 26, Android 8.0 and later); only from 140.0.7339.4 (2025-08-06) is it `sdkVersion=29`, which needs Android 10. So Chromium 138 being the last release for Android 8 and 9 is right; what was wrong is "AOSP never published it". How it was checked: the commit log of `+log/refs/heads/android16-qpr2-release?format=JSON` on Gitiles, then each commit's message (`+/<commit>?format=JSON`). **The ground for "the decision stands" is therefore gone, and whether to change is the owner's to decide again**; until then the image keeps 128. Whether 138 runs on this machine (the 3.0.35 kernel, no GPU, 512 MB) has not been tried.
- The known remaining risk: the V8 and other vulnerabilities fixed in 129-138, some exploited in the wild (CVE-2025-6554 among them), are still in 128. This line went on to say "the only newer option is LineageOS's own build of 138, which is not an official prebuilt", which is wrong: AOSP itself published 138.0.7204.67 (previous item). Nor would it close everything: CVE-2025-6554 was fixed in 138.0.7204.96, and by its number .67 comes before that; which fixes each of these builds carries has not been checked one by one.
