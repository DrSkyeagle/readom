#!/bin/bash
# What this project changed, read off the running device. Reads only: nothing
# here takes the screen, writes to /data, or changes power state.
#
# This lives in the repository on purpose. Until V66 each release carried its own
# copy under build-share/vNN/, which meant every copy inherited the previous
# one's defects -- three of them were found at once when V66's copy finally ran
# on a device: a check that only works in the first minute after boot reported a
# failure, a line-rejoining bug reported eleven, and a `case` inside a command
# substitution was a syntax error. None of those were about the device.
#
# Usage: tools/verify-device.sh
#   ADB=...    path to adb (default: the Homebrew command-line-tools one)
#   ADB_PORT=  adb server port, if another server has the device (see below)
set -u
ADB_BIN=${ADB:-/opt/homebrew/share/android-commandlinetools/platform-tools/adb}
ADB_PORT=${ADB_PORT:-}
adb_() { if [ -n "$ADB_PORT" ]; then "$ADB_BIN" -P "$ADB_PORT" "$@"; else "$ADB_BIN" "$@"; fi; }
pass=0; fail=0; unknown=0
ck() { if [ "$2" = 1 ]; then echo "ok   $1"; pass=$((pass+1)); else echo "FAIL $1${3:+  ($3)}"; fail=$((fail+1)); fi; }
# A third outcome, because "could not look" and "looked and it is wrong" are not
# the same finding and reporting the first as the second sends people after the
# device when the problem is here.
na() { echo "--   $1${2:+  ($2)}"; unknown=$((unknown+1)); }
sh_() { adb_ shell "$@" 2>/dev/null | tr -d '\r'; }

echo "=== 一、设备在不在 ==="
devs=$(adb_ devices | sed -n '2p')
if [ -z "$devs" ]; then
  echo "FAIL adb 看不到设备"
  # The one that has cost the most time: another adb server holding the device.
  for port in 5097 5038; do
    if [ -n "$("$ADB_BIN" -P $port devices 2>/dev/null | sed -n '2p')" ]; then
      echo "     但是端口 $port 上的 adb 服务有它。重跑："
      echo "         ADB_PORT=$port $0"
      exit 1
    fi
  done
  echo "     其他 adb 服务端口上也没有。线插着吗？Android 起来了吗？"
  exit 1
fi
ck "adb 看得到设备" 1 ""
echo "     指纹 $(sh_ 'getprop ro.build.fingerprint')"
up=$(sh_ 'cat /proc/uptime' | cut -d. -f1)
echo "     已开机 ${up:-?} 秒"

# Root for the two readings that need it, without restarting adbd: after
# `adb root` this host's `adb devices` goes empty, because the device moves to
# another adb server on this machine rather than leaving the bus
# (docs/known-issues.md section 3). `su 0` does not restart adbd.
ROOT_PREFIX=""
if [ "$(sh_ 'id -u')" = 0 ]; then
  ROOT="yes"
elif [ "$(sh_ 'su 0 id -u')" = 0 ]; then
  ROOT="yes"; ROOT_PREFIX="su 0 "
else
  ROOT="no"
fi
root_sh_() { sh_ "$ROOT_PREFIX$1"; }
echo "     提权：$([ "$ROOT" = yes ] && echo "可以${ROOT_PREFIX:+（su 0）}" || echo "不行，要 root 的几项会跳过")"

