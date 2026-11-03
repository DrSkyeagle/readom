# Documents

Ordered by "who you are and what you want to know". Every document this index
lists exists in both languages (`*.en.md`), with the two halves pinned together
by `tools/tests/test_doc_pairs.py`.

## I want to put it on a Kindle

| Document | What it answers |
|---|---|
| `../README.en.md` | What this is, which models it supports, how far it works today |
| `install.en.md` | How to install it, what to have ready first, where to look when something fails |
| `known-issues.en.md` | Problems that are known, looked into and **deliberately not fixed**, and log noise checked and found harmless |

## I want to know what is still missing

| Document | What it answers |
|---|---|
| `open-items.en.md` | What is not done: the known fault at the top, fixes waiting for a device, verifications not yet run, and things designed but not built, each with what it is blocked on |
| `../CHANGELOG.en.md` | What each release changed and how far it was verified on the device; the website's changelog and the GitHub Release notes are written from it |

## I want to change the code

| Document | What it answers |
|---|---|
| `../CONTRIBUTING.en.md` | Build environment, tests, the append-only rule of the patch series, the two rules about framework resources, commit conventions |
| `../SECURITY.en.md` | The guarantee that no partition is written, per-device data, the privacy check, signing keys |
| `../THIRD_PARTY.en.md` | Whose code is in here, under what licence, and whether it was changed |
| `../CODE_OF_CONDUCT.en.md` | How to take part in this project, and how to report a problem |
| `design/README.en.md` | Index of the design records |

**Which directory to land in**: five of them carry a README saying what that part is, where to
start reading, and the two things easy to get wrong — `../boot/` (the boot chain), `../device/`
(the device tree), `../installer/` (the on-device installer), `../patches/` (the AOSP and kernel
patches), `../tools/` (build and check scripts). Those five are in Chinese only. `../config/` has
none: each of its five files says in its own opening lines what it is and who reads it, and an
index on top of that would be one layer too many.

## Design records (`design/`)

| Document | What it answers |
|---|---|
| `design/eink-ui.en.md` | The e-paper interface rules: which screens changed, why, and where each rule is implemented |
| `design/performance.en.md` | **Where this machine's time goes**: the boot broken down, the memory account, and the things that measurement showed were not worth doing |
| `design/usb-mtp.en.md` | The composite USB gadget, MTP, adb over FunctionFS, and cable detection |
| `design/app-install.en.md` | Installing and removing WeRead, and how the home screen's cell is handed over |
| `design/diagnostics.en.md` | The release's own diagnostic channel: the report area, the log folders, how the stock side reads them |
| `design/release-build.en.md` | The fourteen release steps, the `user` build, keeping and rotating the signing keys, the security review |
| `design/porting-to-other-models.en.md` | Model-by-model research and an inventory of the work, grouped by stock kernel version rather than by SoC |
