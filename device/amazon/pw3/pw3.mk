LOCAL_PATH := device/amazon/pw3
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_base.mk)
# core_base omits the font product definitions required by framework preload.
$(call inherit-product, frameworks/base/data/fonts/fonts.mk)
$(call inherit-product, external/roboto-fonts/fonts.mk)
# external/noto-fonts/fonts.mk installs all 160 Noto fonts. This build renders
# Chinese and English, so only the families fonts.xml keeps are installed (the
# file is REPLACEMENT_FONTS_FILE in BoardConfig.mk): the CJK collection, the two
# symbol subsets and Noto Serif (the "serif" family). The other 136 files are
# 8.43 MB, and each family they form is one more mmap + cmap read + coverage
# bitmap build that Typeface's static initializer does in the zygote on every
# boot.
# MINIMAL_FONT_FOOTPRINT=true is not the switch for this: its first exclusion
# block is NotoSansCJK-Regular.ttc, the one font Chinese cannot do without.
# NotoColorEmoji.ttf is not installed either: emoji/NotoEmoji-VF.ttf takes its
# place in fonts.xml, 5.30 MB smaller and legible on a 16-grey panel, which
# colour bitmaps dithered down to it are not (emoji/PW3-SOURCE.md).
PRODUCT_PACKAGES += \
    NotoSansCJK-Regular.ttc \
    NotoEmoji-VF.ttf \
    NotoSansSymbols-Regular-Subsetted.ttf \
    NotoSansSymbols-Regular-Subsetted2.ttf \
    NotoSerif-Regular.ttf NotoSerif-Bold.ttf \
    NotoSerif-Italic.ttf NotoSerif-BoldItalic.ttf
$(call inherit-product, external/google-fonts/dancing-script/fonts.mk)
$(call inherit-product, external/google-fonts/carrois-gothic-sc/fonts.mk)
$(call inherit-product, external/google-fonts/coming-soon/fonts.mk)
$(call inherit-product, external/google-fonts/cutive-mono/fonts.mk)
# external/hyphenation-patterns/patterns.mk installs 35 locales, 1.5 MB. Minikin
# hyphenates only the locale of the text being laid out, and 13 of those locales
# are written in scripts whose fonts are no longer installed -- AOSP's own
# fontchain_lint asserts a font exists for every script with a pattern file, so
# keeping them would make the build incoherent, not merely larger.
PRODUCT_PACKAGES += \
    hyph-en-us hyph-en-us.lic.txt \
    hyph-en-gb hyph-en-gb.lic.txt
$(call inherit-product, frameworks/base/data/keyboards/keyboards.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/go_defaults_512.mk)
# Unified userdebug deployment/validation policy: keep the same app Canvas
# and ART choices in the actual image, without test-harness property overrides.
# The existing Canvas hook is intentionally limited to debuggable builds.
# Android 8.1 keeps the first key after expanding inherited product markers.
# Prepend the explicit value; filtering the unexpanded list misses Go defaults.
# dalvik.vm.madvise-random: Go's 512 MB default (true) has ART advise each
# loaded dex mapping MADV_RANDOM, so a fault reads that page alone. WeRead
# cold, 32 KB block read-ahead, three launches each (Test V40.1, launched
# wrapped so the property is read): true read 20 MB less but took 70% more
# major faults and started 0.7 s later.
# pm.dexopt.shared is the filter for code another application loads, which
# cannot be compiled from a profile: that code is not public, and the other
# application could not read it. Go's value is "quicken", no compiled code at
# all, and it has to be set here: it is in build.prop, which init reads after
# the default.prop where AOSP's own "speed" is, and the later file wins for a
# property that is not read-only. A small application does not care (Settings,
# three cold starts each: 2614 ms with no compiled code against 2728 ms with
# it), a large one cannot run without it (WeRead: 129 s and "isn't responding",
# Test V32), and this value has to be the one that is safe for a large one.
# ro.logd.kernel: logd copies the kernel's ring into a buffer of its own when it
# starts (readDmesg, klogctl(KLOG_READ_ALL)), which is the only way to still have
# the boot's kernel messages an hour later -- the ring itself is 32 KB, about 274
# lines, and must stay that size so that the stock kernel can read it back after
# a watchdog reset (config/android-core.config). logd's default is "on, unless
# the device is low-RAM" and this one is, so it has to be said here. It costs one
# more logd buffer and one parsed line a minute from healthd.
PRODUCT_PROPERTY_OVERRIDES := \
    ro.logd.kernel=true \
    dalvik.vm.madvise-random=false \
    pm.dexopt.downgrade_after_inactive_days=36500 \
    pm.dexopt.shared=speed \
    dalvik.vm.heapstartsize=8m \
    dalvik.vm.heapminfree=2m \
    dalvik.vm.heapmaxfree=8m \
    dalvik.vm.foreground-heap-growth-multiplier=1.0 \
    $(PRODUCT_PROPERTY_OVERRIDES)
