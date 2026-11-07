<h1 align="center">
  <a href="https://readom.app">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="docs/img/wordmark-reverse.svg">
      <img src="docs/img/wordmark.svg" alt="Readom" width="280">
    </picture>
  </a>
</h1>

<p align="center"><b>Wake your Kindle.</b><br>Put Android on your Kindle, and a whole new world of reading lights up.</p>

<br>

<!-- The links go to /releases, not /releases/latest: GitHub's latest means the
     latest full release, so while a version is out as a pre-release it still points
     at the full release before it (and 404s when there is none). The badge asks for
     include_prereleases, so it counts pre-releases too. -->
<p align="center">
  <a href="https://github.com/hbyxabc/readom/releases"><img src="https://img.shields.io/github/v/release/hbyxabc/readom?include_prereleases" alt="Latest release"></a>
  <a href="https://github.com/hbyxabc/readom/actions/workflows/tests.yml"><img src="https://github.com/hbyxabc/readom/actions/workflows/tests.yml/badge.svg" alt="Tests"></a>
  <a href="#licence"><img src="https://img.shields.io/badge/licence-Apache--2.0%20%2F%20GPL--2.0-blue" alt="Licence: Apache-2.0 / GPL-2.0"></a>
</p>

<p align="center">
  <a href="https://readom.app/#pick">Download from the website</a> ·
  <a href="https://readom.app/#guide">Install guide</a> ·
  <a href="https://github.com/hbyxabc/readom/releases">GitHub Releases</a> ·
  <a href="README.md">中文</a>
</p>

<br>

<p align="center"><a href="https://readom.app"><img src="docs/img/hero.jpg" alt="Readom's home screen on a Kindle Paperwhite 3" width="340"></a></p>

<br>

<p align="center">The demo, the download and an illustrated install guide are on the website: <b><a href="https://readom.app">readom.app</a></b><br>For now it runs on the Kindle Paperwhite 3 (7th generation) only, and the Kindle's own system stays.</p>

<br>

## Latest progress

<!-- Update with every release: the version, the release date, what it brought. On release day, replace "not yet released" with the date. -->

| Release | Date | What it brought |
|---|---|---|
| v0.1.0 | 2026‑11‑04 | The first public release: Android 8.1 on the Kindle Paperwhite 3, with WeRead in one tap, the EinkBro browser, the pinyin keyboard and the sleep screen, installed and upgraded on the Kindle itself |

What each release changed, and how far it was verified on the device, is in full in [CHANGELOG.en.md](CHANGELOG.en.md); the website's [changelog](https://readom.app/#log) and [GitHub Releases](https://github.com/hbyxabc/readom/releases) are written from it.

## What Readom is

<img src="docs/img/why.webp" alt="The unicorn blows the dust off an old e-reader in a drawer, and its screen lights up" width="100%">

Readom is an Android system made for the Kindle. It brings Android 8.1 (AOSP) to the Kindle Paperwhite 3, with the refresh, the interface, the keyboard and the way it installs all redone for the e-ink screen and this device's hardware. With it, a Kindle browses the web, installs Android apps and reads in the official e-ink edition of WeRead; the Kindle's own system is still there, a switch away.

The name is Read and Freedom put together: what you read, which app you read it in, and on which device, is up to you.

It started because I wanted to read WeRead, legitimately, on my Kindle, without importing books one by one and without pirated ones. After jailbreaking I tried KOReader with a WeRead plugin, and it was miserable; Tencent makes an e-ink edition, but it needs Android 7.0 or later, and every Android for the Kindle was too old.

So one person, with a few weeks' worth of tokens, brought a full Android system up on a Kindle. I believe that in the age of AI, old devices gathering dust can be useful again, and Readom starts with this Kindle.

## What Readom does