# CONFIG_LOG_BUF_SHIFT=15: the kernel log is a 32 KB ring, and a minute or so
# into a run its oldest line is already past the point where PID 1, init and
# this project's own services ran. Every question below that is answered out of
# the kernel log has to know that, because "grep found nothing" and "the lines
# are gone" are the same zero -- and the most important question here, whether
# any of this project's domains was denied anything at boot, read a clean zero
# on a device whose log had held that very denial half a minute earlier.
#
# It is read out of logcat's kernel buffer, not out of dmesg. Two reasons, and
# the second is the better one:
#   * `dmesg` needs root -- the unprivileged shell is denied syslog_read -- and
#     a release is a `user` build with no su, so on the build a reader actually
#     gets, every question below would have gone unanswered;
#   * logd copies the whole kernel ring into a buffer of its own when it starts
#     (about 31 seconds in) and keeps it, so it holds lines the 32 KB ring has
#     long since dropped. Measured on the device on 2026-10-23, 35 minutes into
#     a V81 run: dmesg had no `pw3-preinit` line left, and logcat's kernel
#     buffer still had 8 of them, back to 0.47 seconds after boot.
klog_() { sh_ 'logcat -b kernel -d'; }
# How far back that buffer reaches, in seconds since boot. `-v epoch` prints the
# time right-aligned, so the field is taken as the first ten-digit run.
KLOG_FROM=$(sh_ 'now=$(date +%s); up=$(cut -d" " -f1 /proc/uptime); up=${up%.*}
  first=$(logcat -b kernel -d -v epoch 2>/dev/null | grep -m1 -oE "[0-9]{10}")
  [ -n "$first" ] && echo $(( first - now + up ))')
echo "     内核日志（logcat 的 kernel 缓冲）最早到第 ${KLOG_FROM:-?} 秒"
# Everything this project runs at boot is done by about 35 seconds (the handover
# is at 8, AOSP init starts services from 31). A buffer that starts later than
# that cannot answer a question about boot.
klog_covers_boot() { [ -n "$KLOG_FROM" ] && [ "$KLOG_FROM" -le 30 ]; }

echo
echo "=== 二、安装时的编译级别 ==="
# bg-dexopt is "speed" since 2026-10-29, not "speed-profile". It is the only
# thing that ever compiles an application the owner installed, and over a use
# rather than one cold start the two are not close: opening a book is 95.6 s at
# speed-profile against 51.4 s at speed, four rounds against two with no overlap
# (docs/design/performance.md section 12). This check is here precisely because
# the value is easy to lose in a merge, so it has to say the current answer.
for pair in "pm.dexopt.install quicken" "pm.dexopt.bg-dexopt speed" \
            "pm.dexopt.ab-ota speed-profile" "pm.dexopt.shared speed"; do
  prop=${pair%% *}; want=${pair##* }
  got=$(sh_ "getprop $prop")
  ck "$prop = $want" "$([ "$got" = "$want" ] && echo 1 || echo 0)" "读到 '$got'"
done
# What the reader sees when they tap an application: with the starting window
# on, the system draws the target activity's own windowBackground at once --
# for the application this device is for, its own logo page -- instead of the
# previous screen staying put for the twenty to thirty seconds its first frame
# takes (frameworks/base 0068, pw3.mk). The debug property overrides it on the
# device, so this also says that nothing was left overriding it here.
start_ro=$(sh_ 'getprop ro.pw3.disable_starting_window')
start_debug=$(sh_ 'getprop debug.pw3.disable_starting_window')
ck "启动窗口是开着的（ro.pw3.disable_starting_window = false）" \
   "$([ "$start_ro" = false ] && echo 1 || echo 0)" "读到 '$start_ro'"
ck "没有留下 debug 覆盖" "$([ -z "$start_debug" ] && echo 1 || echo 0)" "读到 '$start_debug'"
# default.prop is where this one lives; build.prop is read later and would win.
# Both files are 0600 root-only in this build, so the unprivileged shell gets
# "Permission denied" and the count comes back empty. Read as root, and say "要
# root" rather than calling an unreadable file a wrong one: on 2026-10-21 this
# and five more checks below reported a device that was in fact correct.
if [ "$ROOT" != yes ]; then
  na "default.prop 里写的就是 quicken，而 build.prop 没有覆盖它" \
     "要 root（两个文件都是 0600）；生效值上面四项已经用 getprop 验过了，这一条只多验一层来历，要看来历就在构建机上看那个镜像"
else
  dp=$(root_sh_ 'grep -c "^pm.dexopt.install=quicken" /default.prop')
  bp=$(root_sh_ 'grep -c "^pm.dexopt.install" /system/build.prop')
  ck "default.prop 里写的就是 quicken，而 build.prop 没有覆盖它" \
     "$([ "$dp" = 1 ] && [ "${bp:-0}" = 0 ] && echo 1 || echo 0)" "default.prop $dp 行, build.prop $bp 行"
fi

echo
echo "=== 三、SELinux：本项目自己的域有没有被自己的策略挡住 ==="
# The V65 and V66 failures were both this. Every avc line whose scontext is one
# of this project's own domains is a missing rule, by definition; the platform's
# own domains have benign ones (docs/known-issues.md section 2).
# Both buffers, because a denial goes to one or the other depending on when it
# happened: before auditd is up the kernel logs it itself (logcat's kernel
# buffer), and afterwards auditd relays it into the main buffer under the tag of
# the process that was denied. Reading only one of them is a zero that means
# "not in this half". Neither needs root: shell is in the log group (1007).
own_k=$(klog_ | grep 'avc:' | grep -c 'scontext=u:r:pw3_')
own_m=$(sh_ 'logcat -d' | grep 'avc: denied' | grep -c 'scontext=u:r:pw3_')
own=$(( ${own_k:-0} + ${own_m:-0} ))
if [ "$own" != 0 ]; then
  ck "没有 pw3_* 域的拒绝" 0 "$own 条"
  echo "     ---- 原文（照 tcontext 和 tclass 补规则）----"
  { klog_ | grep 'avc:' | grep 'scontext=u:r:pw3_'
    sh_ 'logcat -d' | grep 'avc: denied' | grep 'scontext=u:r:pw3_'; } | sed 's/^/     /'
elif klog_covers_boot; then
  ck "没有 pw3_* 域的拒绝" 1 ""
else
  # The zero is real but it only covers from KLOG_FROM on, and everything this
  # project runs at boot happened before that.
  na "没有 pw3_* 域的拒绝" \
     "第 ${KLOG_FROM} 秒之后没有，但开机那段已经被绕掉了——这一项要在开机后一分钟内跑才算数"
fi
enf=$(sh_ 'getenforce')
ck "SELinux 是 Enforcing" "$([ "$enf" = Enforcing ] && echo 1 || echo 0)" "读到 '$enf'"
# Not "none at all": on a userdebug build `su` is permissive by design, the only
# one (checked against the built policy with sepolicy-analyze), and taking root
# at all produces that line. A `user` build has no su, so there the count is a
# plain zero.
perm=$(klog_ | grep "permissive=1" | grep -vc 'scontext=u:r:su:')
ck "除 su（userdebug 构建里唯一的 permissive 域）外没有 permissive=1" \
   "$([ "${perm:-1}" = 0 ] && echo 1 || echo 0)" "$perm 条"

echo
echo "=== 四、存储映射与诊断区 ==="
node=$(sh_ 'ls -l /dev/block/dm-3 2>/dev/null' | cut -c1)
ck "/dev/block/dm-3 在，而且是块设备" "$([ "$node" = b ] && echo 1 || echo 0)" "读到 '$node'"
# Which mapping is mounted where, and read-only or not. This is the cross-link
# question -- the one PID 1 refuses to start on -- and /proc/mounts answers it
# without root, so it is asked on a release build too. Three lines, because dm-3
# is the report area and carries no filesystem.
mounts=$(sh_ 'cat /proc/mounts')
for want in "dm-0 /pw3-user vfat ro" "dm-1 /system ext4 ro" "dm-2 /data ext4 rw"; do
  set -- $want
  line=$(echo "$mounts" | grep "^/dev/block/$1 ")
  at=$(echo "$line" | cut -d' ' -f2); kind=$(echo "$line" | cut -d' ' -f3)
  mode=$(echo "$line" | cut -d' ' -f4 | cut -d, -f1)
  ok=1; [ "$at" = "$2" ] || ok=0; [ "$kind" = "$3" ] || ok=0; [ "$mode" = "$4" ] || ok=0
  ck "$1 挂在 $2，$3，$4" "$ok" "读到 '${at:-没挂} ${kind} ${mode}'"
done
# And the device's own read-only flag, which is the stronger statement: a mount
# flag can be remounted, this one cannot. /sys/class/block/dm-*/ro is not
# readable by the shell domain, so only this half needs root.
for pair in "0 0" "1 1" "2 0" "3 0"; do
  n=${pair%% *}; want=${pair##* }
  if [ "$ROOT" != yes ]; then
    na "dm-$n 的设备级 ro = $want" \
       "要 root（sysfs 对 shell 域不开放）；同一件事 PID 1 每次开机自己验——建完映射核对 DM_READONLY_FLAG（TestFileMapper.h 的 pw3_file_map_status），不符就停住，所以 Android 起来了就说明这条对"
    continue
  fi
  got=$(root_sh_ "cat /sys/class/block/dm-$n/ro 2>/dev/null")
  ck "dm-$n 的设备级 ro = $want" "$([ "$got" = "$want" ] && echo 1 || echo 0)" "读到 '$got'"
done
# Whether the report area was actually written, which is the thing the area is
# for. pw3-boot-report says so in the kernel log either way, under a tag of its
# own (BootReport.cpp, KernelLine) because the tag it shares with the logcat
# relay is dropped by the entry's filter.
#
# It cannot be asked early. The program samples at 0, 30, 30 and 60 seconds and
# writes the area last, so there is no line to find until about two minutes in --
# and the kernel log questions above want to be asked inside the first minute,
# before 32 KB of ring has wrapped past the boot. The two cannot share one
# instruction, so this one says how long to wait rather than calling an absence a
# failure. Uptime is read for that, not guessed.
# The line is in the kernel log, which is readable without root (see klog_), so
# this one is asked on a release build too -- which is the build it matters on.
# Matched on the tag, not on "report area": the line about the area's device
  # node not being usable begins with the node's path, and grepping for the
  # phrase would have missed exactly the failure V66 had.
line=$(klog_ | grep "pw3-report-area:" | tail -1)
up=$(sh_ 'cat /proc/uptime' | cut -d. -f1)
case "$line" in
  *written*)    ck "诊断区这一次写成了" 1 ""; wrote_this_boot=yes ;;
  *incomplete*) ck "诊断区这一次写成了" 0 "写了一半：$line" ;;
  *"not built"*) ck "诊断区这一次写成了" 0 "报告装不进去：$line" ;;
  *unusable*)   ck "诊断区这一次写成了" 0 "拿不到那个设备节点：$line" ;;
  *"cannot be opened"*) ck "诊断区这一次写成了" 0 "打不开：$line" ;;
  *)            if [ -n "$line" ]; then
                  ck "诊断区这一次写成了" 0 "$line"
                elif [ -n "$up" ] && [ "$up" -lt 150 ]; then
                  na "诊断区这一次写成了没有" "开机才 ${up} 秒，pw3-boot-report 要采样到约 120 秒才写——过两分钟再跑一次这个脚本"
                elif klog_covers_boot; then
                  ck "诊断区这一次写成了" 0 "开机 ${up} 秒了，日志里一个字都没说，而缓冲还能回到第 ${KLOG_FROM} 秒——pw3-boot-report 没跑，或者跑了却什么都没说"
                else
                  na "诊断区这一次写成了没有" "日志里没有这一行，而缓冲最早只到第 ${KLOG_FROM} 秒"
                fi ;;