# Room a Java heap is given beyond what is live, i.e. how much an application
# allocates between two garbage collections. The Go defaults set none, which
# leaves ART's 2 MB, and as a low-RAM device nothing is added for the
# foreground application: Test V34, WeRead with 12 MB live collected its heap
# every 0.4 s, each collection 0.2-0.45 s on the one core it shares with the
# application ("NativeAlloc concurrent copying GC ... 11MB/13MB ... total
# 212ms", forty times in a row; the same 2 MB is the allowance for bitmap
# memory). A collection costs about 0.02 s per MB live here and a busy
# application allocates about 5 MB/s, so collecting takes a tenth of the time
# when the room is about as large as the live heap. With these values it is
# 2x the live heap, between 4 and 16 MB, for the application in use (the
# multiplier: ART adds 1 for its concurrent collector, making it 2) and half
# of that for the others. AOSP's 1 GB phone configuration allows 24 MB.
# An application is installed the way AOSP installs it, with "quicken" and no
# compiled code, and is compiled from its own usage profile once it has one.
# Two paired comparisons on WeRead 2.1.2 (13 dex, 70.4 MB of bytecode, the
# owner's own data left in place, the page cache dropped before every single
# start, nothing compiled inside the loop: every filter is put back by copying
# compiled code built beforehand). They were run at different times and so have
# their own baselines; the two cannot be put in one table.
#
#   speed against quicken, 6 rounds, the order alternating ABBA
#     speed 37.1 s, quicken 41.0 s
#     paired difference +3.9 s, SD 3.4, t = 2.77 (df 5), p = 0.04
#     95% confidence interval +0.3 to +7.5 s; one of the six rounds is negative
#     resident 206.9 MB against 183.6 MB
#
#   speed against speed-profile, 6 rounds
#     speed 34.7 s, speed-profile 36.6 s
#     paired difference +2.0 s, SD 4.0, t = 1.22 (df 5), p = 0.28
#     95% confidence interval -2.1 to +6.1 s; two of the six rounds are negative
#     resident 210.6 MB against 181.1 MB
#
#   compiling and what it leaves on disk, same application
#     speed 1214 s, 176 MB      speed-profile 159 s, 78 MB      quicken 60-93 s, 76 MB
#
# What this does and does not say. It does not say the three are equally fast:
# the interval for the profile reaches +6.1 s, and six rounds cannot rule that
# out. What it does say is that the cold start of this application is about
# 37 s whichever way it is compiled, that the most compiling can buy is a few
# seconds of it, and that it costs twenty minutes of a device nobody can use at
# the moment somebody taps install, plus 100 MB, plus 25 MB of a 511 MB machine.
# An earlier run of the first comparison said +9.9 s rather than +3.9 s. It was
# wrong by construction: it compiled "quicken" inside the loop, so every quicken
# start was measured directly after eighty seconds of dex2oat at full tilt, and
# every speed start after a file copy. Dropping the page cache does not undo
# what that leaves in zram and in free memory. Same device, same data, same
# week; only the design differs.
# The reason "speed" was set here was that WeRead with no compiled code took
# 129 s to its bookshelf and put up an "isn't responding" dialog (Test V32).
# That no longer reproduces: 41 s, no dialog. The likeliest explanation is Test
# V34, which gave the foreground application room between garbage collections
# (the heap values above) -- before it, WeRead collected its 12 MB heap every
# 0.4 s -- but that is a reading, not a measurement: nobody has run V32's
# filter against V34's heap settings on their own.
# The profile is written while the application runs and is compiled from by the
# idle job, which AOSP runs once a day while the device is idle and on a
# charger. On a Kindle that is rare, so most of the time an application stays
# at "quicken" and pm.dexopt.bg-dexopt below rarely does anything. Making that
# job reach a device off the charger was written and then withdrawn, and the
# reason is what is *not* measured: neither comparison above is quicken against
# speed-profile. Both are against "speed", taken in different sittings, so
# 3.9 - 2.0 is not a paired result and carries neither interval. What the
# withdrawal says is only that a patch to the scheduler -- with its wake-ups,
# its battery, and JobScheduler behaviour none of which was measured either --
# was not worth making for a difference nobody has measured. Measuring quicken
# against speed-profile, over a use rather than one cold start, is what would
# settle it.
# pm.dexopt.ab-ota is "speed-profile": it is what patch 0047 uses on the boot
# after a system update, when a new boot image makes every compiled file in
# /data invalid and all of it has to be made again. An application that has a
# reference profile by then is compiled from it, 159 s for WeRead; one that has
# not has its dex quickened, 60-93 s (frameworks/base patch 0051 and
# frameworks/native patch 0008). Until 2026-10-21 that second case compiled
# everything instead, 1214 s, which is twenty minutes of a device somebody has
# just updated and is waiting to use, for a start the measurements above put a
# few seconds apart.
# pm.dexopt.bg-dexopt is "speed-profile" because that job is the only thing that
# ever writes a reference profile: PackageDexOptimizer.isProfileUpdated() returns
# without calling mergeProfiles() unless the filter it is given compiles from a
# profile, and patch 0047 deliberately does not merge during boot. With "speed"
# there the current profile never becomes the reference one, every update falls
# back to compiling everything, and the numbers above are never collected.
# It does not ask installd for the fallback (0008) the way the boot does, and the
# reason written here until 2026-10-21 was the wrong one. It said the fallback
# would compile the applications the system came with in full -- 19 MB of /data,
# for applications three cold starts each say do not want it (Settings 2614 ms
# with no compiled code against 2728 ms with 8.4 MB of it, DocumentsUI 1855
# against 1937 ms) -- and that what they lose without it is the quickening. The
# first half stopped being true when the fallback became "quicken"; the second
# half was never true. A profile filter with no profile compiles no method but
# quickens the dex all the same: IsQuickeningCompilationEnabled is true for
# kSpeedProfile, and a method ShouldCompileBasedOnProfile turns down still reaches
# MarkForDexToDexCompilation (art/compiler/driver/compiler_driver.cc). These
# applications lose nothing by the fallback not being set here.
# What setting it would cost is the same work on every run of the job. dex2oat
# records the filter it was asked for and does not downgrade a profile filter when
# the profile is absent, so without the flag the odex says "speed-profile" and the
# next run of this job skips the application. With the flag it would say
# "quicken", which is never as good as the "speed-profile" this job asks for
# (IsAsGoodAs is current >= target over an enum where kQuicken precedes
# kSpeedProfile, art/runtime/compiler_filter.h and oat_file_assistant.cc), so
# every run would quicken every profile-less application again for an odex it
# already has. The boot does not have that problem: it happens once per update.
# Theirs are also the one kind of profile that can outlive the code it describes,
# since a system update replaces /system while /data keeps the profile.
# The Go default above would also take compiled code away again from an
# application left unused for ten days while storage is short; the value only
# moves that out of reach (the property cannot be unset from here, and 0 would
# mean "at once").
# 2026-10-28: bg-dexopt is "speed", not "speed-profile", and the paragraphs
# above about it writing the reference profile no longer describe what this
# product does. The reason is a measurement the owner asked for -- he said the
# device had not been this slow when he first used it -- and it is the one
# pw3.mk said above was missing: the install filter against "speed", over a use
# rather than one cold start. Same device, same account, same shelf of a dozen
# books, started by tapping the icon, A A B B A A with three minutes of settling
# after each compile: "speed-profile" four rounds at 99.8 / 88.9 / 104.8 / 89.0
# seconds, "speed" two rounds at 55.4 / 47.3, and 56 / 50 in two more rounds
# after compiling back. Wall 95.6 s against 51.4 s, process CPU 73.0 against
# 30.2, and the two groups do not overlap. Garbage collection barely moves
# (10.3-15.5 s against 8.0-11.5), so the 43 seconds are the cost of running
# bytecode in the interpreter. The six rounds above measured a cold start
# before signing in with an empty shelf: far less code, and most of what there
# is already in the profile. Loading a shelf and opening a book runs the EPub
# parser, the CSS parser and per-character typesetting, which the profile does
# not have, so they interpret from end to end. Same application, two workloads,
# opposite answers.
#
# What this gives up is the reference profile, which only a profile-guided
# filter writes. Nothing here needs it: "speed" compiles every method, and the
# app image that profile-guided compilation also brings was measured at about
# 0.4 s, inside the noise (docs/design/app-install.md). The runtime still writes
# the current profile, so the cheaper answer -- "speed-profile" against a
# profile that has actually matured -- can still be measured later; it never has
# been, because the job that would mature it has never run on this device.
#
# It does not touch what the image ships. Every application in it is compiled to
# "speed" (PRODUCT_DEX_PREOPT_DEFAULT_COMPILER_FILTER above), so IsAsGoodAs
# holds against this target too, getDexoptNeeded answers kNoDexOptNeeded, and
# nothing of the image is recompiled into /data/dalvik-cache. What changes is
# only the applications the owner installs.
#
# The cost is 99 MB of /data for WeRead (978 MB free) and about 23 MB resident,
# on a machine where low memory has killed the input method. That is why
# dalvik.vm.heapmaxfree and the foreground multiplier above are a question to
# settle together with this one, not separately.
PRODUCT_DEFAULT_PROPERTY_OVERRIDES := \
    pm.dexopt.bg-dexopt=speed \
    pm.dexopt.ab-ota=speed-profile \
    $(PRODUCT_DEFAULT_PROPERTY_OVERRIDES)