<!-- These three were taken on the device in one run on 2026-11-02, in the same batch as the
     website's ten (the raw PNGs live in readom-site, _lab/shots/reshoot-2026-10-28/). All three
     were converted to the 16 grey levels the panel really shows, with Luma.h and Tone.cpp from
     device/amazon/pw3/epdc/ (readom-site/website/tools/panelise.py), and the two icons in the
     top-left of the status bar that only appear over a cable (download, adb) were blanked.
     To reshoot, follow the shot list on the website side. -->

<p align="center">
  <img src="docs/img/shot-home.webp" width="32%" alt="The Readom home screen: a search box and five icons -- WeRead, Browser, Files, Settings, All apps">
  <img src="docs/img/shot-browser.webp" width="32%" alt="WeRead's web home page open in the browser">
  <img src="docs/img/shot-reader.webp" width="32%" alt="A page of the Analects, chapter Wei Zheng, in WeRead">
</p>

<sub>The WeRead interface and content in these screenshots belong to Tencent and are shown only to demonstrate how it runs on Readom.</sub>

### Features

- **WeRead in one tap**<br>
  once Wi-Fi is on, the system downloads the e-ink edition from Tencent's own address, checks Tencent's signature, and installs it, with no computer involved.

- **A newer way to browse the web**<br>
  EinkBro, an open-source browser made for e-ink, comes installed, and the web engine went from the Chromium 61 that Android 8.1 ships (2017) to Chromium 128 (2024). AOSP later published official builds that still support Android 8, up to Chromium 138 (2025); this release stays on 128.

- **Better first guesses in pinyin**<br>
  rime-ice took the dictionary from 65,000 entries to more than 140,000, and on the same set of 110 test inputs the first candidate went from right 84 times to right 97 times; the layout is redrawn for e-ink.

- **Made for the Kindle's own hardware**<br>
  front light, power key, battery, Wi-Fi and USB file transfer all work; close a magnetic cover and it sleeps, open it and it wakes.

- **Back to the Kindle any time**<br>
  hold the power key and choose "Restart to Kindle"; if the system keeps failing, it goes back to the Kindle on its own, without wiping your data.

- **Sleep screen**<br>
  when the screen turns off, it shows a unicorn reading. It is there first because it is nice to look at, so you'd want to pick the Kindle up every time; it also covers what you were reading, since an e-ink screen keeps its last image with the power off. The theme comes from a line I love: "Reading knows no forbidden zones".

<p align="center"><img src="docs/img/sleep-en.webp" width="300" alt="Sleep screen: a unicorn reading on top of the globe, with books flying around it, under the line &quot;Reading knows no forbidden zones&quot;"></p>

### Performance

- **Apps install over ten times faster**<br>
  apps are compiled differently, and installing a large one like WeRead went from more than twenty minutes to about a minute and a half.

- **Slimmed for 512 MB of memory**<br>
  it starts from AOSP's smallest product configuration with Android Go's low-memory mode on, carries no phone or camera apps, and drops the settings for hardware the device does not have; only Chinese and English fonts are kept, from 173 files and 43 MB down to 37 files and 33 MB, so the font families loaded at every start went from 116 to 15.

- **Upgrades in five minutes**<br>
  an upgrade replaces only the system image, and installed apps and data stay as they are; the first install takes about 15 minutes, each upgrade after it about 5.

### Interface

- **Redrawn for e-ink**<br>
  thin black lines instead of shadows and gradients, no dimming behind dialogs, no animations at all.

- **Pages turn without a black flash**<br>
  pages turn with a refresh that does not flash, and the whole screen is cleared of ghosting only every 20 pages.

- **Whiter, cleaner pages**<br>
  grey levels were set again from reading research and measurements of this panel, and on 11 everyday pages pure white now covers 92% to 100%.

- **What you use is close at hand**<br>
  one home screen holds it, swipe up for all apps, the search box searches the web; one pull down and the front light slider is there.

## Installing Readom

