# Installing a large application here (WeChat Reading as the example): the research and the plan

The design of the two ways an application gets onto the device -- by hand, following Tencent's own procedure, and automatically, through the downloader the system carries -- and of how its code is compiled before and after. Researched and measured from 2026-10-16; the patch behind each change is named in its section.

## What is wanted

- WeRead cannot ship with the system (it is Tencent's proprietary software), so in an open-source release the owner installs it.
- The way to install it has to be within an ordinary owner's reach. Best of all is Tencent's own published procedure for "other e-paper devices", rather than a second one invented here.
- It has to be usable afterwards: opening the application, or a book in it, must not become markedly slower because its code was never compiled to machine code.

## Tencent's own procedure

[The WeRead e-paper installation guide](https://ink.qq.com/ink/guide):
1. Download the package on a computer;
2. Connect a cable with the device in "USB file transfer mode"; on macOS, Google's Android File Transfer;
3. Copy the package to the device;
4. Open it in the device's file manager and choose "install".

What this system has to match that:
- USB file transfer: MTP (`docs/design/usb-mtp.en.md`). With no lock screen password, plugging in is enough and no notification has to be tapped.
- A file manager: the files application (DocumentsUI). The first time an application is installed from it, 8.1 asks the owner to allow "files" to install unknown applications in Settings. That is AOSP's own flow, unchanged.

## What is compiled, and when

The order is an ordinary Android phone's: install it and let it be used, and compile machine code once the device is idle. What differs is the level the background compile goes to, and the conditions it waits for.

| When | Compiler filter | Where it is set | Measured with WeRead 2.1.2 |
|---|---|---|---|
| At install | `quicken`: no machine code, the dex is quickened only | `pm.dexopt.install`, AOSP's default, which `pw3.mk` does not override | 72-93 s to compile (five runs), 76 MB of output |
| When the device is idle | `speed`: every method compiled to machine code | `pm.dexopt.bg-dexopt=speed` (`pw3.mk`) | About 20 minutes to compile, 176 MB of output |

The second step is done by AOSP's idle compile job (`BackgroundDexOptService`). Carried over unchanged, that job does almost nothing on a Kindle, and this project changes three things about it:
- The level: AOSP uses `speed-profile`, which compiles only the methods the usage profile recorded; here it is `speed` (`pw3.mk`, since 2026-10-28), for the reasons in the next subsection.
- The condition: AOSP waits for the device to be idle and charging. A Kindle runs for weeks on one charge, so that is almost never true, the job never runs, and an application stays at `quicken` for good. frameworks/base 0066 replaces "charging" with "battery not low" and keeps "device idle".
- Being interrupted: AOSP's installd deletes the old output as soon as a compile starts and writes the new one in its place, so for the whole compile the application has no machine code, and an interruption leaves it with neither the old nor the new. frameworks/native 0009 has each artifact written to `<path>.staging` first and renamed over the old one atomically once dex2oat has succeeded (the oat last), so at every instant the application has a whole set of code, the old one or the new one.

When the owner picks the device up the job is cancelled; a dex2oat still running runs at nice 10 (installd's own setting), which leaves a foreground application about nine tenths of the CPU.

Measured on the device (2026-10-28): the job is registered with the new conditions (`charging=false batteryNotLow=true deviceIdle=true`). With WeRead left at `quicken` and the screen off, the device became idle and the job started after about 71 minutes, compiled the 100.3 MB odex in about 20 minutes and left no staging file behind, with nobody touching the device. During another compile, checked every 10 seconds, the real `base.odex` stayed the old one until the new one replaced it, and `dumpsys package dexopt` said `kOatUpToDate` throughout. The power and the heat of one compile on battery have not been measured on the device yet.

### Why it is split this way

| Measurement | Comparison | Result |
|---|---|---|
| Cold start before signing in, empty shelf (2026-10-20, six paired rounds, page cache dropped before every start) | `quicken` against `speed` | `quicken` 3.9 s slower (95% interval 0.3-7.5 s, p = 0.04) |
| The same cold start, another six paired rounds | `speed-profile` against `speed` | `speed-profile` 2.0 s slower (95% interval -2.1-6.1 s, p = 0.28) |
| One use: tapping the icon on the home screen, loading the shelf, opening a book (2026-10-28) | Four rounds of `speed-profile` against two of `speed` | 95.6 s against 51.4 s; process CPU 73.0 s against 30.2 s; the two groups do not overlap |

- Not `speed` at install: it takes 20 minutes with the CPU saturated and about 6 MB of memory free (next section), so the device cannot be used at exactly the moment the owner has tapped "install"; what it buys is a few seconds of cold start. Compiling everything at install was once justified by "with no machine code WeRead takes 129 seconds to its shelf and shows 'not responding'"; that was measured before the Java heap was given more room, and it has not reproduced since (about 41 seconds, no dialog).
- `speed` rather than `speed-profile` in the background: the two cold-start sets show no difference because a cold start before signing in, with an empty shelf, runs little code, most of it already in the profile. Loading a shelf and opening a book run the EPub parser, the CSS parser and per-character typesetting, which the profile does not have, so they are interpreted from end to end. Garbage collection barely moves (10.3-15.5 s against 8.0-11.5 s), so the 43 s difference is the cost of interpreting.
- What it costs: about 99 MB more of `/data` for WeRead (978 MB was free when it was measured); for resident memory see `performance.en.md`, section 12. `speed` writes no reference profile, and nothing here needs one: only a profile-guided compile produces an application image, and an application image is worth about 0.4 s (next section).

## What one full compile costs (measured)

This is what the background job does when it compiles to `speed`. Measured on 2026-10-16 with `cmd package compile -m speed -f`:

| Item | Measured |
|---|---|
| Compiling all of WeRead here (13 dex files, 70.5 MB) | **1199 s (20 minutes)**, with the CPU saturated throughout |
| dex2oat memory | About 270 MB resident; about 6 MB free during the compile, with 112 MB swapped to zram |
| Flash | 329 MB read, 427 MB written |
| What it produced | 105 MB of odex and 71 MB of vdex; **no application image** (installd makes one only for a profile-guided compile) |
| What no application image costs | Three cold starts each, alternating: with, 17.6/17.2/18.4 s; without, 18.5/18.0/17.8 s -- a mean difference of about 0.4 s, inside the spread. installd is not changed for it |

Other full compiles took between 1214 and 1410 s, the same order.

## PackageManager's watchdog and the compile wake lock

Confirmed by reading the 8.1 source:
- The install happens on PackageManagerService's handler thread (`processPendingInstall` -> `installPackageLI`), and so does the compile (`performDexOpt`, `REASON_INSTALL`).
- The watchdog gives that thread `WATCHDOG_TIMEOUT` = 10 minutes (`Watchdog.addThread(mHandler, WATCHDOG_TIMEOUT)`). Past that it kills system_server, which restarts the whole interface and fails the install.
- The wake lock held during every compile (`PackageDexOptimizer.performDexOpt`) has the same limit plus one minute. Installs, `cmd package compile` and the background job all compile through it.

AOSP's comment says that thread sees "very long I/O", which is why the limit is 10 minutes; on a phone, compiling one application takes seconds. Here, compiling WeRead to `speed` takes 20 minutes, so an install with `speed` would restart in the tenth minute when installed by the official procedure. With `quicken` at install, installing WeRead stays well inside the limit; the background job's compile to `speed`, about 20 minutes, still holds the same wake lock.
- The watchdog half was derived from the source and has never been triggered on the device: triggering it means restarting the framework.
- The 1199 s above came from `cmd package compile`, which compiles on a binder thread and is not watched.
- The wake-lock half of it has been seen on the device: at the end of that 1199 s compile, system_server logged a wtf -- `WakeLock *dexopt* time out. Operation took 1198928 ms.` (`PackageDexOptimizer.releaseWakeLockLI`). So the wake lock expired in the eleventh minute; that run was attached to a computer and the device did not sleep, or the rest of the compile might have been suspended with it.

## Loosening the limit: 0035

**frameworks/base 0035**: `WATCHDOG_TIMEOUT` multiplied by `ro.hw_timeout_multiplier`.
- It is 4 on this machine, so 40 minutes, and the wake lock becomes 41 -- enough to cover a `speed` compile of about 20 minutes.
- That multiplier is the same product parameter frameworks/base 0012 introduced to loosen deadlines for slow hardware, named and meant as later Android's `Build.HW_TIMEOUT_MULTIPLIER`.
- It changes only `services.jar`; the boot image does not change.

**What it costs**: a PackageManager handler thread that really is stuck takes 40 minutes to be noticed rather than 10. That thread has legitimate minutes-long work on it anyway, and 10 minutes on a phone is only "long enough" too.
- A compile on the handler thread that passes 20 minutes, half of 40, makes the watchdog write every thread's stack into `/data/anr/traces.txt` -- a record, not an abort.

**The margin**:
- 40 minutes is about enough to compile 140 MB of dex to `speed`; WeRead's 70.5 MB takes 20 minutes.
- Every other application here (EinkBro, the keyboard, the system applications) is more than an order of magnitude smaller.

## Considered and not taken

| Approach | Why not |
|---|---|
| `speed` at install | The install takes 20 minutes during which the device is barely usable, and it runs into the handler thread's watchdog; the cold start it buys is about 3.9 s. The same machine code is compiled by the background job while the device is idle |
| `speed-profile` at install | A first install has no profile, so the result is the same as `quicken` (the dex is quickened either way) for two and a half times the work: 147 s against 62 s on the same device, a 1,057,192-byte odex both times (measured for frameworks/base 0051) |
| `speed-profile` in the background (AOSP's default) | Opening a book takes 95.6 s against 51.4 s: the methods opening a book runs are not in the profile |
| Moving the compile off PackageManager's handler thread | It would mean changing the install flow itself (the install lock, freezing the application and the order of the result callbacks are all tied to that thread), which is a large change; loosening the deadline is enough |
| Giving installd an application image | Measured at about 0.4 s, inside the spread |

## What the owner sees

- The install screen has the application's name at the top and a progress bar with "installing..." in the middle. The bar tracks only the copy of the package into the session, and while PackageManager compiles the screen does not move (72-93 s for WeRead). Global animation is already off, so nothing extra is spent on the CPU.
- **PackageInstaller 0001 and 0002**: ten seconds after PackageManager has the package and is still not done, the line under the bar becomes
  > Preparing the app for this device...
  > A large app can take a few minutes.
  - Why ten seconds: past about ten seconds attention is not held and the interface should say how long the wait is (Nielsen, *Usability Engineering*, 1993, the three response-time limits). An application that installs within ten seconds never shows this line.
  - Why "a few minutes" rather than the measured seconds: that figure belongs to WeRead alone, and a screen that names a number the next application cannot keep is worse than one that gives the order of magnitude.
  - Only the text of the existing line changes: no new control, and nothing specific to one application; the new strings are written in the default (English) and in Simplified Chinese only.
- For the minute or two of the compile the CPU is saturated, so other applications are slow.
- The later compile to `speed` happens while the device is idle, with nothing on the screen; until it is done the application keeps running the code it already has.
- Not yet verified on the device: the whole manual path from opening the package in "files", including when that line appears and whether the installed application's icon lands in a free cell of the first screen.

## WeRead installing itself: the research and the plan

The direction settled on 2026-10-16: the release carries no WeRead; the system downloads it from Tencent's own address on the spot, checks that it is signed by Tencent and installs it, with nothing more for the owner to do. Installing by hand (Tencent's own procedure, above) still works.

### Why it is not packaged

WeRead is Tencent's closed-source application. Putting its package into a publicly released image is redistributing it on GitHub, and without permission that risks a takedown. Downloading from Tencent's own address on the spot means its contents never pass through us.

### The official address (read 2026-10-16)

"Download the WeRead e-paper package" on Tencent's e-paper guide (https://ink.qq.com/ink/guide) resolves, in the page's script, to `/web/download_ink`:

| Step | Address | Result |
|---|---|---|
| 1 | `https://ink.qq.com/web/download_ink` | 302 -> `https://weread.qq.com/download?p=105&c=900` |
| 2 | `https://weread.qq.com/download?p=105&c=900` | 302 -> `https://download.weread.qq.com/pkg/105/releases/2.1.2.10245900_900.apk` |
| 3 | `download.weread.qq.com` | 302 -> Tencent's CDN; `application/vnd.android.package-archive`, 127,467,642 bytes, 2026-06-23 |

- The entry address is fixed and points at the current version, so whatever is installed is whatever Tencent has published, with nothing for us to follow.
- What came back this time was 2.1.2 (versionCode 10245900), the same size as the one installed on the test device.
- Step 2 returns 404 to a HEAD request; it needs a GET.
- Tencent's guide also has a "Kindle" row, which says a stock Kindle cannot install third-party applications and to use the web version at `r.qq.com`.

### How it is checked

The APK has a v2 signature only (there is no `META-INF/*.RSA`). The signing certificate's SHA-256:

```
e4dd2f049941989ffafa45aef52eda9701372c27bc1708e89c217935140bfa1c
```

Read off the 2.1.2 installed on the test device. Android requires an update to an application to carry the same certificate, so this value does not depend on the version. Two things are checked: that the package name is `com.tencent.weread.eink`, and that the signing certificate equals the value above. Whether the signature itself is intact is verified by PackageManager when it parses the package (`PackageParser.collectCertificates`).

### Which side does it

| | Downloaded by the installer in the stock Kindle system | Downloaded by Android itself after it first has a network |
|---|---|---|
| Network | The stock system already has Wi-Fi configured | Wi-Fi has to be joined in Android first; using WeRead needs a network to sign in anyway |
| Checking the signature | The stock system would have to parse the v2 signature block itself, and could not verify its integrity | PackageManager does it natively |
| Where it is kept | 122 MB more of the FAT user area; in Android the FAT is read-only and only root can read it, so getting it across needs another idea | The application's own directory |
| Reinstalling later | Not possible | Possible |

**Chosen: the Android side.** Checking the signature and installing are both native to Android and are not rebuilt in the stock system.

### The plan

A very small system application (working name: the Readom application installer) with `INSTALL_PACKAGES` (a privileged permission, added to `privapp-permissions`):

1. At boot completion with a network up, it checks: WeRead is not installed, and the owner has not uninstalled it (it installs once; after an uninstall it does not come back on its own).
2. It downloads from `https://ink.qq.com/web/download_ink` (following redirects, resuming after a dropped connection), with the progress in a notification.
3. It checks the package name and the signing certificate; on a mismatch it deletes the file, says so in the notification, and does not install.
4. It installs through a PackageInstaller session without a confirmation dialog. The install compiles to `quicken`, a minute or two here, and the notification says "preparing its code for this device; a minute or two".
5. It deletes the package when the install is done.

The compile to `speed` after that is the same as for an application installed by hand: the background job does it while the device is idle (see "What is compiled, and when").

**The certificate chain (checked 2026-10-19)**: all four hosts on the redirect chain -- `ink.qq.com`, `weread.qq.com`, `download.weread.qq.com` and the final `*.dlied1.cdntips.net` CDN -- chain to **the same root: DigiCert Global Root G2**. That root is in this project's image (`/system/etc/security/cacerts/c90bc37d.0`, of 143). So this download chain validates on the device with no certificate carried along and nothing added to the root store. What came back that day was still 2.1.2 (versionCode 10245900).

### The implementation

`device/amazon/pw3/apps/WeReadInstaller`, package `org.readom.wereadinstaller`, a privileged application (`privapp-permissions-wereadinstaller.xml` grants `INSTALL_PACKAGES`).

- **When**: a JobScheduler job is queued at boot completion with "has a network" as its condition, persisted and retried with exponential backoff (ten minutes the first time). When the network drops or the job times out, what has been downloaded is kept and resumes next time.
- **It installs by itself once only**: seeing WeRead installed (by this application or by hand) records it as done and it acts no further; after the owner uninstalls it, it is **not** installed again by itself. A hand installation during a download voids that download too.
- **The entry follows whether WeRead is there**: after a successful install the entry component is disabled, so it is neither on the home screen nor in "all applications". Once WeRead is uninstalled the entry is enabled again (`StatusActivity.followWhetherWeReadIsInstalled`) and returns to "all applications" -- not to the home screen, whose cell has been handed to WeRead -- because this machine has to be able to get it back without a computer. Two places check the same thing: the moment of the uninstall (`RemovalReceiver`, on `PACKAGE_FULLY_REMOVED` -- one of the few a manifest-declared receiver still gets after API 26, which `PACKAGE_REMOVED` is not), and every boot (`BootReceiver`, catching the `pm uninstall -k` case that broadcasts neither). **The automatic download does not start again because of this**: only the owner pressing "Download now" starts it.
- **Resuming**: it records the ETag and the total length and resumes with `Range` and `If-Range`. Tencent's CDN supports ranges (206) and gives an ETag; when the version changes the server returns the whole file and it starts again. The CDN's final address carries the client's IP, which is not kept, and every attempt goes through the official entry again. The whole redirect chain was measured as HTTPS: `ink.qq.com` -> `weread.qq.com` -> `download.weread.qq.com` -> Tencent's CDN.
- **Checking**: `getPackageArchiveInfo(GET_SIGNATURES)` parses and verifies the signature (v2 included), the package name has to be `com.tencent.weread.eink`, and there has to be exactly one certificate whose SHA-256 equals the value above. On a mismatch the file is deleted, and after three it stops installing by itself and says so in the notification.
- **Installing**: a PackageInstaller session, `MODE_FULL_INSTALL`, completed by the system after the commit (a minute or two), with the notification saying so. A session interrupted by a restart is abandoned and retried. The package is deleted on success. Afterwards, once the device is idle, the background job compiles it to `speed`.
- **The notification**: one low-priority channel, with the download's progress updated at most every five seconds (every update on e-paper is a panel refresh). The text is written in both languages.
- **Concurrency and carrying on: three invariants** (every problem Codex rounds three to seven found here landed on one of these; they are written down because whoever next changes this code will meet them again):
  1. **One attempt at a time in a process.** A process-wide lock (`Installer.BUSY`) is held for the whole of `run()`; an attempt that cannot take it waits thirty seconds and then leaves itself to the retry. Without it, "download now" -- which cancels the running job and schedules another in the same breath -- has two attempts live for as long as the old one takes to poll the cancel flag, and both open and truncate the same file. After taking the lock the flag is asked again: in those thirty seconds, whatever wanted this attempt may have stopped wanting it.
  2. **A committed session always leaves a retry in the queue.** `attempt()` returns `RETRY` after `install()` succeeds and whenever it finds a session still active, so the job queues the retry before letting go. `install()` has exactly one caller, so every committed session comes from a job run -- and the retry survives whichever of the result callback and the worker thread runs first. **Do not replace this with "read a flag, then decide whether to schedule"**: that shape looks equivalent and leaves a window between the read and the old job's removal, where the flag is set with no job left to consume it, while the notification has already promised that it will try again. The cost is one extra job run per install, and that run returns at `finished()` in a moment.
  3. **The remembered length and ETag describe the file at `apk()`: they live as long as it does and are forgotten where it goes.** The length is also the test for "this is already the whole thing", so it must **not** be cleared when the download succeeds -- cleared there, an attempt called off between the last byte and the install comes back to a complete file it cannot recognise, asks for the whole thing again, and truncates it on the 200. When another object is to replace it, invalidate the old length **synchronously** (`commit()`, not `apply()`) before replacing the file: `apply()` writes in the background and may skip a version a later write supersedes, so "cleared first" is not a barrier on the drive -- after a kill, a device can come back to an old length that happens to equal a prefix of the new object, take that prefix for a finished download, and spend one of the three refusals on it.

Measured on the device (2026-10-23, on a system that had never had WeRead): the job started 11 seconds after boot; downloading and writing the session took about 100 seconds and the compile 124.7 seconds, 3 minutes 48 seconds in all; the home screen cell was handed to WeRead the second after. Three launches in a row then took 26.7 / 13.0 / 6.6 seconds to a cold start, none of them "not responding".

### The downloader hands its own cell to WeRead

**What has to be solved**: the downloader's cell should become WeRead once it is installed -- **in the cell it was in**, even if the owner has moved it elsewhere -- and the downloader should then disappear.

**Considered and not taken**: writing WeRead's package name into the system's default layout (`<resolve>`: WeRead if installed, the downloader if not). It worked, but it was wrong at the root: **the layout belongs to the image, and the image has no business knowing which applications the owner will install**, and the requirement is that the system carries no special logic for any one application. Nor can a thing like that be done halfway: once one package name is lying in the layout, the next one is only an argument about whether to add another, not a principle.

**What is done now**: only the downloader knows about this, so the downloader does it. At the moment the install succeeds and it is about to take itself off, it updates **its own row** in the launcher's home screen table to point at the installed application; the row's `container`/`screen`/`cellX`/`cellY` are not touched, so the replacement happens in that cell. Only then does it disable its own component.

That this can be done at all rests on interfaces the launcher **publishes itself**, and all four facts were checked against the built APK:

| Fact | How it was checked |
|---|---|
| The home screen table is an exported provider, `com.android.launcher3.settings/favorites` | `aapt d xmltree` over the release APK's manifest |
| Read permission `normal`, write permission `signatureOrSystem` | The same; neither the downloader nor the launcher sets `LOCAL_CERTIFICATE`, both are signed with the same release key and both are privileged, so either route works |
| An external write makes the launcher reload | `LauncherProvider.update()` calls `reloadLauncherIfExternal()` (which AOSP left there for this) |
| SELinux allows applications to call each other | `system/sepolicy/private/app.te`: `binder_call(appdomain, appdomain)` |

**The row carries no title and no icon** (`title`/`icon`/`iconPackage`/`iconResource` left empty): once `itemType` says "application", the launcher reads the name and the icon out of the package at every load, so a change of either in WeRead follows.

**What about the other path, "an installed application appears on the home screen"**: the system's path (a session that says a person asked for it -> `SessionCommitReceiver`) may already have put an icon somewhere, and the order is not ours to decide. So the handover is: update its own row first, and **only on success** delete the other rows pointing at the same application. The other way round, one failed update would delete the only icon.

**If it cannot be done, nothing is done**: another launcher in use, the owner having removed the downloader's cell from the home screen (where the system's icon is then the only one and must not be deleted), or the application having no launchable activity -- all three log one line, leave the home screen as it is, and let the downloader take itself off anyway, because WeRead really is installed.

**After the default layout changes**: this row does not disappear again. Changing the layout no longer clears the home screen table (`aosp-launcher3/0015`, `known-issues.en.md` section 6), so a handed-over cell stays WeRead. Once that cell is handed over, "the downloader's cell" is no longer on the home screen -- and the next layout change with a new version number would, by the difference, put it back. **It does not while WeRead is there**, and not by a special case either: the entry component is disabled then, and the layout parser (`AutoInstallsLayout.AppShortcutParser`) looks the component up with `getActivityInfo(cn, 0)` -- with flags 0 a disabled component is not found, so it throws `NameNotFoundException`, throws again for the canonical package name, logs `Favorite not found` and gives up on that row. So **taking itself off has to really disable the component**, not merely hide the screen; otherwise one layout change grows a second downloader icon. Conversely, once WeRead is uninstalled the entry is enabled again (above), and a layout change then really does put the downloader's cell back on the home screen -- which is also as it should be.

### An installed application appears on the home screen

**What is wanted**: an application the owner installs is on the home screen when it finishes. This is a different thing from the section above: that one is "the downloader's cell becomes what it installed", and this one is where **every other application** lands. Neither section knows any particular package name.

There are two gates here, and missing either means no icon.

**The first: who is allowed on the home screen.** After an install, `PackageManagerService.sendPackageAddedForUser` sends the default home screen a session broadcast carrying this install's `installReason`; Launcher3's `SessionCommitReceiver` queues an icon only for `INSTALL_REASON_USER` and returns for anything else. And AOSP's own PackageInstaller interface **never sets that field**, so it defaults to `INSTALL_REASON_UNKNOWN` -- which is why, unmodified, opening an APK from the file manager and installing it adds nothing to the home screen (measured during development). `aosp-packageinstaller/0003` adds that one line in `InstallInstalling.onCreate`: on this machine, whoever opens an APK is the machine's owner, and there is no second possibility.

**The second: which cell.** AOSP's `AddWorkspaceItemsTask` looks for an empty cell from screen index **1** onwards and never offers screen 0. On a phone that is right: there are several screens and screen 0 is the one with the search field and the widgets. Here there is one screen (`aosp-launcher3/0012`), so "no empty cell" -> a new page at the end -> the icon lands on page 2; and reaching page 2 means swiping sideways with nothing on the screen saying there is a page 2 (the all-applications arrow is gone, `0006`; the line AOSP draws under the page has alpha 0 before a scroll starts, so it answers a swipe rather than inviting one; the owner asked for the indicator to go). The icon really is placed and the person really cannot find it. `aosp-launcher3/0016` makes the search start from screen 0: the first screen is 4x4 with the search field in row 0, the layout takes 5 of the other 12 cells and leaves 7 free, and only when it is full does it look at later screens and only when they are all full does it make a page -- the last two steps as in AOSP.

**Row 0 has to be marked occupied**: the search field is not a row in the home screen table. `LoaderCursor` marks row 0 of the first screen occupied when it reads the home screen, so placement has to mark it too; without that, the first application installed is written into the cell behind the search field and then dropped at the next load rather than shown -- a problem AOSP never meets, because it never puts anything on that screen.

**Nothing is missed when the launcher is not in the foreground**: the queue this path uses is flushed the next time the launcher is shown (`InstallShortcutReceiver`), which is AOSP's own design.
