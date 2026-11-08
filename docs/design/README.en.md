# Design records

Each one answers a question, and they are divided by who arrives with which question. Somebody installing the system needs only `docs/install.en.md`; these are for somebody changing it.

| Document | What it answers | When to read it |
|---|---|---|
| [eink-ui.en.md](eink-ui.en.md) | The rules this screen's interface is drawn by | Before changing anything that appears on the screen. More code comments point here than anywhere else, because the numbers and the reasons for them are both here |
| [performance.en.md](performance.en.md) | Where this machine's time and memory go, and how much of each | Before trying to make something faster. Every number in it was measured on a PW3 running this system, not estimated, including the several that measurement showed were not worth doing |
| [app-install.en.md](app-install.en.md) | What happens when a large application is installed on this machine, and how WeChat Reading installs itself | Before changing the install path or the WeRead flow |
| [usb-mtp.en.md](usb-mtp.en.md) | How file transfer over USB is done on the Android side, and what was walked into on the way | Before touching USB, adb or MTP |
| [diagnostics.en.md](diagnostics.en.md) | How the device gets itself out when a start fails, and how the evidence reaches a person | Before changing the start checks, the report area or the log export |
| [release-build.en.md](release-build.en.md) | How to build a release and what has to be true before one (the release checklist, the keys, the security review, the data that expires) | Before a release, line by line |
| [porting-to-other-models.en.md](porting-to-other-models.en.md) | What it takes to move this to another Kindle, and what is already there | When considering a second model |
| [voyage-port.en.md](voyage-port.en.md) | What the Voyage, the second model, changed, what it shares with the PW3, and what is still missing | Before touching the Voyage device directory, boot profile or build wiring |

**Why eight**: they are for different readers -- somebody changing the interface, somebody making a release, somebody porting. More than forty "see design/..." comments in the code each point at a particular one, so whoever follows a comment lands on the content it means. The other way round, the two halves of one thing share a document -- WeRead's automatic install and "installing a large application here", the way out of a failed start and the diagnostic channel, the security review and the release checklist -- so the reader does not have to put them together.

**When renaming or merging them**: every reference to these file names in the repository, including the code comments under `device/`, has to change with them, and `tools/tests/test_doc_links.py` points out the ones that were missed.