PRODUCT_NAME := pw3
PRODUCT_DEVICE := pw3
# Readom: the name users see (Kindle library entry, installer); the model is what Windows
# shows for the MTP device. Test builds keep the names the test device's apps have known
# since the first test (WeRead reports brand and model), so that a test build does not
# look like another device to an app that is logged in.
ifeq ($(TARGET_BUILD_VARIANT),user)
PRODUCT_BRAND := Readom
PRODUCT_MODEL := Kindle Paperwhite 3
else
PRODUCT_BRAND := PW3LocalPort
PRODUCT_MODEL := Kindle Paperwhite 3 development port
endif
PRODUCT_MANUFACTURER := Amazon
PRODUCT_FULL_TREBLE_OVERRIDE := false
PRODUCT_AAPT_CONFIG := normal large xlarge hdpi xhdpi
PRODUCT_AAPT_PREF_CONFIG := xhdpi
# Simplified Chinese by default, English selectable in Settings. Without this
# list every application carried the resources of all 80 AOSP languages
# (Test V32: framework-res.apk 31.9 MB, Settings.apk 32.1 MB).
PRODUCT_LOCALES := zh_CN en_US
PRODUCT_CHARACTERISTICS := tablet
# Avoid installing duplicate debug ART runtimes on the 512 MiB target.
# Host debug dexpreopt and all build tests remain enabled.
PRODUCT_ART_TARGET_INCLUDE_DEBUG_BUILD := false
# Every application in this image is compiled ahead of time, to `speed`.
#
# This is one statement rather than a list of applications, because a list is
# what made the image's compilation a suggestion. 8.1 compiles anything not
# named in PRODUCT_DEXPREOPT_SPEED_APPS to `quicken`, which leaves no compiled
# code in the image, and ART then prefers a /data/dalvik-cache copy over the
# image's odex whenever the APK's directory is read-only -- without comparing
# the two filters (art/runtime/oat_file_assistant.cc, GetBestInfo). So a
# `quicken` application is recompiled into /data by the first dexopt pass with
# a better target, and that copy keeps running even once the image ships a
# better one: raising a module to `speed` does not change its dex, so the copy
# stays valid. Measured on the device under V59 (2026-10-20): Settings,
# DocumentsUI and PackageInstaller were all raised to `speed` and all three
# still ran V58's `speed-profile` copies out of /data -- 21 of the image's 31
# applications were running from /data, 14 MB of it, and the three that
# mattered are the settings, files and install-an-APK screens.
#
# With `speed` as the default, CompilerFilterIsOkay's IsAsGoodAs holds against
# every target this product sets (install and shared `speed`, bg-dexopt and
# ab-ota `speed-profile`, boot `verify`, first-boot `quicken`), getDexoptNeeded
# answers kNoDexOptNeeded, and nothing is ever written to /data/dalvik-cache
# for an application of this image. What the build decided is what runs, no
# application added later is quietly left out, and the device stops spending
# its one core recompiling what was already compiled. The knob is the one later
# AOSP adds for it (patches/aosp-build-make/0003).
#
# What it costs, measured by building it both ways rather than estimated
# (2026-10-20): every odex of the image read back with `oatdump --header-only`
# says `speed` (56 of them; the four system-server jars say `speed-profile`,
# which is go_defaults_common.mk's and a separate question), and the image went
# from 487,909,260 bytes with nine applications named to 493,832,076 with all
# of them: 5.9 MB, in a partition of 603,979,776 that is then still 105 MB
# empty. Against the `quicken` default the odex grows by 63.6 MB and the image
# by 21.5 MB, because a `speed` odex holds compiled code where a `quicken` vdex
# held quickened dex -- the odex total is 70.4 MB and the vdex 75.1 MB, which is
# 2.4 MB less than the same build at `quicken`.
#
# The 45 boot image files of /system/framework/arm are byte-identical with
# V59's, so nothing compiled into /data by an earlier release is invalidated by
# this change.
PRODUCT_DEX_PREOPT_DEFAULT_COMPILER_FILTER := speed
# Code that system_server loads, as core.mk lists it for the packages shipped
# here (core_base, inherited above, lists SettingsProvider and WallpaperBackup).
# This says what loads it, not how it is compiled -- the default above covers
# that now -- and it still has to be said: system_server may not map code from
# /data/dalvik-cache at all, so an application of this list that ever gained a
# copy there would be refused by SELinux on every load, which is what happened
# to FusedLocation while it was compiled to quicken (Test V47). It is the same
# root cause the default above closes, met from the other side.
PRODUCT_SYSTEM_SERVER_APPS += FusedLocation InputDevices KeyChain
# Release builds (I8, docs/design/release-build.md) are signed with the project's own keys
# (releasekey, platform, shared, media), which never enter this repository:
# tools/build-aosp-system.sh passes their directory in PW3_RELEASE_KEY_DIR. AOSP's test keys
# are public, so anyone could sign an app with the platform key and get system rights; a user
# build is refused without the release keys.
ifneq ($(PW3_RELEASE_KEY_DIR),)
PRODUCT_DEFAULT_DEV_CERTIFICATE := $(PW3_RELEASE_KEY_DIR)/releasekey
else ifeq ($(TARGET_BUILD_VARIANT),user)
$(error pw3-user is a release build: set PW3_RELEASE_KEY_DIR to the release keys)
endif
DEVICE_PACKAGE_OVERLAYS += $(LOCAL_PATH)/overlay
# One input method for Chinese and English: PinyinIME, reworked for the panel
# (apps/PinyinIME/PW3-SOURCE.md). No LatinIME: the user wants neither English
# prediction nor correction (2026-10-14), and its 17 MB were a second keyboard.
# core_base does not include the one-time AOSP setup completion activity.
# Without Provision, user_setup_complete stays 0 and policy blocks HOME.
PRODUCT_PACKAGES += \
    Provision Settings SystemUI Launcher3Go DocumentsUI PackageInstaller \
    EinkBro PinyinIME SystemWebView pw3-sleep-picture-zh pw3-sleep-picture-en pw3-sleep-glyphs \
    pw3-sleepwall-01 pw3-sleepwall-02 pw3-sleepwall-03 \
    ExternalStorageProvider DownloadProviderUi KeyChain CertInstaller \
    CaptivePortalLogin InputDevices FusedLocation ProxyHandler \
    libEGL_swiftshader libGLESv1_CM_swiftshader libGLESv2_swiftshader \
    libRS librs_jni \
    gralloc.pw3 hwcomposer.pw3 lights.pw3 power.pw3 \
    android.hardware.graphics.allocator@2.0-service \
    android.hardware.graphics.allocator@2.0-impl \
    android.hardware.graphics.mapper@2.0-impl \
    android.hardware.light@2.0-service android.hardware.light@2.0-impl \
    android.hardware.power@1.0-service android.hardware.power@1.0-impl \
    android.hardware.health@1.0-service android.hardware.health@1.0-impl