esac
# And whether the stock side is acting on what the area says, which is what the
# area became on 2026-10-21 (docs/open-items.md, 二十). The user store is
# mounted read only here, so its state file can be read from Android. Two
# things are being asked, and they fail differently:
#
#   * no android= line at all -- the installer that wrote this file predates
#     the accounting, so a damaged system image is still the loop with no way
#     out and the compiling notice still runs out by taps;
#   * the line present but stuck at 0 while the area holds a report -- the
#     installer read the area and did not write down what it read, so the next
#     tap would treat the same report as new again.
#
# "Stuck at 0" cannot be judged on a device whose Android has never written a
# report, which is also when 0 is the right answer, so the second half is only
# asked once the area says it has been written.
# Read as root: the user store is mounted for the system, and the shell user
# gets "Permission denied" on every path under it -- which this check used to
# report as "no state file", sending the reader after a file that was there all
# along.
state_schema=$(root_sh_ 'grep -c "^schema=1$" /pw3-user/android/state 2>/dev/null')
state_line=$(root_sh_ 'grep "^android=" /pw3-user/android/state 2>/dev/null' | tail -1)
if [ "$ROOT" != yes ]; then
  na "安装器记下了它处理到哪一份报告" \
     "要 root 才读得到 /pw3-user；发行版上这一条从 Kindle 盘读：接电脑看 android/state 里的 schema= 和 android= 两行"
