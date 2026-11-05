#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The installer's screens (installer/README.md, "安装画面"), in the build VM: each
screen's text in Chinese and English, laid out and rasterised here into 8-bit
grey bitmaps, so the Kindle needs no fonts. On the device the helper puts a
screen together (text, progress bar, number) and shows it with the stock eips
(installer/install/screen.c).

Outputs:
  screens.bin         every bitmap, zstd-compressed, one after another
  screens.h           the layout constants and screen names (enum pw3_screen)
  screens-data.h      the bitmaps' index, for screen.c
  preview/*.png       each screen as the Kindle would show it (with sample numbers)
  build-result.json   fonts and outputs with their SHA256

Usage: render_screens.py --manifest PAYLOAD/manifest.json --cjk-font NotoSansCJK-Regular.ttc
                         --latin-font Roboto-Regular.ttf --latin-title-font Roboto-Medium.ttf --output DIR
"""
import argparse, hashlib, json, os, platform, re, subprocess, sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# PW3: 1072x1448, 300 ppi, density 1.875 (docs/design/eink-ui.md). The screens
# fill a region below the stock status bar, the same region every time, so a
# shorter screen leaves nothing of a longer one.
SCREEN_W, SCREEN_H = 1072, 1448
REGION_X, REGION_Y, REGION_W = 64, 176, 944
# Sizes in px (sp = px / 1.875): titles 28 sp, text 20 sp (Chinese) and 17 sp
# (English), all above the 16 sp floor for reading at 35-40 cm (eink-ui.md rule 8).
TITLE_ZH, TITLE_EN, BODY_ZH, BODY_EN, NUMBER = 52, 40, 38, 32, 48
LINE = 1.45                   # line height, as a multiple of the size
GAP_LANG, GAP_PARA = 14, 36   # between the Chinese and English of one paragraph; between paragraphs
BAR_H, BAR_LINE, BAR_GAP = 32, 2, 56   # progress bar: height, outline (2 px, rule 3), space above it
NUMBER_GAP = 20               # between the bar (or the text) and the number

NO_LINE_START = set('，。；：、！？）」”》,.;:!?)%')
NO_LINE_END = set('（「“《(')


# Where a screen tells the reader to report something, it also tells them what
# to attach. The diagnostic "package" is the log folder the launcher already
# writes on every tap (installer/README.md, "诊断：日志目录就是诊断包"): it is at a fixed place on the
# Kindle drive, the tap that went wrong is in it (five are kept), and it needs
# no entry of its own, no second tap and no choice to get wrong. Not packed into
# an archive either -- a folder is what the person already has in front of them
# on a computer, and every computer can zip one.
DIAGNOSTICS_ZH = '报告时请把 Kindle 盘上 android/logs 文件夹整个发来。'
DIAGNOSTICS_EN = 'When you report it, send the whole android/logs folder from the Kindle drive.'
# The last sentence of every screen that ends a run; see screens(), errors.
TAP_BACK_ZH = '看完点一下屏幕，回到书库。'
TAP_BACK_EN = 'Tap the screen to go back to the library.'


# The way back when a start does not finish. A restart always returns to the
# Kindle's own system, because nothing starts Android but a tap in the library.
# Four things do the restarting: the storage check and the interface check each
# ask for one when they fail (initcheck/pw3-boot-report.rc), the entry's own
# stop() reboots after STOP_SECONDS (preinit/PreInit.c), and RescueParty takes
# the device through a restart rather than a wipe after five crashes (patch
# 0045). The screen says so while the start is running, because a failed start
# cannot be told anything afterwards (docs/design/diagnostics.md).
#
# "Usually", not "always": a freeze that crashes nothing and fails no check is
# not covered by any of the four, and RescueParty counts crashes. The sentence
# after this one in STUCK_* is what that case gets.
#
# "Books", not "books and data": the books are the Kindle's own, on its own
# store, and nothing here touches them. Android's settings are a different
# matter -- RescueParty's levels below the last one still reset them, which
# patch 0045 leaves alone on purpose, so promising the reader that no data
# changes is a promise this tree does not keep (a Codex review, 2026-10-30).
RESTART_ZH = 'Kindle 通常会在几分钟内自己重启回原来的系统，书不会丢。'
RESTART_EN = ('the Kindle usually restarts itself back to its own system within a few minutes, '
              'and your books are not touched.')
# What the reader is told about a start that is not working, on both screens
# that have to say it. It says what happens when a start fails, and counts
# nothing: "most failures" was a proportion this project has never measured,
# and the sentence does not need one to be useful.
#
# It is also narrower than what the website says about the same situation, and
# that is deliberate: do not "align" them by widening this one. The site says
# books and data are both kept, which is true -- patch 0045 replaced the only
# RescueParty level that wipes user data, and the three settings levels before
# it reset settings, which that patch's own description calls no cost to the
# reader. This screen says only the books because it has a line budget the site
# does not, and because AGAIN_ZH below already promises the app data in its own
# paragraph. Checked with the website session, 2026-10-30; recorded on both
# sides so that a future comparison of the two does not read this as the site
# having overclaimed.
#
# This screen used to carry a duration, and now carries none: it is drawn again
# after the handover and stands until Android's first frame, which on a plain
# boot is about three minutes and on the boot after an update is minutes for
# every installed app. One number is false in one of those two cases, and a
# reader who acts on the false one force-restarts a device that was working.
#
# What replaced it, until 2026-10-30, was worse than useless: "only a screen
# that flashes over and over means it could not start". A Codex review found
# the counterexample in this tree. PreInit.c stop() writes the reason into the
# report area and then sleeps STOP_SECONDS -- 120 of them -- before rebooting,
# and draws nothing: a start refused by the entry stands perfectly still for
# two minutes and then goes back on its own. Telling the reader that a still
# screen means it is working is a sentence this program can prove wrong.
#
# So neither a clock nor a symptom: what the reader needs is that waiting costs
# nothing and that there is a way back whenever they want one. The flashing is
# not named at all, because a reader who has to sort their symptom into two
# buckets has been given a job, and the answer is the same in both.
MOSTFAIL_ZH = '启动失败时，'
MOSTFAIL_EN = 'When a start fails, '
STUCK_ZH = ('启动时屏幕可能长时间不变，这本身不代表失败，不要强制重启。' + MOSTFAIL_ZH + RESTART_ZH +
            '没有自己回去，就按住电源键不放，直到屏幕变化（最多约 40 秒），同样回到原来的系统。')
STUCK_EN = ('The screen may not change for a long time while Android starts, and that on its own does '
            'not mean it failed; do not force a restart. ' + MOSTFAIL_EN + RESTART_EN +
            ' If it does not go back by itself, hold the power button until the screen changes -- up '
            'to about 40 seconds -- which goes to the same place.')
# The one screen two programs draw: the installer shows it with the stock eips
# before the handover, and the handover's own init draws it again afterwards,
# because the display driver of the kernel that takes over clears the panel as
# it initialises (mxc_epdc_fb_probe -> do_epdc_init_sequence) and the picture
# the reader was looking at does not survive. Defined here so that there is one
# wording and one layout; boot/handoff-init/make-start-picture.py renders this
# entry for the other side.
BOOT_ZH, BOOT_EN = '正在启动 Android', 'Starting Android'
# A system image damaged without changing size. The start refuses it by its
# SHA-256 and the device returns to the library; the tap after that reads the
# refusal out of the report area and writes the image again (install.c,
# read_report), leaving the data image beside it -- books, accounts, settings --
# untouched. This used to say to connect a computer and delete system.img,
# because nothing on this side could tell a damaged image from a good one and
# the owner had to break the record by hand. That is what the report area was
# for (docs/design/diagnostics.md).
AGAIN_ZH = ('回到了书库就再点一次：如果是系统文件坏了，这一次会自己查出来并重装，不需要电脑。'
            '书和应用数据都在另一个文件里，不会丢。')
AGAIN_EN = ('If you end up back in the library, tap it again: a damaged system file is found and written '
            'again on that tap, with no computer needed. Your books and app data are in a separate file '
            'and are not affected.')
BOOT_BODY = [(STUCK_ZH, STUCK_EN), (AGAIN_ZH, AGAIN_EN)]


def screens(payload_mb, payload_name, supported):
    """Each screen: (name, title_zh, title_en, [(text_zh, text_en)], progress, number).

    payload_name is on two of these screens because it is the one thing the
    owner cannot work out from the screen: "copy the package into documents"
    does not say which file, and the person reading it has no Wi-Fi and
    therefore cannot read the project page either. install.c looks for exactly
    this name in documents/ (rel->payload_name), so naming it is also the truth
    about what that folder is searched for.
    """
    tap = '再点书库里的“Readom”'
    restart, restart_en = RESTART_ZH, RESTART_EN
    s = [
        ('CHECK', '正在检查', 'Checking', [], False, False),
        ('DOWNLOAD', '正在下载 Android', 'Downloading Android',
         [('约 %d MB。请保持 Wi-Fi 连接；如果中断，%s会接着下载。' % (payload_mb, tap),
           'About %d MB. Keep Wi-Fi on. If it stops, tap Readom in the library again to continue.' % payload_mb)], True, False),
        ('VERIFY', '正在核对安装包', 'Checking the package', [], True, False),
        ('SYSTEM', '正在安装（第 1 步，共 2 步）', 'Installing (step 1 of 2)',
         [('请不要关机，也不要连接电脑；可以接充电器。', 'Do not turn the Kindle off or connect it to a computer. A charger is fine.')],
         True, False),
        ('DATA', '正在准备存储空间（第 2 步，共 2 步）', 'Preparing storage (step 2 of 2)',
         [('这一步较慢，请耐心等待。请不要关机，也不要连接电脑。', 'This step is slow. Do not turn the Kindle off or connect it to a computer.')],
         True, False),
        ('REPAIR', '正在重装损坏的系统文件', 'Installing the damaged system file again',
         [('上次启动时 Android 发现 Kindle 盘上的系统文件和它该有的样子不一样，就没有启动。现在重装这个文件，'
           '接下来和第一次安装一样。书和应用数据在另一个文件里，不会动。',
           'When Android last started, it found that the system file on the Kindle drive was not what it should be, so '
           'it did not start. That file is being written again now, and what follows looks like the first installation. '
           'Your books and app data are in a separate file and are not touched.'),
          # The one thing this screen cannot work out for the owner: a file that
          # is damaged again after being written and verified is the store
          # failing, not this file. The installer could count the repairs and
          # refuse the second one, but the repairs are not a loop it runs -- each
          # one is a tap, announced by this screen and five minutes long -- so
          # the sentence does what a counter and an error code would.
          ('这一屏如果反复出现，就不是这个文件的问题，而是 Kindle 的存储有问题。请用电脑把书和重要文件备份出来，'
           '再到项目主页报告。' + DIAGNOSTICS_ZH,
           'If this screen keeps coming back, the problem is not this file but the Kindle\'s storage. Copy your books '
           'and anything else you need off it with a computer, and report it on the project page. ' + DIAGNOSTICS_EN)], False, False),
        ('PREPARE', '正在准备启动 Android', 'Preparing to start Android', [], False, False),
        ('BOOT', BOOT_ZH, BOOT_EN, BOOT_BODY, False, False),
        ('BOOT_UPGRADE', '正在启动 Android（更新后第一次）', 'Starting Android (first time after the update)',
         # Three to five minutes is deliberately longer than anything measured
         # (181 s for the one large app on the owner's device with a usage
         # profile, 60-93 s for one without). A screen whose whole purpose is
         # "wait, do not force a restart" is worth nothing if the wait runs past
         # the number on it, and nobody is harmed by a start that finishes
         # early. The measurements stay in docs/design/performance.md, where
         # they are a measurement rather than advice.
         [('已安装的应用要为新系统重新编译。每个应用大约三到五分钟，装得多就更久，期间屏幕不变。请接上充电器，不要强制重启：重启不会损坏数据，但正在编译的应用要从头再来。',
           'The installed apps are being compiled for the new system. That takes three to five minutes per app, longer '
           'if you have many, and the screen will not change meanwhile. Connect a charger and do not force a restart: '
           'it does no harm, but the app being compiled starts over.'),
          (MOSTFAIL_ZH + restart, MOSTFAIL_EN + restart_en)],
         False, False),
    ]
    errors = [
        # The screen used to stop at the requirement, which for the one reader
        # who can act on it is a dead end: a Paperwhite 3 whose owner turned
        # updates off years ago is not a wrong device, it is a right device on
        # an older firmware, and 5.16.2.1.1 is the last one Amazon made for it
        # (docs/design/porting-to-other-models.md), so "update it" is an
        # instruction that always points at the version named here and can
        # always be followed. The version is not written into this sentence --
        # it is already in supported[], which comes from the profile, so the
        # advice stays true for a profile that names another one. The second
        # half keys off something the reader can see rather than off what the
        # update did: a stock update removes the jailbreak unless the hotfix is
        # installed, and the visible difference either way is whether the
        # library still has the entry.
        ('E101', '这台 Kindle 的型号或系统版本不受支持。没有改动任何文件。本安装包只给 %s。'
         '如果机型对得上、只是系统版本更早，把 Kindle 更新到这个版本再试；更新后书库里没有“Readom”，就按越狱教程再越狱一次。' % supported[0],
         'This Kindle model or its software version is not supported. Nothing was changed. '
         'This package is only for the %s. If the model matches and only the software version is older, update the '
         'Kindle to that version and try again; if Readom is then missing from the library, jailbreak it once more.'
         % supported[1]),
        ('E102', '电量低于 40%%。请充电后%s。' % tap, 'The battery is below 40%. Charge it, then tap Readom in the library again.'),
        ('E103', '上一次的安装还在进行，请等它完成。', 'The previous installation is still running. Please wait for it to finish.'),
        ('E104', 'Kindle 连着电脑时不能启动 Android。请拔掉数据线，然后%s。接充电器没有关系。' % tap,
         'Android cannot start while the Kindle is connected to a computer. Unplug the USB cable, then tap Readom in the '
         'library again. A charger is fine.'),
        # The kept part is up to 190 MB in a folder nobody sees from a computer's
        # file list unless they go looking, and the reader who gives up here is
        # the one person no later run of this installer can help: nothing of ours
        # runs again. So this screen says where the space went and how to get it
        # back, which is the whole of the answer -- no partition is made, all of
        # it is ordinary files in one folder.
        # It names android/download and not android, which is where every byte
        # this run took is: prepare_paths() puts the payload and its .part there
        # (install.c). The folder above it is also where an earlier install left
        # system.img and data.img, and data.img is the reader's Android -- their
        # account, their books, where they were in them. Naming the folder above
        # would be right for someone installing for the first time and would
        # destroy all of that for everyone else, and this screen cannot tell
        # which reader is in front of it. The narrower name is true for both.
        # The way out by hand belongs here too, and did not use to be here (a
        # Codex review, 2026-10-30). A network that keeps dropping is exactly
        # where tapping again stops helping, and this screen is where that
        # reader ends up -- E202 names the file because Wi-Fi is off, but a
        # reader whose Wi-Fi is on and whose download will not finish was told
        # only to check the Wi-Fi, which is working.
        # It used to promise that what had been downloaded is kept, and that is
        # not always so: a server that will not resume is answered by deleting
        # the part and starting over, and six of those in a row end here with
        # nothing on the drive (a Codex review, 2026-10-30). Tapping again is
        # the same instruction either way, and "继续" / "to continue" says what
        # the reader does without claiming what is on the drive, so the promise
        # is gone rather than hedged.
        ('E201', '下载中断。请检查 Wi-Fi，然后%s继续。'
         '一直下不动，就用电脑下载 %s，拷进 Kindle 盘的 documents 文件夹，再点一次，不需要再下载。'
         '不想装了就用电脑删掉 Kindle 盘上 android 文件夹里的 download 文件夹，下载占的空间就回来了。'
         % (tap, payload_name),
         'The download stopped. Check Wi-Fi, then tap Readom in the library '
         'again to continue. If it will not finish, download %s with a computer, copy it into the documents '
         'folder on the Kindle drive, and tap again -- nothing is downloaded then. If you would rather not '
         'install it, delete the download folder inside the android folder on the Kindle drive from a '
         'computer and the space it took comes back.' % payload_name),
        ('E202', '需要下载安装包，但 Wi-Fi 没有连接。请连接 Wi-Fi，或者用电脑把 %s 拷进 Kindle 盘的 documents 文件夹，然后%s。'
         % (payload_name, tap),
         'The package must be downloaded, but Wi-Fi is not connected. Connect Wi-Fi, or copy %s into the documents '
         'folder on the Kindle drive from a computer, then tap Readom in the library again.' % payload_name),
        # Every curl 60 lands here, and a wrong clock is only the likeliest of
        # the reasons: a certificate chain that does not verify gives the same
        # code with the clock correct. So the clock is named as the thing to
        # check first, not asserted as the fault (a Codex review, 2026-10-30).
        ('E203', '无法确认下载是否安全。最常见的原因是 Kindle 的日期和时间不对，请先在设置里核对，然后%s。'
         '时间没问题还是这一屏，就不是时间的事，请到项目主页报告。' % tap,
         'The download could not be checked as safe. The usual reason is that the Kindle\'s date and time are '
         'wrong, so check those in Settings first, then tap Readom in the library again. If the time is right '
         'and this screen comes back, the time is not the problem -- please report it on the project page.'),
        ('E302', '安装包不完整或已损坏，已经删除。%s会重新下载；也可以用电脑把 %s 重新拷进 documents 文件夹。'
         % (tap.replace('再点', '再点一次'), payload_name),
         'The package was incomplete or damaged and has been deleted. Tap Readom in the library again to download it, '
         'or copy %s into the documents folder again from a computer.' % payload_name),
        ('E303', '这个版本的发行文件有问题，与你的 Kindle 无关。可以改用上一个版本，并到项目主页报告。' + DIAGNOSTICS_ZH,
         'There is a problem with this release, not with your Kindle. You can use the previous version instead, and '
         'please report it on the project page. ' + DIAGNOSTICS_EN),
        ('E304', '已有的 Android 数据来自不兼容的版本，没有改动任何文件。请继续使用原来的版本。'
         '如果你从来没装过 Android，就用电脑删掉 Kindle 盘上 android 文件夹里的 data.img，然后%s。' % tap,
         'The Android data on this Kindle is from an incompatible version. Nothing was changed. Please keep using the '
         'earlier version. If you have never installed Android, delete data.img in the android folder on the Kindle '
         'drive from a computer, then tap Readom in the library again.'),
        # The same folder as E201, for the same reason, and here the reader is
        # already being asked to make room: the one thing they must not be sent
        # to delete for it is the Android they already have.
        ('E401', 'Kindle 空间不足。请删除一些书，腾出下面这么多空间，然后%s。'
         '不想装了就用电脑删掉 Kindle 盘上 android 文件夹里的 download 文件夹，下载占的空间就回来了。' % tap,
         'There is not enough space. Delete some books to free the space below, then tap Readom in the '
         'library again. If you would rather not install it, delete the download folder inside the android '
         'folder on the Kindle drive and the space it took comes back.'),
        ('E402', '读写存储时出错。请重启 Kindle 后%s。' % tap, 'Reading or writing the storage failed. Restart the Kindle, then tap '
         'Readom in the library again.'),
        ('E403', '内存不足。请重启 Kindle 后%s。' % tap, 'Out of memory. Restart the Kindle, then tap Readom in the library again.'),
        # Not E402's wording. E402 says a read or a write failed; here nothing was
        # read or written wrong -- the installer looked first and refused, because the
        # place it would have written is shared with another file and writing there
        # would have gone into that file. The two need different things done, so they
        # cannot share a screen. And it does not tell the reader to delete anything:
        # what is wrong is the drive's own bookkeeping, which a computer repairs.
        # "Nothing was written there", not "nothing was written": by this point
        # the state file exists and the report area may have been remade. What is
        # true, and what the reader is being told, is that the shared clusters
        # were not written over -- their book is as it was.
        ('E404', 'Kindle 盘上的文件系统有问题：安装要写的位置和另一个文件共用，写下去会覆盖那个文件，'
         '所以没有往那里写。请用数据线把 Kindle 连到电脑，让电脑检查并修复这个盘，然后%s。' % tap,
         'The Kindle drive\'s filesystem is inconsistent: the place the installation would be written is shared '
         'with another file, and writing there would have written into that file, so nothing was written there. '
         'Connect the Kindle to a computer, let it check and repair the drive, then tap Readom in the library again.'),
        # "Restart the Kindle, then tap again" was the advice while this side
        # could not know why the start failed. It can read that now: the reason
        # is in the report area, and the next tap acts on it -- a damaged system
        # image is written again, with its own screen. So the first thing to do
        # is the tap, not the restart.
        ('STOPPED', '没能启动 Android，已回到原来的系统。请%s：没能启动的原因留在了盘上，下一次点击会读到它，'
         '该重装的会自己重装，不需要电脑。' % tap.replace('再点', '再点一次') + DIAGNOSTICS_ZH,
         'Android could not be started; the Kindle is back on its own system. Tap Readom in the library again: the '
         'reason it could not start is on the drive, the next tap reads it, and anything that needs reinstalling is '
         'reinstalled, with no computer needed. ' + DIAGNOSTICS_EN),
    ]
    # Every screen that ends a run stays until the reader taps it (kindle.h,
    # kindle_hold_screen): the launcher that runs this script repaints the
    # library over the screen about a second after the script exits, so the run
    # waits instead, and the screen has to say what it is waiting for. The last
    # sentence of the text and not a paragraph of its own, because E401 draws
    # its number under the text, and a paragraph between "the space below" and
    # the number would put the number under the wrong sentence.
    errors = [(code, zh + TAP_BACK_ZH, en + ' ' + TAP_BACK_EN) for code, zh, en in errors]
    for code, zh, en in errors:
        if code == 'STOPPED':
            s.append((code, '没能启动', 'Could not start', [(zh, en)], False, False))
        else:
            s.append((code, '无法继续（%s）' % code, 'Cannot continue (%s)' % code, [(zh, en)], False, code == 'E401'))
    return s


def wrap(draw, text, font, width, cjk):
    """Lines no wider than width: Chinese breaks between characters (not before closing
    or after opening punctuation), English between words."""
    if not cjk:
        lines, line = [], ''
        for word in text.split(' '):
            trial = (line + ' ' + word).strip()
            if line and draw.textlength(trial, font=font) > width:
                lines.append(line)
                line = word
            else:
                line = trial
        return [l for l in lines + [line] if l]
    # Tokens: a run of Latin letters, digits and their punctuation stays whole (Wi-Fi, 196, documents).
    tokens = re.findall(r"[A-Za-z0-9][A-Za-z0-9.'/-]*|.", text)
    lines, line = [], []
    for tok in tokens:
        if not line and tok == ' ':
            continue
        if line and draw.textlength(''.join(line + [tok]), font=font) > width:
            # A closing mark may not start a line, nor an opening mark end one:
            # the token before goes down too. Then the same question is asked
            # again of the token that just became the first, because one step
            # is not enough: 「…“Readom”。」 breaks at the full stop, moves the
            # closing quotation mark down with it, and leaves a line beginning
            # 」。 -- which is what the E202 screen did until 2026-10-23.
            #
            # Both halves of the question are asked of visible characters. A
            # space carries none: as the line's last token it hid an opening
            # mark from the test (「甲（ 乙」 broke after the bracket, and the
            # rstrip below then left it ending the line), and backtracking can
            # hand back a token that begins with the space the break fell on
            # (「甲乙 ，丙」 started the next line with it). Neither shows in
            # any screen's text today; both are a wording away, and a Codex
            # review found them on 2026-10-30.
            while len(line) > 1 and (tok.lstrip(' ')[:1] in NO_LINE_START
                                     or ''.join(line).rstrip()[-1:] in NO_LINE_END):
                tok = line.pop() + tok
            lines.append(''.join(line).rstrip())
            tok = tok.lstrip(' ')
            line = [tok] if tok else []
        else:
            line.append(tok)
    # Nothing is a line. Text ending in a space left an empty last line, and a
    # paragraph of nothing but spaces left one empty line, and text_block()
    # gives every line in this list a line's height -- so an invisible one
    # pushes the screen down by a line and can be what makes it too tall to
    # fit. The guard above keeps the loop from emitting empty lines in the
    # middle; this is the end of the same rule.
    #
    # One limit stays, and is written down rather than papered over: when
    # backtracking runs out (the line is down to one token), an opening mark
    # can still end a line -- 「甲（   乙」 at a width that fits neither. The
    # alternative is emitting an empty line to avoid it, which is worse. No
    # screen's text reaches it today.
    return [l for l in lines + [''.join(line).rstrip()] if l]


class Fonts:
    def __init__(self, cjk, latin, latin_title):
        self.cjk = {size: ImageFont.truetype(str(cjk), size, index=2) for size in (TITLE_ZH, BODY_ZH)}
        if self.cjk[BODY_ZH].getname()[0] != 'Noto Sans CJK SC':
            sys.exit('face 2 of %s is %s, not Noto Sans CJK SC' % (cjk, self.cjk[BODY_ZH].getname()))
        self.latin = {BODY_EN: ImageFont.truetype(str(latin), BODY_EN), NUMBER: ImageFont.truetype(str(latin), NUMBER)}
        self.latin_title = ImageFont.truetype(str(latin_title), TITLE_EN)


def text_block(fonts, title_zh, title_en, paragraphs):
    """The screen's text as one grey bitmap of the region's width."""
    scratch = ImageDraw.Draw(Image.new('L', (1, 1)))
    rows = []   # (text, font, size, gap before)
    rows.append((title_zh, fonts.cjk[TITLE_ZH], TITLE_ZH, 0))
    rows.append((title_en, fonts.latin_title, TITLE_EN, GAP_LANG))
    for zh, en in paragraphs:
        for i, line in enumerate(wrap(scratch, zh, fonts.cjk[BODY_ZH], REGION_W, True)):
            rows.append((line, fonts.cjk[BODY_ZH], BODY_ZH, GAP_PARA if i == 0 else 0))
        for i, line in enumerate(wrap(scratch, en, fonts.latin[BODY_EN], REGION_W, False)):
            rows.append((line, fonts.latin[BODY_EN], BODY_EN, GAP_LANG if i == 0 else 0))
    height = sum(round(size * LINE) + gap for _, _, size, gap in rows)
    img = Image.new('L', (REGION_W, height), 255)
    d = ImageDraw.Draw(img)
    y = 0
    for text, font, size, gap in rows:
        y += gap
        # Baselines on whole pixels: ascent placed from the line's top with the extra leading split evenly.
        ascent, descent = font.getmetrics()
        top = y + (round(size * LINE) - (ascent + descent)) // 2
        d.text((0, top), text, font=font, fill=0)
        if d.textlength(text, font=font) > REGION_W:
            sys.exit('a line is wider than the region: ' + text)
        y += round(size * LINE)
    return img


def glyphs(fonts):
    """Characters for numbers: their bitmaps side by side, and each one's x and width."""
    chars = '0123456789% MB'
    font = fonts.latin[NUMBER]
    ascent, descent = font.getmetrics()
    height = ascent + descent
    scratch = ImageDraw.Draw(Image.new('L', (1, 1)))
    widths = [round(scratch.textlength(c, font=font)) for c in chars]
    img = Image.new('L', (sum(widths), height), 255)
    d = ImageDraw.Draw(img)
    x = 0
    for c, w in zip(chars, widths):
        d.text((x, 0), c, font=font, fill=0)
        x += w
    xs = [sum(widths[:i]) for i in range(len(chars))]
    return img, chars, xs, widths


def zstd(data):
    return subprocess.run(['zstd', '-q', '-19', '-c'], input=data, capture_output=True, check=True).stdout


def sha(data):
    return hashlib.sha256(data).hexdigest()


def compose(text, progress, number_text, glyph_img, chars, xs, widths, region_h):
    """What screen.c makes on the device, for the previews."""
    region = Image.new('L', (REGION_W, region_h), 255)
    region.paste(text, (0, 0))
    y = text.height
    if progress is not None:
        y += BAR_GAP
        d = ImageDraw.Draw(region)
        d.rectangle([0, y, REGION_W - 1, y + BAR_H - 1], fill=0)
        d.rectangle([BAR_LINE, y + BAR_LINE, REGION_W - 1 - BAR_LINE, y + BAR_H - 1 - BAR_LINE], fill=255)
        inner = REGION_W - 2 * BAR_LINE
        fill = inner * progress // 1000
        if fill:
            d.rectangle([BAR_LINE, y + BAR_LINE, BAR_LINE + fill - 1, y + BAR_H - 1 - BAR_LINE], fill=0)
        y += BAR_H
    if number_text is not None:
        y += NUMBER_GAP
        x = 0
        for c in number_text:
            i = chars.index(c)
            region.paste(glyph_img.crop((xs[i], 0, xs[i] + widths[i], glyph_img.height)), (x, y))
            x += widths[i]
    screen = Image.new('L', (SCREEN_W, SCREEN_H), 255)
    screen.paste(region, (REGION_X, REGION_Y))
    return screen


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ['manifest', 'profile', 'cjk-font', 'latin-font', 'latin-title-font', 'output']:
        ap.add_argument('--' + name, type=Path, required=True)
    a = ap.parse_args()
    if platform.node() != 'lima-pw3' or os.geteuid() == 0:
        sys.exit('Run as the builder in the build VM.')
    manifest = json.loads(a.manifest.read_text())
    payload_mb = round(manifest['files']['system']['bytes'] / 1e6)
    payload_name = manifest['files']['system']['name']
    fonts = Fonts(a.cjk_font, a.latin_font, a.latin_title_font)
    out = a.output
    out.mkdir(parents=True, exist_ok=False)
    (out / 'preview').mkdir()

    profile = json.loads(a.profile.read_text())
    supported = (profile['supported_zh'], profile['supported_en'])
    defs = screens(payload_mb, payload_name, supported)
    glyph_img, chars, xs, widths = glyphs(fonts)
    texts = [text_block(fonts, title_zh, title_en, paras) for _, title_zh, title_en, paras, _, _ in defs]
    # How tall each screen draws, as screen_render draws it: its text, then a
    # progress bar with the percentage under it, or the number by itself, or
    # neither. The region is the tallest of those, because the device clears the
    # whole of it before every screen. Reserving the bar and the number on every
    # screen would reserve them on the longest screen too, which has neither.
    with_bar = BAR_GAP + BAR_H + NUMBER_GAP + glyph_img.height
    with_number = NUMBER_GAP + glyph_img.height
    region_h = max(text.height + (with_bar if progress else with_number if number else 0)
                   for text, (_, _, _, _, progress, number) in zip(texts, defs))
    if REGION_Y + region_h > SCREEN_H:
        sys.exit('the screens do not fit: %d px high, and there is room for %d' % (region_h, SCREEN_H - REGION_Y))

    blob, index = b'', []
    for (name, *_), img in zip(defs, texts):
        packed = zstd(img.tobytes())
        index.append((name, len(blob), len(packed), img.width, img.height))
        blob += packed
    packed = zstd(glyph_img.tobytes())
    glyph_entry = (len(blob), len(packed), glyph_img.width, glyph_img.height)
    blob += packed
    (out / 'screens.bin').write_bytes(blob)

    gen = '/* Generated by installer/screens/render_screens.py; do not edit. */'
    h = [gen, '#pragma once', '#include <stdint.h>',
         '#define PW3_SCREEN_W %d' % SCREEN_W, '#define PW3_SCREEN_REGION_X %d' % REGION_X,
         '#define PW3_SCREEN_REGION_Y %d' % REGION_Y, '#define PW3_SCREEN_REGION_W %d' % REGION_W,
         '#define PW3_SCREEN_REGION_H %d' % region_h, '#define PW3_SCREEN_BAR_H %d' % BAR_H,
         '#define PW3_SCREEN_BAR_LINE %d' % BAR_LINE, '#define PW3_SCREEN_BAR_GAP %d' % BAR_GAP,
         '#define PW3_SCREEN_NUMBER_GAP %d' % NUMBER_GAP, '#define PW3_SCREENS_BYTES %du' % len(blob),
         'struct pw3_bitmap { uint32_t offset, packed; uint16_t width, height; };', 'enum pw3_screen {']
    h += ['    PW3_SCREEN_%s,' % name for name, *_ in defs] + ['    PW3_SCREEN_COUNT', '};', '']
    (out / 'screens.h').write_text('\n'.join(h))
    u = '__attribute__((unused)) static const'
    d = [gen, '#pragma once', '#include "screens.h"',
         '%s struct pw3_bitmap pw3_screen_text[PW3_SCREEN_COUNT] = {' % u]
    d += ['    {%du, %du, %d, %d},' % entry[1:] for entry in index] + ['};']
    d += ['%s uint8_t pw3_screen_progress[PW3_SCREEN_COUNT] = {%s};' % (u, ', '.join(str(int(x[4])) for x in defs)),
          '%s uint8_t pw3_screen_number[PW3_SCREEN_COUNT] = {%s};' % (u, ', '.join(str(int(x[5])) for x in defs)),
          '%s char *const pw3_screen_names[PW3_SCREEN_COUNT] = {%s};' % (u, ', '.join('"%s"' % x[0] for x in defs)),
          '%s struct pw3_bitmap pw3_screen_glyphs = {%du, %du, %d, %d};' % ((u,) + glyph_entry),
          '%s char pw3_screen_glyph_chars[] = "%s";' % (u, chars),
          '%s uint16_t pw3_screen_glyph_x[] = {%s};' % (u, ', '.join(map(str, xs))),
          '%s uint16_t pw3_screen_glyph_w[] = {%s};' % (u, ', '.join(map(str, widths))), '']
    (out / 'screens-data.h').write_text('\n'.join(d))

    samples = {'DOWNLOAD': (370, None), 'VERIFY': (820, None), 'SYSTEM': (450, None), 'DATA': (90, None), 'E401': (None, '812 MB')}
    previews = {}
    for (name, _, _, _, progress, number), img in zip(defs, texts):
        p, n = samples.get(name, (None, None))
        shown = compose(img, p if progress else None, (n if number else ('%d%%' % (p // 10) if progress else None)),
                        glyph_img, chars, xs, widths, region_h)
        path = out / 'preview' / (name + '.png')
        shown.save(path, optimize=True)
        previews[name] = sha(path.read_bytes())

    result = {'schema': 2, 'payload_mb': payload_mb, 'payload_name': payload_name,
              'supported': list(supported),
              'profile': a.profile.name, 'region': [REGION_X, REGION_Y, REGION_W, region_h],
              'screens': [d[0] for d in defs], 'screens_bin': {'bytes': len(blob), 'sha256': sha(blob)},
              'headers_sha256': {n: sha((out / n).read_bytes()) for n in ('screens.h', 'screens-data.h')},
              'fonts': {str(p): sha(p.read_bytes()) for p in (a.cjk_font, a.latin_font, a.latin_title_font)},
              'pillow': Image.__version__, 'previews_sha256': previews}
    (out / 'build-result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: result[k] for k in ('region', 'screens_bin')}, indent=1))


if __name__ == '__main__':
    main()