| Requirement | Where to check |
|---|---|
| Kindle Paperwhite 3 (7th generation, 2015) | Settings → Device Options → Device Info |
| Firmware 5.16.2.1.1 | Same place. On an older version, update first as the install guide explains |
| Jailbroken, with a library that can show `.sh` scriptlets | Follow the guide at [kindlemodding.org](https://kindlemodding.org/jailbreaking/); on 5.16.2.1.1, WinterBreak2 first. If it is jailbroken but the library cannot show or run `.sh` items, how to confirm that and where to turn are in the [install guide](docs/install.en.md#does-the-jailbreak-include-sh_integration). Jailbreaking is not part of this project; the risk is yours to judge |
| 2.6 GB free, battery above 40% | Connect it to a computer and look at the Kindle drive |

On the wrong model or firmware, the installer shows E101 and stops without changing any file.

1. Download `Readom.sh` (about 8 MB): the website, [readom.app](https://readom.app/#pick), and [GitHub Releases](https://github.com/hbyxabc/readom/releases) have the same file.

2. Connect the Kindle with a cable, copy the file into the `documents` folder on the Kindle drive, eject, unplug.

3. Tap "Readom" in the Kindle library.

4. Checking, downloading, verifying and writing all happen on the Kindle. A first install takes about 15 minutes, then Android starts on its own.

The installer carries both download addresses, the website's and GitHub's, and tries them in turn. If neither delivers, download the offline package on the computer as well (both places have it) and copy it in with `Readom.sh`.

The website has an illustrated install guide, and a way to have an AI walk you through it: [readom.app/#guide](https://readom.app/#guide). Upgrades, the offline package and error codes are in the [install guide](docs/install.en.md); if "Readom" does not appear in the library after copying, or KUAL stopped working after the install, look there first too: [If Readom is not in the library](docs/install.en.md#if-readom-is-not-in-the-library) and [If KUAL stopped working after the install](docs/install.en.md#if-kual-stopped-working-after-the-install).

### Back to the Kindle, and uninstalling

- **Back to the Kindle**<br>
  hold the power key and choose "Restart to Kindle". To go back to Android, tap "Readom" in the library.

- **If it is stuck**<br>
  hold the power key down until the screen changes (at most about 40 seconds) and you are back on the Kindle, with nothing lost.

- **Uninstall**<br>
  delete the `android` folder and `documents/Readom.sh` from the Kindle drive. The Kindle's system and your books are untouched.

## Building and porting

### How it is built

Readom does not flash anything. The whole system is made of these pieces:

- **Started from the stock system**<br>
  after jailbreaking, the library has a "Readom" entry. Tapping it has the stock kernel load our handover module, which puts the Readom kernel and its boot parameters in memory and jumps to it. No partition is written and the bootloader is untouched, so the stock system is always one restart away.

- **The kernel**<br>
  Amazon's published Kindle GPL source (Linux 3.0.35) with patches that add what Android 8.1 needs -- binder, ashmem, SELinux and more -- and fix the USB, power and display drivers.

- **The system is two image files**<br>
  `system.img` and `data.img` sit on the Kindle drive, and Readom's own start program finds, checks and mounts them at boot.

- **Android 8.1 (AOSP)**<br>
  one device directory, `device/amazon/pw3/`, and a patch series for each AOSP project it changes.

- **The e-ink display**<br>
  there is no GPU, so the interface is composed on the CPU; our own display code decides which area to refresh, with which waveform, and how 16 grey levels map, and drives the i.MX EPDC display controller directly.

- **No device-specific data in the package**<br>
  the panel waveforms, Wi-Fi calibration, MAC and everything else that differs from one device to the next are read from this device's stock system at every start.

- **Installed on the Kindle**<br>
  the installer checks the model and the space, downloads, verifies against SHA-256 and writes the images, and carries on after a power loss.

The build environment and steps are in [CONTRIBUTING.en.md](CONTRIBUTING.en.md), and the design record of each part in [docs/README.en.md](docs/README.en.md).

### Porting to other Kindles

<img src="docs/img/porting.webp" alt="The unicorn lights up a row of e-readers from different years with its horn; the ones further along are still dusty" width="100%">

Readom runs on the PW3 only for now, but most of it does not depend on the model: about two in three kernel patches never touch hardware code, most of the AOSP changes carry over as they are, and the boot handover takes one configuration per model and firmware. Moving to another Kindle means redoing the kernel patches that touch hardware, one boot configuration and one device directory, then measuring everything again on the real device.

| Model | How far from the PW3 |
|---|---|
| Voyage | Closest: Amazon builds it with the same kernel configuration as the PW3, and the kernel already builds |
| Oasis 1 | The kernel already builds; the Wi-Fi driver is missing |
| Kindle 8, PW4 and others | A different kernel, but the display side can be reused |
| PW5, Kindle 11th generation, Scribe and other MediaTek models | Display, Wi-Fi and power all have to be rewritten |

What exactly it takes is in [porting to other models](docs/design/porting-to-other-models.en.md).

## Licence

- This project's code and documentation: Apache-2.0; the full text is at the root, in [LICENSE](LICENSE).

- The kernel, its patches, and the whole handover boot chain under `boot/` together with the kernel configuration (`config/android-core.config`): GPL-2.0-only. That is the overview; a few files under `boot/` say otherwise (`boot/handoff/sha256.c`, for one, is public domain).

- File by file, what governs is that file's own SPDX tag and `REUSE.toml` (this repository follows [REUSE](https://reuse.software/); the licence texts are in `LICENSES/`).

- Bundled third-party components (EinkBro, Chromium WebView, the pinyin dictionary and others) keep their own licences, listed in [THIRD_PARTY.en.md](THIRD_PARTY.en.md).

- Neither the repository nor a release contains Amazon's private files (panel waveforms, Wi-Fi calibration, stock private modules). They are read and used only on your own device.

<!-- At release: the SHA-256 fingerprint of the release certificate goes here, for readers to check against. -->

The Readom name and logo belong to this project. Readom is an unofficial project, not affiliated with Amazon, Google or Tencent.

### Thanks

Readom stands on the work of these projects and communities:

- **The system**: [AOSP](https://source.android.com/), Amazon's published Kindle GPL source

- **Browsing and typing**: [EinkBro](https://github.com/plateaukao/einkbro), [Chromium](https://www.chromium.org/), [rime-ice](https://github.com/iDvel/rime-ice) (the pinyin dictionary)

- **Icons and fonts**: [Material Symbols](https://github.com/google/material-design-icons) (the home screen icons), [Noto Emoji](https://github.com/googlefonts/noto-emoji), [LXGW WenKai](https://github.com/lxgw/LxgwWenKai) (the title on the sleep screen)

- **The community**: the [Contributor Covenant](https://www.contributor-covenant.org/), [KindleModding](https://kindlemodding.org/) and the whole Kindle jailbreak community

## Wake up the Kindles gathering dust

<img src="docs/img/star.webp" alt="The unicorn pushes a golden star into a Star button" width="100%">

If Readom made your Kindle useful again, please give it a Star, and tell a friend who has a Kindle.

If you want to join in, here is where to start:

- **Something went wrong while using it**<br>
  check the [known issues](docs/known-issues.en.md) first; if it is not there, open an [Issue](https://github.com/hbyxabc/readom/issues) and include the line from "Settings > About tablet > Build number".

- **An idea or a question**<br>
  bring it to [Discussions](https://github.com/hbyxabc/readom/discussions).

- **You want to change the code**<br>
  pick something from the [Issues](https://github.com/hbyxabc/readom/issues) -- the ones marked `good first issue` are a good start -- read [CONTRIBUTING.en.md](CONTRIBUTING.en.md), and send a Pull Request.

- **You have a different Kindle**<br>
  fork this repository, port it to your device following the [porting notes](docs/design/porting-to-other-models.en.md) and release your own build; once it is stable, you are welcome to bring it back here so more people can use it.

- **Security issues**<br>
  see [SECURITY.en.md](SECURITY.en.md).

Talking and contributing here means following the [code of conduct](CODE_OF_CONDUCT.en.md).