# Keep Recents titles/icons while avoiding task image rendering in system_server.
# The userdebug package Canvas policy is selected explicitly above.
# dex2oat gives up after 570 s. On this CPU a large application needs longer
# (Test V32: WeRead, "dex2oat did not finish after 570 seconds") and would
# then stay interpreted for good; 61% of its start-up CPU time was the
# interpreter. Hence --no-watch-dog (system.prop: the value has a space).
# ro.hw_timeout_multiplier (frameworks/base patch 0012): the deadlines for
# "isn't responding" are 10 s for a broadcast and 5 s for a touch. A current
# application starting cold on this CPU keeps its main thread busy for longer
# than that without being hung: WeRead, fully compiled, needs 25-30 s to an
# idle bookshelf (Test V34), and whenever that crossed a full minute the clock
# broadcast ran into the 10 s and the dialog came up. 4 puts the broadcast
# deadline (40 s) beyond the whole start; a touch is given 20 s.
# No GPU: application windows use the software Canvas renderer instead of hwui
# on CPU-emulated GLES (frameworks/base patch 0006). debug.pw3.software_canvas
# overrides this at run time for newly started processes.
# ro.sf.disable_triple_buffer: Android 8.1's default "1" is read inverted
# (frameworks/native Layer.cpp: mProducer->setMaxDequeuedBufferCount(2) when
# set; BufferQueueCore's own default is one dequeued and one acquired buffer),
# so every window had three buffers; "0" gives each two. A full-screen buffer
# here is 1072x1448 RGBA, 6.2 MB. The third buffer only lets an application
# draw a frame further ahead of what the panel shows.
# ro.pw3.prefetch_recent_app: once boot has completed, the compiled code of the
# app used last is read into the page cache (frameworks/base 0025): the first
# start of WeRead after boot takes 13-16 s instead of 18-22 s.
# ro.logd.size: as a low-RAM device each log buffer would hold 64 KB, which
# boot alone overruns within minutes (Test V40), so a problem is gone from the
# log before anyone can look. 256 KB is Android's size for other devices; the
# buffers that fill (main, system, events, crash) then take about 1 MB more.
# ro.pw3.disable_display_transforms: the composer works on the CPU and neither
# scales nor applies a colour matrix, so magnification and colour inversion or
# correction would make SurfaceFlinger compose every frame with CPU-emulated
# GLES: more than 4 s of its CPU per frame magnified (Test V40.1). Settings does
# not offer them (Settings 0006).
# ro.pw3.hold_screen_past_deadline: true, and written here so that the list
# above says so. The window manager's default is the same (frameworks/base
# patches 0069-0073: when a transition's deadline passes with nothing drawn,
# the previous screen is kept until the new one draws); it was only ever in the
# Java until 2026-11-04 (open-items, "two small changes"), which is why a reader
# of this file could not see the feature was on. debug.pw3.hold_screen_past_deadline
# overrides it at run time.
# ro.pw3.disable_starting_window: false since 2026-11-03, true before that. The
# starting window costs another window -- 6 MB of fresh pages and a full-screen
# update of 134 ms or more -- and what it buys is that the reader sees what they
# tapped is starting: the system draws the target activity's own windowBackground
# from the package's resources, in a fraction of a second, without waiting for
# its process. For the application this device is for that background is a white
# page with the application's own black emblem on it (launch_style ->
# @drawable/launch_bg), which is also what e-paper draws best, and the
# application's own first frame is twenty to thirty seconds away. With this true
# the reader taps and nothing changes for that whole time (frameworks/base 0067
# and 0068; open-items row 16). debug.pw3.disable_starting_window overrides this
# on the device, so both ways can be looked at without a build.
PRODUCT_PROPERTY_OVERRIDES += \
    ro.sf.disable_triple_buffer=0 \
    ro.hardware.gralloc=pw3 \
    ro.hardware.hwcomposer=pw3 \
    ro.hardware.lights=pw3 \
    ro.hardware.power=pw3 \
    ro.hardware.egl=swiftshader \
    ro.sf.lcd_density=300 \
    ro.opengles.version=131072 \
    ro.lmk.use_inkernel_interface=true \
    ro.pw3.disable_task_previews=true \
    ro.pw3.prefetch_recent_app=true \
    ro.pw3.software_canvas=true \
    ro.rescue_party.no_wipe=true \
    ro.pw3.disable_starting_window=false \
    ro.pw3.hold_screen_past_deadline=true \
    ro.pw3.disable_display_transforms=true \
    ro.hw_timeout_multiplier=4 \
    ro.logd.size=256K \
    persist.sys.timezone=Asia/Shanghai \
    debug.sf.no_hw_vsync=0 \
    debug.sf.nobootanimation=1
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.touchscreen.xml:system/etc/permissions/android.hardware.touchscreen.xml \
    $(LOCAL_PATH)/input/max77696-onkey.kl:system/usr/keylayout/max77696-onkey.kl \
    $(LOCAL_PATH)/input/cyttsp4_mt.idc:system/usr/idc/cyttsp4_mt.idc \
    $(LOCAL_PATH)/input/fsr_keypad.kl:system/vendor/usr/keylayout/fsr_keypad.kl \
    $(LOCAL_PATH)/input/haptics/haptics0.config:system/vendor/usr/haptics/haptics0.config \
    $(LOCAL_PATH)/input/haptics/haptics1.config:system/vendor/usr/haptics/haptics1.config