elif [ "${state_schema:-0}" = 0 ]; then
  # Not a judgement: the test entry hands over without an installer at all, and
  # then there is no state file to have a line in.
  na "安装器记下了它处理到哪一份报告" "/pw3-user/android/state 读不到或不是安装器写的"
elif [ -z "$state_line" ]; then
  ck "安装器记下了它处理到哪一份报告" 0 \
     "状态文件里没有 android= 这一行：写它的安装器还不认报告区，第七、八节那两个缺陷在这台机器上还在"
else
  ck "安装器记下了它处理到哪一份报告" 1 "$state_line"
fi
# The line the compiling notice now rests on: every report carries
# ro.build.version.incremental, and the installer clears the notice when it
# matches the build of the system that entry installs (install.c,
# PW3_REPORT_BUILD). Both halves of that chain can break quietly -- the sampler
# could stop recording the property, or record something other than what the
# running system answers -- and then the notice would simply never clear, with
# nothing failing anywhere.
#
# Asked of this boot's own report, so not before the sampler has written it:
# until then the area holds the previous system's report, whose build is
# *supposed* to differ (that is the replay this check's subject refuses).
# The magic first, because a file that does not begin with it is not a report at
# all: PID 1 will not map it and the installer reads nothing out of it, so a
# version number still legible inside it would have this check call a dead
# channel healthy. The checksum over the body is the installer's own test and is
# not repeated here -- what this check is for is the sampler, not the file's
# integrity -- so it says "unverified" rather than "ok" when the magic is wrong.
report_magic=$(root_sh_ 'head -c 13 /pw3-user/android/report.bin 2>/dev/null')
report_build=$(root_sh_ 'strings /pw3-user/android/report.bin 2>/dev/null | grep "^ro.build.version.incremental=" | tail -1' |
               sed 's/^ro.build.version.incremental=//')
