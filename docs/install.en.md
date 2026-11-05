# Installation

> Revised 2026-11-01. **A clean installation of v0.1.0 was run on a Paperwhite 3, really downloading the package; earlier, on 2026-10-19, a whole installation from an offline package and an upgrade that replaced only the system**, and the times below are what those runs took (read off the times of the log folders on the Kindle drive). What has not been verified yet is listed in `open-items.en.md`. The library entry is called "Readom". Chinese: `install.md`.

## Supported devices

| Requirement | Where to check |
|---|---|
| Kindle Paperwhite 3 (7th generation, 2015) | Settings → Device Options → Device Info |
| Software version 5.16.2.1.1 | Same place. On an older version, see "If the software version is older" below |
| Jailbroken, so that `.sh` scriptlets run from the library | Follow the guide at [kindlemodding.org](https://kindlemodding.org/jailbreaking/). Jailbreaking is not part of this project; weigh the risk yourself |

On any other model or version the installer shows E101 and stops without changing any file.

### If the software version is older

5.16.2.1.1 is the **last firmware Amazon made** for the PW3 (August 2023); there is nothing after it. So any PW3 that has been online is on it, and one that is not is usually one whose owner turned updates off after jailbreaking it. Updating brings it back into range, and the steps are the same kind of thing as copying the package in -- no Wi-Fi needed:

1. On the computer, open Amazon's [Kindle software update page](https://www.amazon.com/gp/help/customer/display.html?nodeId=GKMQC26VQQMM8XSW), find **Kindle Paperwhite (7th Generation)** and download `update_kindle_all_new_paperwhite_5.16.2.1.1.bin` (about 258 MB). **Do not download another model's file**; the wrong one will not install.
2. Connect the cable and copy that `.bin` to the **top level** of the Kindle drive -- not `documents`, which is where the package goes. Eject, unplug.
3. Settings → Device Options → Update Your Kindle. It restarts on its own; check the version in Device Info afterwards.
4. **The update removes the jailbreak** unless the Kindle Jailbreak Hotfix was installed. Afterwards, look for "Readom" in the library: if it is gone, jailbreak the Kindle once more with the guide above. `Readom.sh` itself is not deleted by the update.
5. Tap "Readom" in the library.

## What you need

- A USB cable and a computer (Windows, macOS or Linux; it is only used to copy files).
- At least **2.6 GB** of free space on the Kindle. (What the installer works out is 2.49 GB: a 512 MiB system image, 1.75 GiB of data space, and 64 MiB spare. If there is less, the screen says how many MB more are needed.)
- Battery above 40 %, or a charger connected.
- Wi‑Fi that can reach `github.com` or `readom.app`. The installer carries both addresses and tries them in that order, taking whichever answers. If neither does, download the offline package on the computer as well (see below).

## Installing

1. Download `Readom.sh` (about 8 MB) from this project's release page.
   - If the Kindle's network cannot fetch the package, also download `readom-system-<version>.img.zst` (about 191 MB) from the same page: that is the offline package.
2. Connect the Kindle to the computer and open the Kindle drive. Copy `Readom.sh` into the `documents` folder, and the offline package too if you downloaded it.
   - **No Kindle drive on the computer**: press the power button once to wake the Kindle, then look again. Asleep, it may only charge and not hand its storage over (seen here on 2026-10-22: the device was on the USB bus, with no drive on the computer). If that is not it, try another cable (some carry power only) or another USB port.
3. Eject the Kindle on the computer, then unplug it. A charger is fine from now on; do not connect it to a computer during the installation.
4. Tap "Readom" in the Kindle library.
   - The screen shows, in turn: checking → downloading (skipped with the offline package) → checking the package → installing, step 1 → installing, step 2, each with a progress bar.
   - Meanwhile the screen does not respond to touch, and a swipe down from the top does not open the Kindle menu, which would cover the progress.
   - The first installation **took about 15 minutes** on the device (2026-11-01, v0.1.0, from tapping "Readom" to done, download of 188 MB and decompression included). The run from an offline package (2026-10-19, an earlier build) took about 18 minutes without a download. Different builds and different networks, so the gap is not a speed-up; both say this takes about a quarter of an hour. Step 2 is the slowest: it writes about 1.9 GB of storage space on the Kindle.
   - Android then starts by itself, which takes about 3 minutes; the screen does not change meanwhile.
5. Afterwards the offline package is deleted. Keep `Readom.sh`: tapping it is how you start Android from now on.

If the battery runs out, the Kindle restarts, or you leave the installer by accident, nothing is lost: tap "Readom" again and it continues where it stopped. The download resumes where it stopped too -- it starts over only when the server will not resume, and the screen asks you to tap again in that case.

## Everyday use

- **Start Android:** tap "Readom" in the Kindle library; about 3 minutes.
- **Back to the Kindle's own system:** in Android, hold the power button and choose "Restart to Kindle".
- **If it hangs:** wait two minutes first. When a start-up check does not pass, the device restarts itself back into the Kindle's own system. If nothing has happened after two minutes, **keep the power button pressed until the screen changes** -- up to about 40 seconds -- and it comes back into its own system the same way, with no data lost. (Do not let go after a dozen seconds: the exact reset time has never been measured on this device, and Amazon's own instructions say 40 seconds.)
- **The screen keeps saying "Starting Android":** that is normal; the screen may stay the same for a long time while it starts (the first start after a version change also recompiles the installed applications, which takes longer). A screen that does not change is not by itself a failure: when the start fails, the Kindle usually reboots into its own system by itself within a few minutes; if it never does, hold the power button down until the screen changes.
- **How long does installing an app take**: open the APK with Files on the home screen. A warning about apps from unknown sources comes first ("Your tablet and personal data are more vulnerable to attack by unknown apps…"): tap Continue, then Install; when it is done, tap Open or go home, where the new app is in an empty cell. (Walked end to end on release candidate l on 2026-11-04, with an empty test app.) A large app takes a minute or two, and the screen says "Preparing the app for this device". This machine has no GPU, so an app's code is converted for it first, which is slower than a phone; after that it starts like anything else.
- **Where an installed app is**: an app you install appears in a free cell of the home screen. The home screen also has an "All apps" cell, which opens the list of everything on this machine (a swipe up on the home screen opens the same list).