# First validation image uses bounded RAM data and read-only eMMC.
$(call inherit-product, $(LOCAL_PATH)/initialization.mk)

$(call inherit-product, $(LOCAL_PATH)/wifi/wifi.mk)
PRODUCT_PACKAGES += pw3-fg-state
# Installs WeRead from Tencent's official address on the first network connection, once
# (apps/WeReadInstaller, docs/design/app-install.md): the release carries no WeRead.
PRODUCT_PACKAGES += WeReadInstaller
# Shows the browser under the name of what it is, without touching the signed
# APK (apps/BrowserName).
PRODUCT_PACKAGES += PW3BrowserName

# Separate static entry; the original AOSP /init is preserved.
PRODUCT_PACKAGES += pw3-testfile-init

# Reads the system image in the background on the starts that trusted their
# record of the last complete check instead of reading it
# (preinit/SystemRecheck.h).
PRODUCT_PACKAGES += pw3-system-verify

# Expose the AOSP software Keymaster through the real HIDL service.
# core_minimal already selects keystore, gatekeeperd and keystore.default.
PRODUCT_PACKAGES += \
    android.hardware.keymaster@3.0-impl \
    android.hardware.keymaster@3.0-service

# Binary fs_config overrides enforce private firmware modes in the image.
PRODUCT_PACKAGES += fs_config_dirs fs_config_files