live_build=$(sh_ 'getprop ro.build.version.incremental')
up=${up:-$(sh_ 'cat /proc/uptime' | cut -d. -f1)}
if [ "$ROOT" != yes ]; then
  na "报告写下了这个系统的版本号" \
     "要 root 才读得到报告区；发行版上这一条从 Kindle 盘读：接电脑看 android/report.bin （开头应为 PW3-REPORT-1），或看 android/logs/ 里安装器那次点击的日志"
elif [ -n "$report_magic" ] && [ "$report_magic" != "PW3-REPORT-1 " ]; then
  na "报告写下了这个系统的版本号" "报告区开头不是 PW3-REPORT-1，安装器不会读它——先看上一项"
elif [ -z "$live_build" ]; then
  na "报告写下了这个系统的版本号" "读不到 ro.build.version.incremental"
elif [ -z "$report_build" ] && [ -n "$up" ] && [ "$up" -lt 150 ]; then
  na "报告写下了这个系统的版本号" "开机才 ${up} 秒，这一次的报告还没写——过两分钟再跑一次"
elif [ "$report_build" = "$live_build" ] && [ "${wrote_this_boot:-no}" = yes ]; then
  ck "报告写下了这个系统的版本号" 1 "$live_build"
elif [ "$report_build" = "$live_build" ]; then
  # The value matches, but nothing says this boot is the one that wrote it: a
  # report left by an earlier start of the same release reads exactly the same.
  # Saying "ok" there would call a channel that stopped working healthy, which
  # is the one thing this check exists to catch.
  na "报告写下了这个系统的版本号" "区里的值对得上，但内核日志没有说这一次开机写过它——过两分钟再跑一次"
elif [ -n "$up" ] && [ "$up" -lt 150 ]; then
  na "报告写下了这个系统的版本号" "区里还是上一个系统那份（$report_build），这一次的要到约 120 秒才写"
else
  ck "报告写下了这个系统的版本号" 0 \
     "开机 ${up} 秒了，区里写的是 '$report_build'，系统自己答的是 '$live_build'——编译提示将永远清不掉"
fi