## Updating

Copy the new `Readom.sh` into `documents`, replacing the old one, and tap "Readom". Installed apps and data are kept.

An update that replaces only the system, leaving the data alone, **took about 5 minutes** on the device.

The first start after a version change compiles **the apps installed recently, before the background job got to them**, once more (WeRead took 70 seconds; what comes out is the same level as at install, and the background job compiles them to the final level when the device is idle), and leaves the ones that job has done alone; some updates -- the ones that replace the boot image -- have to compile every installed app again, and the screen before the start says so when that is the case. **A large app takes three to five minutes** (the figure on the start screen, deliberately wider than what was measured): WeRead, at 127 MB, took 159 seconds once the system had a usage profile for it, and 60 to 93 seconds before it had one. Connect a charger and do not force a restart: a restart does no harm, but the app being compiled starts over.

If that start is interrupted -- a flat battery, an impatient hand -- the next one compiles from the beginning just the same. So the notice stays up **until the work has actually finished**, however many times you tap: every Android start leaves a report on the Kindle drive, and the stock side reads it and takes the notice away only once that system has completed a start.

## Removing

Open the Kindle drive on a computer and delete the `android` folder and `documents/Readom.sh`. The Kindle's own system and your books are not touched.

To empty Android's data only (like a factory reset): delete just `android/data.img` and tap "Readom"; an empty one is made.

**If you stopped partway through the download and no longer want to install**: delete the `android/download` folder. Up to 191 MB comes back at once, and an Android already installed is unaffected. On a successful install the installer clears that folder itself, so it is normally empty.

## When something goes wrong

The screen shows an error code with an explanation in Chinese and English. It stays there until you tap the screen, which takes you back to the library; it also goes back by itself after 10 minutes, or when the power button puts the Kindle to sleep.

| Code | Meaning | What to do |
|---|---|---|
| E101 | Model or software version not supported | No file was changed. If the model matches and only the version is older, see "If the software version is older" above |
| E102 | Battery below 40 % and not charging | Charge it, then tap again |
| E103 | The previous installation is still running | Wait for it to finish |
| E104 | The Kindle is still connected to a computer | Eject the Kindle drive, unplug the cable, and tap again. A charger is fine |
| E201 | The download stopped | Check Wi‑Fi and tap again; what can be resumed is, and what cannot starts over. Or use the offline package |
| E202 | A download is needed but Wi‑Fi is not connected | Connect Wi‑Fi and tap again, or copy the offline package into `documents` |
| E203 | The download cannot be confirmed as safe | The likeliest reason is that the Kindle's date and time are wrong, so check those first in Settings; a certificate that does not verify gives the same code |
| E302 | The package was incomplete or damaged and has been deleted | Tap again to download it, or copy it again |
| E303 | There is a problem with this release | Not your fault; please report it on the project page |
| E304 | The Android data on the Kindle is from an incompatible version | Keep using the earlier version's `Readom.sh`. **If you have never installed Android before**, then `android/data.img` came from somewhere else -- delete it from a computer and tap again |
| E401 | Not enough space | The screen shows how many MB are missing; delete some books and tap again |
| E402 | Reading or writing the storage failed | Restart the Kindle and tap again |
| E403 | Out of memory | Restart the Kindle and tap again |
| E404 | The drive's filesystem is inconsistent: the place to be written is shared with another file | **Nothing was written there (the book is as it was).** Connect the Kindle to a computer, let it check and repair the drive, then tap again |
| Could not start | Android did not start; the Kindle is back on its own system | **Tap it again**: the reason it could not start is left on the drive, the next tap reads it, and anything that needs reinstalling is reinstalled (see below) |
| One line of English on the screen, `ANDROID DID NOT START. FREE SOME SPACE…` | The Kindle drive is so full that not even an empty folder could be made | Connect a computer, delete some books, and tap again. This line is the last resort, which is why it is English only: nothing that could draw a screen has run yet |

If a second try also fails, **tap it once more**: when Android cannot start it leaves the reason on the Kindle drive, and the stock side reads it on the next tap. If the system file is damaged, the screen says so ("reinstalling the damaged system file") and it is installed again, with no computer needed. Your books and app data are in a separate file (`data.img`) and are not affected.

(If that screen keeps coming back, the problem is not that file but the Kindle's storage -- back up your books and report it on the project page.)

Every tap is logged on the Kindle drive under `android/logs/<time>/`, and only the five most recent are kept. You can attach the whole folder to a report: the kernel log in it is filtered against an allow-list of the parts this project uses, so serial numbers, MAC addresses and network names are not written there.

## Privacy

- The installer only downloads from the two addresses this project's release files live at (the GitHub release and `readom.app`), and uploads nothing.
- What belongs to one Kindle alone (display waveform, Wi‑Fi calibration) is read from your own Kindle at each start and never leaves it.

## Notice

This project is not affiliated with Amazon or Google. Kindle and Paperwhite are trademarks of Amazon; Android is a trademark of Google. They are used here only to name the device it runs on and the system it is based on. Jailbreaking and installing another system may affect your warranty; decide for yourself.