# Android's AudioService requires the audio and effects factories even on a
# reader with no speaker or microphone. AOSP's default HAL discards PCM output.
# Do not advertise audio output, microphone, Bluetooth, or USB audio features.
PRODUCT_PACKAGES += \
    audio.primary.default \
    android.hardware.audio@2.0-service \
    android.hardware.audio@2.0-impl \
    android.hardware.audio.effect@2.0-impl
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/audio/audio_policy_configuration.xml:system/vendor/etc/audio_policy_configuration.xml \
    $(LOCAL_PATH)/audio/audio_effects.xml:system/vendor/etc/audio_effects.xml \
    frameworks/av/services/audiopolicy/config/audio_policy_volumes.xml:system/vendor/etc/audio_policy_volumes.xml \
    frameworks/av/services/audiopolicy/config/default_volume_tables.xml:system/vendor/etc/default_volume_tables.xml

# Supplement the standard extractor filter for the pre-vDSO ARM kernel.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/seccomp/mediaextractor.policy:system/vendor/etc/seccomp_policy/mediaextractor.policy

# Rebuilt public kernel modules; private firmware is injected only at install.
PW3_USB_COMPONENT_ROOT := vendor/amazon/pw3-hardware
$(call inherit-product, $(PW3_USB_COMPONENT_ROOT)/pw3-usb-component.mk)