echo
echo "=== 五、后台整镜像校验 ==="
# The other thing that has never run on a device. On V66 pw3-system-verify was
# denied the search on /dev/block, failed at open() and exited 1, with the
# interface up and nothing verified -- and nothing here asked, so a run of this
# script would have reported a clean device. It says what it found in a property
# rather than only in logcat, which is what makes it askable later:
# sys.pw3.system_verified is 1 when the image is what it was verified to be and
# 0 when it is not (verify/SystemVerify.cpp).
#
# There are two legitimate ways for the property to be unset, and they are not
# the same finding. /pw3-system-recheck is the record the start leaves when it
# did *not* read the whole image itself; without that file the service exits at
# once, there is nothing to verify, and an unset property is correct. With the
# file, the service waits ninety seconds after boot_completed on purpose and
# then reads 512 MiB at priority 19 with idle I/O, so the answer is not there
# before about three minutes of uptime.
# Its logcat lines, found without relying on the tag: InitLogging(argv) leaves
# the tag as the program's name, which is an inference about liblog rather than
# something this project sets, so the message's own words are searched too.
logcat_verify_() { sh_ 'logcat -d 2>/dev/null' | grep -E 'pw3-system-verify|the system image' | tail -3; }
verified=$(sh_ 'getprop sys.pw3.system_verified')
recheck=$(sh_ '[ -e /pw3-system-recheck ] && echo yes || echo no')
up=$(sh_ 'cat /proc/uptime' | cut -d. -f1)
case "$verified" in
  1) ck "后台整镜像校验的结论" 1 "" ;;
  0) ck "后台整镜像校验的结论" 0 "镜像与安装时校验过的不一致——下一次启动会拒绝它，不要再装东西，先把这一行连设备日志一起留下" ;;
  *) if [ "$recheck" = no ]; then
       na "后台整镜像校验的结论" "这次启动自己通读过镜像（没有 /pw3-system-recheck），没有后台校验要做，属性不设是对的"
     elif [ -n "$up" ] && [ "$up" -lt 180 ]; then
       na "后台整镜像校验的结论" "开机才 ${up} 秒，这个服务在 boot_completed 之后先等 90 秒再读 512 MiB——过三分钟再跑一次这个脚本"
     elif [ -n "$(logcat_verify_)" ]; then
       ck "后台整镜像校验的结论" 0 "开机 ${up} 秒了属性还没有，而它自己说了话：$(logcat_verify_ | tail -1)"
     else
       ck "后台整镜像校验的结论" 0 "开机 ${up} 秒了，有 /pw3-system-recheck 要校验，属性却没有设，logcat 里也没有它的行——服务没跑起来，或者跑了却被挡住（V66 就是被 SELinux 挡在 /dev/block 外、退出 1）"
     fi ;;
esac

echo
echo "=== 六、PID 1 的日志通道 ==="
# CONFIG_LOG_BUF_SHIFT=15: the kernel log is 32 KB, so the lines PID 1 wrote
# between about 8 and 28 seconds are gone a minute or so into the run. Asking
# for them later and calling their absence a failure is wrong -- the earliest
# timestamp still in the buffer says whether the question can be asked at all.
klog=$(klog_)
contract=$(echo "$klog" | grep -c "pw3-preinit")
if [ "${contract:-0}" -gt 0 ]; then
  ck "内核日志里有 pw3-preinit 的行" 1 "$contract 行"
elif klog_covers_boot; then
  ck "内核日志里有 pw3-preinit 的行" 0 "0 行，而缓冲还能回到第 ${KLOG_FROM} 秒——所以是真的没有"
else
  na "内核日志里有 pw3-preinit 的行" \
     "问不了：缓冲最早只到第 ${KLOG_FROM} 秒。要验这一项得在开机后一分钟内跑，或者看 Kindle 盘上 android/logs/ 里交回来的那份"
fi
warn=$(echo "$klog" | grep -c "unable to open an initial console")
if [ "${warn:-1}" != 0 ]; then
  ck "内核不再报 'unable to open an initial console'" 0 "$warn 条"
elif klog_covers_boot; then
  ck "内核不再报 'unable to open an initial console'" 1 ""
else
  na "内核不再报 'unable to open an initial console'" "问不了：那一行在开机那段，已经被绕掉了"
fi
# PID 1 belongs to root and its fd directory is 0500, so the shell sees nothing
# (see the note on default.prop).
if [ "$ROOT" != yes ]; then
  na "init 的 0/1/2 都指向 selinux/null（没有悬空给后来的 open 占）" \
     "要 root；这一条在发行版上没有别的地方验，只能在开发构建上看（AOSP 的 /init 自己在 InitKernelLogging 里把 0/1/2 指过去，这里核的是 PID 1 没有留下悬空的）"
else
  fds=$(root_sh_ 'ls -l /proc/1/fd/0 /proc/1/fd/1 /proc/1/fd/2 2>/dev/null' | grep -c 'selinux/null')
  ck "init 的 0/1/2 都指向 selinux/null（没有悬空给后来的 open 占）" \
     "$([ "${fds:-0}" = 3 ] && echo 1 || echo 0)" "$fds / 3"
fi

echo
echo "=== 七、/system 里的代码仍然全是 speed 编译的 ==="
# dumpsys wraps long lines, and the wrap can fall inside the word `speed`. The
# previous version rejoined on a six-space continuation and reported eleven
# failures that were one truncated word each. Removing every space and newline
# rejoins whatever the wrap split; the value ends at the `]`, so nothing runs on
# into the next token.
dex=$(sh_ 'dumpsys package dexopt' | tr -d ' \n\r')
sys_total=$(printf '%s' "$dex" | grep -oE '/system/[^[]*\.odex\[status=kOatUpToDate,compilation_filter=[a-z-]+' | wc -l | tr -d ' ')
sys_other=$(printf '%s' "$dex" | grep -oE '/system/[^[]*\.odex\[status=kOatUpToDate,compilation_filter=[a-z-]+' | grep -vc 'compilation_filter=speed$')
ck "/system 里有编译记录的代码文件数 > 0" "$([ "${sys_total:-0}" -gt 0 ] && echo 1 || echo 0)" "$sys_total 份"
ck "/system 里没有一份是非 speed 的" "$([ "${sys_other:-1}" = 0 ] && echo 1 || echo 0)" "$sys_other 份"
# /data apps are a different question: what they carry depends on when they were
# installed, so this reports rather than judges.
echo "     /data 里的（装的时候是什么就是什么）："
printf '%s' "$dex" | grep -oE '/data/[^[]*\.odex\[status=kOatUpToDate,compilation_filter=[a-z-]+' |
  sed 's|.*/\([^/]*\)/oat/.*compilation_filter=|     \1  →  |' | sort -u
cache=$(printf '%s' "$dex" | grep -c '/data/dalvik-cache')
ck "没有一份来自 /data/dalvik-cache" "$([ "${cache:-1}" = 0 ] && echo 1 || echo 0)" "$cache 份"

echo
echo "=== 八、UsageStatsService 没有在刷「时间变了」 ==="
tc=$(sh_ 'logcat -d -t 2000 | grep -c "Time changed in UsageStats"')
ck "最近 2000 行日志里没有这条" "$([ "${tc:-1}" = 0 ] && echo 1 || echo 0)" "$tc 条"

echo
echo "=== 九、首页与输入法 ==="
ime=$(sh_ 'ime list -s' | head -1)
case "$ime" in *inputmethod.pinyin*) ime_ok=1 ;; *) ime_ok=0 ;; esac
ck "选中的输入法是拼音" "$ime_ok" "$ime"
home=$(sh_ 'dumpsys activity activities | grep -c "com.android.launcher3/.Launcher"')
ck "桌面在跑" "$([ "${home:-0}" -gt 0 ] && echo 1 || echo 0)" "$home"

echo
# 这一节是 V80 教的。那一版上任何应用都装不上——adb install、系统的安装器、
# 微信读书那一格，全都停在同一步（PackageInstaller 声明大小之后
# readlink /proc/self/fd 被拒，`patches/android-core/1055-...`）。而装不上这件事
# 在任何别的地方都看不出来：桌面、输入法、刷新、日志全是正常的，上面九节也全过。
# 所以把它变成一条能自己跑的检查：建一个安装会话、写 4 KB 进去、立刻放弃。
# 不需要 root，不装任何东西，放弃之后设备上什么都不留。
echo "=== 十、安装通道还通不通 ==="
sess=$(sh_ 'cmd package install-create -r' | sed -n 's/.*\[\([0-9]*\)\].*/\1/p')
if [ -z "$sess" ]; then
  na "建不出安装会话" "cmd package install-create 没有给出会话号"
else
  wr=$(sh_ "head -c 4096 /dev/zero | cmd package install-write -S 4096 $sess base - 2>&1")
  sh_ "cmd package install-abandon $sess" >/dev/null
  case "$wr" in
    Success*) ck "能往安装会话里写（声明大小的那条路）" 1 ;;
    *) ck "能往安装会话里写（声明大小的那条路）" 0 "$wr" ;;
  esac
fi

echo
# 同一个教训的推广：装不上应用是"一条通道整条不通，而别处全是正常的"。
# 这一节走另外三条使用者真的要用、而且坏了同样看不出来的通道。都不占屏幕；
# 唯一的写操作是在 /sdcard/Download 下建一个一看就知道是什么的临时文件，
# 读回来再删掉。
echo "=== 十一、另外三条通道 ==="
# 1. 闹钟。AlarmManager 设不上 RTC 告警，后台任务和唤醒就都不成立，而前台
#    用起来一切正常。有没有在排的告警，是这条通道通不通最直接的证据。
alarms=$(sh_ 'dumpsys alarm | grep -cE "RTC_WAKEUP|ELAPSED_WAKEUP"')
ck "AlarmManager 里有在排的告警" "$([ "${alarms:-0}" -gt 0 ] && echo 1 || echo 0)" "$alarms 条"
# 2. 使用者的存储。浏览器下载、文件应用、MTP 看到的都是这一个目录，它走的是
#    FUSE/sdcardfs 那条路，和应用私有目录不是一回事。写在 /sdcard 根下而不是
#    Download 下：后者要等下载管理器建出来，不一定在。
store=$(sh_ 'echo verify-device-temp > /sdcard/verify-device-check.tmp 2>&1 && cat /sdcard/verify-device-check.tmp 2>&1; rm -f /sdcard/verify-device-check.tmp 2>&1')
ck "使用者存储能写能读" "$([ "$store" = "verify-device-temp" ] && echo 1 || echo 0)" "$store"
# 3. 时钟。差太多的话 HTTPS 直接失败，而设备上只表现为"下载不动"。
dev_epoch=$(sh_ 'date +%s')
host_epoch=$(date +%s)
if [ -n "$dev_epoch" ] 2>/dev/null && [ "$dev_epoch" -gt 0 ] 2>/dev/null; then
  skew=$(( dev_epoch > host_epoch ? dev_epoch - host_epoch : host_epoch - dev_epoch ))
  ck "设备时钟与本机相差在 5 分钟内" "$([ "$skew" -le 300 ] && echo 1 || echo 0)" "差 $skew 秒"
else
  na "读不到设备时钟" "$dev_epoch"
fi

echo
echo "=== 十二、装一个应用要多久（唯一真正的验收，需要你动手）==="
cat <<'EOF'
     属性对了只说明设置生效了，不说明体验变了。真正的验收是装一个 APK：

       1. 用「文件」应用打开一个 APK，点安装；
       2. 看它要多久（微信读书这种 127 MB 的，预期一两分钟，不是二十分钟）；
       3. 看进度条下那行字是不是「正在为本机准备应用…」，下一行「较大的应用可能要几分钟。」；
       4. 装完看首页有没有自动出现它的图标。

     tools/measure-apk-install.sh 把这四件事量成数而不是印象。它用 su 0 提权，
     不要用 adb root：之后 adb devices 会变空，先查别的端口（-P 5037、-P 5038、
     -P 5097），不要先拔插（docs/known-issues.md 第三节）。
EOF

echo
printf '通过 %d 项，失败 %d 项' "$pass" "$fail"
[ "$unknown" -gt 0 ] && printf '，%d 项没能查（上面标 -- 的）' "$unknown"
echo
[ "$fail" = 0 ]
