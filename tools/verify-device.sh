#!/bin/bash
# What this project changed, read off the running device. It does not change
# power state and it leaves nothing behind -- but it is not read-only, and
# saying it was is how a reader would have found out the hard way. Two sections
# write, both named at the place they do it: section ten creates an install
# session, writes 4 KB into it and abandons it, and section eleven writes one
# file under /sdcard and deletes it again. One section takes the screen:
# section twelve force-stops WeRead, cold-starts it, waits for its first frame
# (up to two minutes) and goes back to the home screen -- don't touch the device
# while it runs. Nothing else writes anywhere or takes the screen (2026-11-03:
# the header claimed "reads only"; 2026-11-04: section twelve added after the
# script reported "0 failed" for ten hours on a device where WeRead crashed on
# every start).
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
# A fourth outcome, because na() had become a bin for two different things: a
# question that could not be answered (which must stop a release) and a question
# that this image cannot answer by design and that says where to answer it
# instead (which must not). Mixing them meant a healthy `user` image -- where
# six readings need root that a release build has not got -- could never reach
# exit 0, so the acceptance gate asking for exit 0 was a gate nothing could
# pass (2026-11-03).
skipped=0
skip() { echo "~~   $1${2:+  ($2)}"; skipped=$((skipped+1)); }

# "Is this decimal string a number no greater than $2", answered for every
# input -- including the ones the shell's own integer arithmetic cannot hold.
#
# `[ "$v" -gt N ] 2>/dev/null` inside an `||` chain was the shape, and it fails
# the wrong way twice. Measured on this host, 2026-11-03 (found by review):
#
#   android=9223372036854775808   `[` exits 2, the `||` reads that as "not
#                                 greater", the value is accepted as a baseline
#   a device clock of 01800000000 $(( )) reads the leading zero as octal, hits
#                                 an 8, and the whole `ck` call never runs --
#                                 the check leaves no line and the run exits 0
#
# Length instead of arithmetic: with the leading zeros gone, a shorter decimal
# string is a smaller number, so only equal lengths need comparing, and those
# are inside the cap's own width. dec_ exists because a validated value still
# has to lose its leading zeros before any `$(( ))` sees it.
dec_() { local v=${1#"${1%%[!0]*}"}; printf '%s' "${v:-0}"; }
num_in_range_() {
  local n
  case "$1" in ''|*[!0-9]*) return 1 ;; esac
  n=$(dec_ "$1")
  [ ${#n} -lt ${#2} ] && return 0
  [ ${#n} -gt ${#2} ] && return 1
  [ "$n" -le "$2" ]
}

# Asking the device is itself three outcomes, and this used to be two.
#
# `adb_ shell "$@" 2>/dev/null | tr -d '\r'` threw away the remote command's
# exit status -- the pipeline's status is tr's -- and the reason with it. Every
# negative check below then read a failed query as a clean zero and counted it
# a pass. Both of these were run against the original lines on 2026-11-03:
#
#   /system/build.prop unreadable  -> "build.prop 没有覆盖它"        ok
#   the kernel log unreadable      -> "除 su 外没有 permissive=1"    ok
#
# The status is carried by a sentinel the remote shell prints, not by adb's own
# exit code, because that would make this depend on the shell protocol
# propagating it -- and a script that has just failed to reach a device is in no
# position to check that assumption. The sentinel separates all three on its
# own: present and zero, the output is an answer; present and non-zero, the
# command ran and failed, so the output is not an answer; absent, the question
# never reached the device.
#
# The status goes through a file because `x=$(sh_ ...)` runs sh_ in a subshell,
# so a variable set there never reaches the caller -- and every call site here
# is a command substitution or a pipeline.
SH_UNREACHABLE=255
# The marker is random per run. A fixed string can be impersonated by the
# answer itself: a command whose output ends in the marker, followed by the
# connection dropping before the real one arrives, reads as a success that
# never happened (2026-11-03, found by review). A value the device has no way
# to know cannot be produced by accident.
SH_MARK="__rc_$(od -An -N6 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n')="
[ "$SH_MARK" = "__rc_=" ] && SH_MARK="__rc_$$_$(date +%s)="
SH_STATE=$(mktemp "${TMPDIR:-/tmp}/verify-device-status.XXXXXX")
trap 'rm -f "$SH_STATE"' EXIT
sh_() {
  local raw rc
  # A newline before the closing paren so a command that ends in `;` is still
  # one command, and a subshell so nothing a question does leaks into the next.
  raw=$(adb_ shell "( $*
) ; echo \"$SH_MARK\$?\"" 2>/dev/null | tr -d '\r')
  case "$raw" in
    *"$SH_MARK"*) rc=${raw##*"$SH_MARK"}; raw=${raw%"$SH_MARK"*} ;;
    *)            rc=$SH_UNREACHABLE ;;
  esac
  case "$rc" in ''|*[!0-9]*) rc=$SH_UNREACHABLE ;; esac
  printf '%s' "$rc" > "$SH_STATE"
  printf '%s' "$raw"
}
# The status of the most recent sh_, including through a command substitution.
sh_status_() { cat "$SH_STATE" 2>/dev/null || printf '%s' "$SH_UNREACHABLE"; }
# True when that query cannot be trusted to have answered. Used to pick na()
# over ck(), so that "could not look" stops being reported as "looked and it is
# right".
sh_failed_() { [ "${1:-$(sh_status_)}" != 0 ]; }
# A check whose answer came from a question that may not have been answered.
# `ckq <name> <status of the query> <0|1> [detail]` -- the status decides which
# of the three outcomes this is, so that every call site stops having to
# remember. A query that failed is na(): not ok, because nothing was learned,
# and not FAIL either, because a device nobody could ask is not a device that is
# wrong. Reporting it as FAIL is the mistake that sent people after the device
# on 2026-10-21 when the problem was that six files were root-only.
ckq() { if sh_failed_ "$2"; then na "$1" "$(sh_why_ "$2")"; else ck "$1" "$3" "${4:-}"; fi; }
# Why, in the words of the thing that failed, for the na() reason field. Takes a
# status saved earlier, because a check that asks two questions has already
# overwritten the live one by the time it reports.
sh_why_() {
  local rc=${1:-$(sh_status_)}
  case "$rc" in
    "$SH_UNREACHABLE") printf '问不到设备' ;;
    *) printf '设备上的命令以 %s 退出，输出不是答案' "$rc" ;;
  esac
}

echo "=== 一、设备在不在 ==="
devs=$(adb_ devices | sed -n '2p')
# The state field, not just "there is a line". `offline` and `unauthorized` are
# lines too, and a device in either state answers nothing -- so every question
# below would have come back empty with this check already reporting ok
# (2026-11-03, found by review).
dev_state=$(printf '%s' "$devs" | awk '{print $2}')
if [ -n "$devs" ] && [ "$dev_state" != device ]; then
  echo "FAIL adb 看到设备，但它的状态是 '$dev_state'，这种状态下它什么都不答"
  case "$dev_state" in
    unauthorized) echo "     设备屏幕上应该有一个「允许 USB 调试」的框，点它" ;;
    offline) echo "     拔插一次，或者等它启动完；也可能是 adb 服务和设备的版本对不上" ;;
  esac
  exit 1
fi
if [ -z "$devs" ]; then
  echo "FAIL 这个 adb 服务看不到设备"
  # The one that has cost the most time: another adb server on this machine is
  # holding the device. Only one server can have it, and a reboot drops it off
  # USB without the server that had it taking it back -- so after a reboot it
  # turns up on whichever server asks next.
  #
  # Asked by enumerating the servers that are actually running, not by walking a
  # list of ports. A fixed list is a list the next reboot walks outside of: on
  # 2026-11-03 the device was on 5040, the list held 5097 and 5038, and "the
  # ports I know are all empty" was reported as "the device is not there". Those
  # are two different states and this used to run them together.
  # ps rather than pgrep: on macOS `pgrep -af` prints only pids, so the sed
  # below matched nothing and this reported "the device is not there" with the
  # device sitting on a server two lines away. `pgrep -fl` prints the command
  # here but not on Linux, where -l means the name only. `ps -A -o args=` says
  # the same thing on both (2026-11-03, found by running it).
  for port in $(ps -A -o args= 2>/dev/null | grep '[a]db' |
                sed -n 's/.*-L tcp:\([0-9][0-9]*\).*/\1/p' | sort -un); do
    [ "$port" = "${ADB_PORT:-5037}" ] && continue
    if [ -n "$("$ADB_BIN" -P "$port" devices 2>/dev/null | sed -n '2p')" ]; then
      echo "     端口 $port 上的 adb 服务有它。重跑："
      echo "         ADB_PORT=$port $0"
      exit 1
    fi
  done
  # And if no server has it, one of ours would claim it -- which separates "no
  # server is holding it" from "it is not on the bus".
  probe=5199
  if [ -n "$("$ADB_BIN" -P "$probe" devices 2>/dev/null | sed -n '2p')" ]; then
    echo "     本机原来的 adb 服务都没有它，我在端口 $probe 上新起的服务拿到了。重跑："
    echo "         ADB_PORT=$probe $0"
    exit 1
  fi
  "$ADB_BIN" -P "$probe" kill-server >/dev/null 2>&1
  echo "     本机在跑的 adb 服务都没有它，新起一个也拿不到——线插着吗？Android 起来了吗？"
  echo "     （Android 模式下它在 USB 总线上叫 ro.product.model 那个名字，不是 Amazon Kindle，"
  echo "      所以拿 'Amazon Kindle' 去 grep ioreg 会把插着的设备判成没插 —— 2026-11-03）"
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
# Three answers, not two. "There is no root here" is a property of a release
# build and the readings that need it are then skipped by design; "the question
# did not get through" is not, and the readings that need it are then unknown.
# Told apart they report differently; run together, a run that reached nothing
# at all would claim six checks were skipped on purpose.
ROOT_PREFIX=""
uid=$(sh_ 'id -u'); uid_rc=$(sh_status_)
if sh_failed_ "$uid_rc"; then
  ROOT="unknown"
elif [ "$uid" = 0 ]; then
  ROOT="yes"
else
  # A `su` that fails is an answer, not a failure to answer: a release build has
  # no su at all, so the shell replies "not found" and that is exactly how this
  # image says there is no root here. `id` failing is different -- id is always
  # there -- which is why only that one above makes this unknown.
  su_uid=$(sh_ 'su 0 id -u'); su_rc=$(sh_status_)
  if [ "$su_uid" = 0 ] && ! sh_failed_ "$su_rc"; then
    ROOT="yes"; ROOT_PREFIX="su 0 "
  elif [ "$su_rc" = "$SH_UNREACHABLE" ]; then
    # The question did not get through. That is not this image saying it has no
    # su -- and calling it so would hand six checks a "skipped by design" they
    # have not earned (2026-11-03, found by review).
    ROOT="unknown"; uid_rc=$su_rc
  else
    # su answered and did not give root. That means "this image has no root by
    # design" only when the image says it is a release build: a non-debuggable
    # user build. Anything else is an image that should have su and does not
    # work -- su missing (the shell's 127) on a userdebug build included -- and
    # calling it a release build would turn every root-only reading into a skip
    # that does not hold the release (2026-11-04, Codex round three, item 6,
    # and the 127 arm that still skipped the build type: the 11-04 review,
    # item 3).
    btype=$(sh_ 'getprop ro.build.type'); btype_rc=$(sh_status_)
    dbg=$(sh_ 'getprop ro.debuggable'); dbg_rc=$(sh_status_)
    if ! sh_failed_ "$btype_rc" && ! sh_failed_ "$dbg_rc" \
         && [ "$btype" = user ] && [ "$dbg" = 0 ]; then
      ROOT="no"
    else
      ROOT="unknown"; uid_rc=$su_rc
    fi
  fi
fi
root_sh_() { sh_ "$ROOT_PREFIX$1"; }
case "$ROOT" in
  yes) echo "     提权：可以${ROOT_PREFIX:+（su 0）}" ;;
  no)  echo "     提权：不行（发行构建没有 su），要 root 的几项会按设计跳过" ;;
  *)   echo "     提权：问不出来，要 root 的几项会记成没能查" ;;
esac

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
  got=$(sh_ "getprop $prop"); rc=$(sh_status_)
  ckq "$prop = $want" "$rc" "$([ "$got" = "$want" ] && echo 1 || echo 0)" "读到 '$got'"
done
# What the reader sees when they tap an application: with the starting window
# on, the system draws the target activity's own windowBackground at once --
# for the application this device is for, its own logo page -- instead of the
# previous screen staying put for the twenty to thirty seconds its first frame
# takes (frameworks/base 0068, pw3.mk).
start_ro=$(sh_ 'getprop ro.pw3.disable_starting_window'); start_rc=$(sh_status_)
ckq "启动窗口是开着的（ro.pw3.disable_starting_window = false）" "$start_rc" \
    "$([ "$start_ro" = false ] && echo 1 || echo 0)" "读到 '$start_ro'"
# And nothing was left overriding anything. Every debug.pw3.* at once rather
# than the one this section is about, for two reasons found the same day
# (2026-11-03): the properties that settle behaviour on the device by hand come
# and go with the patch series, so a check that names one of them has to be kept
# in step by hand and silently stops covering the rest -- a test run had left
# debug.pw3.app_transition_timeout_ms behind and this reported no override. And
# `getprop` with no argument is one question whose status is its own, where
# `getprop X | grep Y` hands back grep's status, in which "nothing matched" and
# "the read failed" are the same non-zero. Filtering here rather than on the
# device is what makes the three outcomes separable at all.
props=$(sh_ 'getprop'); props_rc=$(sh_status_)
# A property whose value is empty is not an override: property_get_bool treats
# it as unset, and clearing one is how a test restores the shipping default
# (which is why the restore writes "" and not false). The first version of this
# matched any value including the empty one, so it reported an override on
# exactly the state a tidy test leaves behind. The timeout's 0 is the same
# story: patch 0069 uses the built-in deadline unless ms > 0.
left=$(printf '%s\n' "$props" |
  sed -n 's/^\[\(debug\.pw3\.[^]]*\)\]: \[\(..*\)\]$/\1=\2/p' |
  grep -v '^debug\.pw3\.app_transition_timeout_ms=0$')
ckq "没有留下任何 debug.pw3.* 覆盖" "$props_rc" \
    "$([ -z "$left" ] && echo 1 || echo 0)" \
    "$([ -z "$left" ] && echo "一项都没有" || printf '%s' "$(printf '%s' "$left" | tr '\n' ' ')")"
# default.prop is where this one lives; build.prop is read later and would win.
# Both files are 0600 root-only in this build, so the unprivileged shell gets
# "Permission denied" and the count comes back empty. Read as root, and say "要
# root" rather than calling an unreadable file a wrong one: on 2026-10-21 this
# and five more checks below reported a device that was in fact correct.
if [ "$ROOT" = unknown ]; then
  na "default.prop 里写的就是 quicken，而 build.prop 没有覆盖它" "$(sh_why_ "$uid_rc")"
elif [ "$ROOT" != yes ]; then
  skip "default.prop 里写的就是 quicken，而 build.prop 没有覆盖它" \
     "要 root（两个文件都是 0600）；生效值上面四项已经用 getprop 验过了，这一条只多验一层来历，要看来历就在构建机上看那个镜像"
else
  dp=$(root_sh_ 'grep -c "^pm.dexopt.install=quicken" /default.prop'); dp_rc=$(sh_status_)
  bp=$(root_sh_ 'grep -c "^pm.dexopt.install" /system/build.prop'); bp_rc=$(sh_status_)
  # `grep -c` exits 1 on no match, so only "could not ask at all" makes this
  # unknown. Without this, an unreadable build.prop came back empty, `${bp:-0}`
  # called that zero overrides, and the check reported ok -- the very reading
  # this section is about (2026-11-03, found by review).
  if [ "$dp_rc" = "$SH_UNREACHABLE" ] || [ "$bp_rc" = "$SH_UNREACHABLE" ] ||
     [ "$dp_rc" -gt 1 ] 2>/dev/null || [ "$bp_rc" -gt 1 ] 2>/dev/null; then
    na "default.prop 里写的就是 quicken，而 build.prop 没有覆盖它" \
       "读 /default.prop 或 /system/build.prop 没成功（状态 $dp_rc / ${bp_rc}）"
  else
    ck "default.prop 里写的就是 quicken，而 build.prop 没有覆盖它" \
       "$([ "$dp" = 1 ] && [ "${bp:-0}" = 0 ] && echo 1 || echo 0)" "default.prop $dp 行, build.prop $bp 行"
  fi
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
# Both logs read once, with their statuses. A failed read turned into zero
# matches and the reach of the buffer, from an earlier successful query, then
# vouched for it, so "no denial was recorded" was reported on a log nobody
# could read (2026-11-03, found by review).
avc_k=$(klog_); avc_k_rc=$(sh_status_)
avc_m=$(sh_ 'logcat -d'); avc_m_rc=$(sh_status_)
own_k=$(printf '%s\n' "$avc_k" | grep 'avc:' | grep -c 'scontext=u:r:pw3_')
own_m=$(printf '%s\n' "$avc_m" | grep 'avc: denied' | grep -c 'scontext=u:r:pw3_')
own=$(( ${own_k:-0} + ${own_m:-0} ))
if sh_failed_ "$avc_k_rc" || sh_failed_ "$avc_m_rc"; then
  na "没有 pw3_* 域的拒绝" "读内核日志或主日志没成功（状态 $avc_k_rc / ${avc_m_rc}）"
elif [ "$own" != 0 ]; then
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
enf=$(sh_ 'getenforce'); enf_rc=$(sh_status_)
ckq "SELinux 是 Enforcing" "$enf_rc" "$([ "$enf" = Enforcing ] && echo 1 || echo 0)" "读到 '$enf'"
# Not "none at all": on a userdebug build `su` is permissive by design, the only
# one (checked against the built policy with sepolicy-analyze), and taking root
# at all produces that line. A `user` build has no su, so there the count is a
# plain zero.
# The log is read once and filtered here, so that the status belongs to the read
# of the device rather than to grep: with the kernel log unreadable the original
# line reported "nothing was permissive" (2026-11-03).
klog=$(klog_); klog_rc=$(sh_status_)
perm=$(printf '%s\n' "$klog" | grep "permissive=1" | grep -vc 'scontext=u:r:su:')
ckq "除 su（userdebug 构建里唯一的 permissive 域）外没有 permissive=1" "$klog_rc" \
    "$([ "${perm:-1}" = 0 ] && echo 1 || echo 0)" "$perm 条"

echo
echo "=== 四、存储映射与诊断区 ==="
# The question is asked so that the remote command succeeds whenever it was
# able to look, and the finding is in the output. `ls` on a path that is not
# there exits non-zero, and a non-zero exit is how this script now says "the
# question did not get through" -- so a node that genuinely does not exist was
# being reported as "could not look" instead of as the failure it is
# (2026-11-03, found by review).
node=$(sh_ 'ls -ld /dev/block/dm-3 2>/dev/null || echo 没有这个节点'); node_rc=$(sh_status_)
ckq "/dev/block/dm-3 在，而且是块设备" "$node_rc" \
    "$([ "$(printf '%s' "$node" | cut -c1)" = b ] && echo 1 || echo 0)" "读到 '$node'"
# Which mapping is mounted where, and read-only or not. This is the cross-link
# question -- the one PID 1 refuses to start on -- and /proc/mounts answers it
# without root, so it is asked on a release build too. Three lines, because dm-3
# is the report area and carries no filesystem.
mounts=$(sh_ 'cat /proc/mounts'); mounts_rc=$(sh_status_)
for want in "dm-0 /pw3-user vfat ro" "dm-1 /system ext4 ro" "dm-2 /data ext4 rw"; do
  set -- $want
  line=$(echo "$mounts" | grep "^/dev/block/$1 ")
  at=$(echo "$line" | cut -d' ' -f2); kind=$(echo "$line" | cut -d' ' -f3)
  mode=$(echo "$line" | cut -d' ' -f4 | cut -d, -f1)
  ok=1; [ "$at" = "$2" ] || ok=0; [ "$kind" = "$3" ] || ok=0; [ "$mode" = "$4" ] || ok=0
  ckq "$1 挂在 $2，$3，$4" "$mounts_rc" "$ok" "读到 '${at:-没挂} ${kind} ${mode}'"
done
# And the device's own read-only flag, which is the stronger statement: a mount
# flag can be remounted, this one cannot. /sys/class/block/dm-*/ro is not
# readable by the shell domain, so only this half needs root.
for pair in "0 0" "1 1" "2 0" "3 0"; do
  n=${pair%% *}; want=${pair##* }
  if [ "$ROOT" = unknown ]; then
    na "dm-$n 的设备级 ro = $want" "$(sh_why_ "$uid_rc")"
    continue
  fi
  if [ "$ROOT" != yes ]; then
    skip "dm-$n 的设备级 ro = $want" \
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
# The name is the question and is the same on every arm. Two of these used to
# say "诊断区这一次写成了没有", so the same check was reported under two names
# depending on how it came out, and anyone -- or anything -- looking for the
# question found only the arms that had an answer (2026-11-03).
report_klog=$(klog_); report_klog_rc=$(sh_status_)
line=$(printf '%s\n' "$report_klog" | grep "pw3-report-area:" | tail -1)
up=$(sh_ 'cat /proc/uptime'); up_rc=$(sh_status_); up=$(printf '%s' "$up" | cut -d. -f1)
if sh_failed_ "$report_klog_rc"; then
  na "诊断区这一次写成了" "$(sh_why_ "$report_klog_rc")"
else
case "$line" in
  *written*)    ck "诊断区这一次写成了" 1 ""; wrote_this_boot=yes ;;
  *incomplete*) ck "诊断区这一次写成了" 0 "写了一半：$line" ;;
  *"not built"*) ck "诊断区这一次写成了" 0 "报告装不进去：$line" ;;
  *unusable*)   ck "诊断区这一次写成了" 0 "拿不到那个设备节点：$line" ;;
  *"cannot be opened"*) ck "诊断区这一次写成了" 0 "打不开：$line" ;;
  *)            if [ -n "$line" ]; then
                  ck "诊断区这一次写成了" 0 "$line"
                elif [ -n "$up" ] && [ "$up" -lt 150 ]; then
                  na "诊断区这一次写成了" "开机才 ${up} 秒，pw3-boot-report 要采样到约 120 秒才写——过两分钟再跑一次这个脚本"
                elif klog_covers_boot; then
                  ck "诊断区这一次写成了" 0 "开机 ${up} 秒了，日志里一个字都没说，而缓冲还能回到第 ${KLOG_FROM} 秒——pw3-boot-report 没跑，或者跑了却什么都没说"
                else
                  na "诊断区这一次写成了" "日志里没有这一行，而缓冲最早只到第 ${KLOG_FROM} 秒"
                fi ;;
esac
fi
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
state_schema=$(root_sh_ 'grep -c "^schema=1$" /pw3-user/android/state 2>/dev/null'); schema_rc=$(sh_status_)
state_line=$(root_sh_ 'grep "^android=" /pw3-user/android/state 2>/dev/null'); line_rc=$(sh_status_)
state_line=$(printf '%s\n' "$state_line" | tail -1)
if [ "$ROOT" = unknown ]; then
  na "安装器记下了它处理到哪一份报告" "$(sh_why_ "$uid_rc")"
elif [ "$ROOT" != yes ]; then
  skip "安装器记下了它处理到哪一份报告" \
     "要 root 才读得到 /pw3-user；发行版上这一条从 Kindle 盘读：接电脑看 android/state 里的 schema= 和 android= 两行"
# grep answers 1 for "no such line", which is an answer, and 2 for "could not
# read the file", which is not. Excluding only 255 let a read error through as
# "the line is missing" and blamed the installer for it (2026-11-03 review).
elif [ "$schema_rc" != 0 ] && [ "$schema_rc" != 1 ]; then
  na "安装器记下了它处理到哪一份报告" "$(sh_why_ "$schema_rc")"
elif [ "$line_rc" != 0 ] && [ "$line_rc" != 1 ]; then
  na "安装器记下了它处理到哪一份报告" "$(sh_why_ "$line_rc")"
elif [ "${state_schema:-0}" = 0 ]; then
  # Not a judgement: the test entry hands over without an installer at all, and
  # then there is no state file to have a line in.
  na "安装器记下了它处理到哪一份报告" "/pw3-user/android/state 读不到或不是安装器写的"
elif [ -z "$state_line" ]; then
  ck "安装器记下了它处理到哪一份报告" 0 \
     "状态文件里没有 android= 这一行：写它的安装器还不认报告区，第七、八节那两个缺陷在这台机器上还在"
# A value, not just a line. The installer needs it to parse as a number no
# greater than 0xffffffff before it treats the baseline as known
# (installer/install/install.c:242), so a truncated `android=` is a line this
# check accepted and the installer does not (2026-11-03, found by review).
# Read the way the installer reads it: sscanf("android=%llu") takes the leading
# digits and reads past whatever follows, and older states carry a ` seen=`
# there on purpose (install.c, the comment at that sscanf). Demanding that the
# whole rest be a number failed a state the installer accepts and said the
# installer would not (11-04 review, item 11).
# %llu also skips leading white space and takes a leading `+` (round two,
# item 9); a `-` it would take too, and wrap, which the installer's own range
# test does not catch either -- so that one stays refused here.
elif ! num_in_range_ "$(printf '%s' "${state_line#android=}" | sed -n 's/^[[:space:]]*+\{0,1\}\([0-9][0-9]*\).*/\1/p')" 4294967295; then
  ck "安装器记下了它处理到哪一份报告" 0 \
     "android= 后面不是安装器能用的数字（读到 '$state_line'）——安装器会当成基线未知"
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
# Each read carries its own status. An empty magic used to fall straight past
# the arm that rejects a wrong one -- that arm needs it non-empty -- and could
# then reach "ok" on the strength of the other read of the same file
# (2026-11-03, found by review).
report_magic=$(root_sh_ 'head -c 13 /pw3-user/android/report.bin 2>/dev/null'); magic_rc=$(sh_status_)
# The status has to be the read's. Piped into `tail -1` it was tail's, which is
# 0 whatever happened before it, so an unreadable report area came back as a
# report with no version in it (2026-11-03 review). So each read is the whole
# command and the picking happens here; the root prefix covers only the first
# command of a line, which is the other reason not to build a pipeline over there.
#
# And only the body the header declares. The area is a fixed 64 KB and a report
# is a few KB (4082 bytes on f); the installer reads the fields of exactly
# `bytes=` bytes after the first line (ReportArea.h, pw3_report_body), so text
# past that -- a longer earlier report the writer did not clear, say -- is not
# a field of this report. `strings` over the whole file read it anyway and
# `tail -1` preferred it (11-04 review, item 8; no such remnant on f today).
#
# And only a body the installer would accept: exactly `bytes=` bytes, whose
# CRC32 is the header's (pw3_report_body). A header declaring more than was
# written passed the bounded read above with whatever text came first (11-04
# review, round two, item 5), and `bytes=044` went through shell arithmetic as
# octal 36 (item 8) -- the installer reads it digit by digit as decimal. The
# body travels as bytes (exec-out, into a file) because it is checked as bytes;
# text through `$(...)` loses NULs and trailing newlines.
report_head=$(root_sh_ 'head -n 1 /pw3-user/android/report.bin 2>/dev/null'); head_rc=$(sh_status_)
report_bytes=$(printf '%s' "$report_head" | sed -n 's/^PW3-REPORT-1 bytes=\([0-9][0-9]*\) .*/\1/p')
report_crc=$(printf '%s' "$report_head" | sed -n 's/.* crc32=\([0-9a-fA-F]\{8\}\)\( .*\)\{0,1\}$/\1/p' | tr 'A-F' 'a-f')
build_rc=$head_rc; report_build=""; body_why=""
if ! sh_failed_ "$head_rc" && num_in_range_ "$report_bytes" 65536 && [ -n "$report_crc" ]; then
  body_n=$(dec_ "$report_bytes")
  body_file=$(mktemp "${TMPDIR:-/tmp}/verify-device-report.XXXXXX")
  adb_ exec-out "${ROOT_PREFIX}tail -c +$(( ${#report_head} + 2 )) /pw3-user/android/report.bin" 2>/dev/null |
    head -c "$body_n" > "$body_file"
  body_got=$(wc -c < "$body_file" | tr -d ' ')
  body_crc=$(python3 -c 'import sys, zlib; print("%08x" % (zlib.crc32(open(sys.argv[1], "rb").read()) & 0xffffffff))' "$body_file" 2>/dev/null)
  if [ "$body_got" != "$body_n" ]; then
    build_rc=short; body_why="报告头说正文 $body_n 字节，读回来 ${body_got:-0} 字节"
  elif [ "$body_crc" != "$report_crc" ]; then
    build_rc=crc; body_why="正文的 CRC32 是 ${body_crc:-?}，报告头写的是 $report_crc——安装器不会读这份报告"
  else
    report_build=$(grep -a '^ro.build.version.incremental=' "$body_file" | tail -1 |
                   sed 's/^ro.build.version.incremental=//')
  fi
  rm -f "$body_file"
elif ! sh_failed_ "$head_rc" && [ "${report_head#PW3-REPORT-1 }" != "$report_head" ]; then
  body_why="报告头读不出合法的 bytes= 和 crc32=（读到 '$report_head'）"
fi
live_build=$(sh_ 'getprop ro.build.version.incremental'); live_rc=$(sh_status_)
up=${up:-$(sh_ 'cat /proc/uptime' | cut -d. -f1)}
if [ "$ROOT" = unknown ]; then
  na "报告写下了这个系统的版本号" "$(sh_why_ "$uid_rc")"
elif [ "$ROOT" != yes ]; then
  skip "报告写下了这个系统的版本号" \
     "要 root 才读得到报告区；发行版上这一条从 Kindle 盘读：接电脑看 android/report.bin （开头应为 PW3-REPORT-1），或看 android/logs/ 里安装器那次点击的日志"
elif [ -n "$body_why" ]; then
  na "报告写下了这个系统的版本号" "$body_why"
elif sh_failed_ "$magic_rc" || sh_failed_ "$build_rc" || sh_failed_ "$live_rc"; then
  na "报告写下了这个系统的版本号" "读报告区或读版本号没成功，所以下面每一支都不作数"
# The magic has to be right, not merely "not visibly wrong". Written as
# `non-empty and different`, thirteen NUL bytes answered as empty -- command
# substitution drops NULs -- with a successful status, so a corrupt report area
# went past this arm and was accepted two arms below (2026-11-03, found by
# review, reproduced with head -c 13 /dev/zero).
elif [ "$report_magic" != "PW3-REPORT-1 " ]; then
  na "报告写下了这个系统的版本号" "报告区开头不是 PW3-REPORT-1（读到 ${#report_magic} 个可见字节），安装器不会读它——先看上一项"
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
  na "报告写下了这个系统的版本号" "区里还是上一个系统那份（${report_build}），这一次的要到约 120 秒才写"
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
verified=$(sh_ 'getprop sys.pw3.system_verified'); verified_rc=$(sh_status_)
# The probe reports which of the three it is. `[ -e X ] && echo yes || echo no`
# wrapped "it is not there" and "I was not allowed to look" into the same `no`,
# with a zero status -- and `no` is the arm that now says "this boot read the
# whole image, nothing to verify" and does not block. Claiming that on a
# permission error is asserting something nobody checked. The installer's own
# code only treats ENOENT as nothing-to-do (verify/SystemVerify.cpp:144).
#
# The second version still asked `[ -e ]`, which is false for "not there" and
# for "not allowed to look" alike -- and on this image the shell is not allowed
# to look at some of the root directory: `ls -d /init` answers "Permission
# denied" while `ls -d /pw3-system-recheck` answers "No such file or directory"
# (measured on f, 2026-11-04). So the probe reports ls's own words, and only
# "No such file or directory" is no.
recheck=$(sh_ 'r=$(ls -d /pw3-system-recheck 2>&1); case "$r" in /pw3-system-recheck) echo yes ;; *"No such file or directory"*) echo no ;; *) echo unsure ;; esac'); recheck_rc=$(sh_status_)
up=$(sh_ 'cat /proc/uptime'); up_rc=$(sh_status_); up=$(printf '%s' "$up" | cut -d. -f1)
# The property itself answers 1 and 0 outright, and neither answer needs the
# other two questions: a 0 is an image that does not match what was verified at
# install time, which is worth reporting whatever the uptime is. Only the unset
# case is a case analysis, and only it needs the other two.
#
# The first version of this gated all three answers on all three questions, so
# an uptime read that failed turned an already-read 0 into "could not look" --
# and explained it with the status of the query that had succeeded. The arm it
# used to land on before that was the one that blames the service and names a
# SELinux denial from V66, which on a run where nothing was asked is an
# accusation rather than a finding (2026-11-03, both found by review).
if sh_failed_ "$verified_rc"; then
  na "后台整镜像校验的结论" "$(sh_why_ "$verified_rc")"
elif [ "$verified" = 1 ]; then
  ck "后台整镜像校验的结论" 1 ""
elif [ "$verified" = 0 ]; then
  ck "后台整镜像校验的结论" 0 "镜像与安装时校验过的不一致——下一次启动会拒绝它，不要再装东西，先把这一行连设备日志一起留下"
elif [ -n "$verified" ]; then
  # Set, and neither answer the service gives. Only an unset property goes on
  # to the analysis below, whose arms are all about why it is not set yet
  # (11-04 review, item 10).
  ck "后台整镜像校验的结论" 0 "属性是 '$verified'，既不是 1 也不是 0——校验服务不会写这个值"
elif sh_failed_ "$recheck_rc"; then
  na "后台整镜像校验的结论" "属性没有设，而$(sh_why_ "$recheck_rc")，所以问不出有没有后台校验要做"
elif sh_failed_ "$up_rc"; then
  na "后台整镜像校验的结论" "属性没有设，而$(sh_why_ "$up_rc")，所以不知道开机多久了、该不该等"
else
# When it is due: ninety seconds after boot_completed, plus the read at idle
# I/O priority, which a reader in front of it can stretch. Counted from boot it
# called a slow start's verifier late while it was still inside its own wait
# (round two, item 7). The boot time is in the events buffer while that is
# young enough to hold it; after that the run is long past any of this and the
# old floor of 180 s is as good as any.
booted_ms=$(sh_ "logcat -b events -d boot_progress_enable_screen:I '*:S'" | sed -n 's/.*boot_progress_enable_screen: \([0-9][0-9]*\).*/\1/p' | tail -1)
booted_s=${booted_ms:+$(( $(dec_ "$booted_ms") / 1000 ))}
if [ -n "$booted_s" ]; then verify_due=$(( booted_s + 210 )); else verify_due=180; fi
case "$verified" in
  *) if [ "$recheck" = no ]; then
       skip "后台整镜像校验的结论" "这次启动自己通读过镜像（没有 /pw3-system-recheck），没有后台校验要做，属性不设是对的"
     elif [ "$recheck" != yes ]; then
       # Not knowing whether there is anything to verify is not "the service
       # did not run": the arms below accuse the service, and they may only do
       # that when the file is known to be there.
       na "后台整镜像校验的结论" "属性没有设，而问不出有没有 /pw3-system-recheck（ls 的回答是 ${recheck:-空}），所以不知道有没有后台校验要做"
     elif [ -n "$up" ] && [ "$up" -lt "$verify_due" ]; then
       na "后台整镜像校验的结论" "开机才 ${up} 秒，这个服务在 boot_completed（${booted_s:-时间不详} 秒）之后先等 90 秒再读 512 MiB——过三分钟再跑一次这个脚本"
     elif [ -n "$(logcat_verify_)" ]; then
       ck "后台整镜像校验的结论" 0 "开机 ${up} 秒了属性还没有，而它自己说了话：$(logcat_verify_ | tail -1)"
     else
       ck "后台整镜像校验的结论" 0 "开机 ${up} 秒了，有 /pw3-system-recheck 要校验，属性却没有设，logcat 里也没有它的行——服务没跑起来，或者跑了却被挡住（V66 就是被 SELinux 挡在 /dev/block 外、退出 1）"
     fi ;;
esac
fi

echo
echo "=== 六、PID 1 的日志通道 ==="
# CONFIG_LOG_BUF_SHIFT=15: the kernel log is 32 KB, so the lines PID 1 wrote
# between about 8 and 28 seconds are gone a minute or so into the run. Asking
# for them later and calling their absence a failure is wrong -- the earliest
# timestamp still in the buffer says whether the question can be asked at all.
# Its own status, and both checks below read it. This block re-read the log and
# left klog_rc holding the status of the read in section three, so one failed
# read here produced two wrong diagnoses out of the same empty output: `FAIL
# 内核日志里有 pw3-preinit 的行 (0 行...所以是真的没有)` from a check that asked for no
# status at all, and `ok 内核不再报 'unable to open an initial console'` from a
# check that asked the stale one (2026-11-03, found by review).
klog=$(klog_); klog_rc=$(sh_status_)
contract=$(echo "$klog" | grep -c "pw3-preinit")
if sh_failed_ "$klog_rc"; then
  na "内核日志里有 pw3-preinit 的行" "$(sh_why_ "$klog_rc")"
elif [ "${contract:-0}" -gt 0 ]; then
  ck "内核日志里有 pw3-preinit 的行" 1 "$contract 行"
elif klog_covers_boot; then
  ck "内核日志里有 pw3-preinit 的行" 0 "0 行，而缓冲还能回到第 ${KLOG_FROM} 秒——所以是真的没有"
else
  na "内核日志里有 pw3-preinit 的行" \
     "问不了：缓冲最早只到第 ${KLOG_FROM} 秒。要验这一项得在开机后一分钟内跑，或者看 Kindle 盘上 android/logs/ 里交回来的那份"
fi
warn=$(printf '%s\n' "$klog" | grep -c "unable to open an initial console")
if sh_failed_ "$klog_rc"; then
  na "内核不再报 'unable to open an initial console'" "$(sh_why_ "$klog_rc")"
elif [ "${warn:-1}" != 0 ]; then
  ck "内核不再报 'unable to open an initial console'" 0 "$warn 条"
elif klog_covers_boot; then
  ck "内核不再报 'unable to open an initial console'" 1 ""
else
  na "内核不再报 'unable to open an initial console'" "问不了：那一行在开机那段，已经被绕掉了"
fi
# PID 1 belongs to root and its fd directory is 0500, so the shell sees nothing
# (see the note on default.prop).
if [ "$ROOT" = unknown ]; then
  na "init 的 0/1/2 都指向 selinux/null（没有悬空给后来的 open 占）" "$(sh_why_ "$uid_rc")"
elif [ "$ROOT" != yes ]; then
  skip "init 的 0/1/2 都指向 selinux/null（没有悬空给后来的 open 占）" \
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
dex=$(sh_ 'dumpsys package dexopt'); dex_rc=$(sh_status_)
dex=$(printf '%s' "$dex" | tr -d ' \n\r')
# Every /system record, whatever its status, and then the two questions asked of
# the same set. The previous version matched `status=kOatUpToDate` in both the
# total and the "is any of them not speed" line, so a record in any other state
# was filtered out before being judged: one good record and one
# kOatDexOutOfDate record came back 3 passed, 0 failed (2026-11-03). A state
# this does not expect is not a record to drop, it is a record to report.
# Each record up to its closing bracket, whatever is inside it. Requiring
# `compilation_filter=` dropped whole records instead of judging them: ART
# prints `vdex-only` in place of that field when only the vdex is there
# (art/runtime/oat_file_assistant.cc), so a kOatDexOutOfDate entry vanished and
# the remaining good one answered for all of them. Ending the match at `]` also
# stops a truncated `speed-profile` from reading as `speed` (2026-11-03, found
# by review).
#
# And no `[` inside the brackets either. With the newlines gone, a record whose
# `]` never arrived runs straight into the next package's `[name]` header, and
# `[^]]*\]` closed it on that header's bracket: a truncated record counted as
# complete and the completeness check below said ok (2026-11-04, found by
# feeding it a real dump with one bracket removed).
sys_all=$(printf '%s' "$dex" | grep -oE '/system/[^[]*\.odex\[[^][]*\]')
sys_total=$(printf '%s\n' "$sys_all" | grep -c '/system/')
sys_stale=$(printf '%s\n' "$sys_all" | grep -vc 'status=kOatUpToDate,')
sys_other=$(printf '%s\n' "$sys_all" | grep 'status=kOatUpToDate,' | grep -vc 'compilation_filter=speed\]$')
# And the records ART could not read at all, which carry no path and so are not
# in the set above: both files unopenable prints `invalid[]`.
sys_broken=$(printf '%s' "$dex" | grep -c 'invalid\[\]')
# Every record this output claims, against the ones that are complete. A record
# whose closing bracket never arrived simply failed to match above, so a
# truncated answer was read as a complete set of whatever did match
# (2026-11-03, found by review).
sys_claimed=$(printf '%s' "$dex" | grep -oE '/system/[^[]*\.odex\[' | grep -c .)
# And every path this output names, against the records that opened. Deleting
# the newlines rejoins a wrapped word, but it also rejoins two records, and
# `[^[]*` then runs one path into the next: a `/system/b.odex` line with no
# record of its own followed by a complete `/system/a.odex[...]` matched as one
# record and answered `开了 1 条，读完整的 1 条`, five checks ok (2026-11-03,
# found by review). Each record names /system/ once, so a record that swallowed
# a path has more of them than there are records.
#
# Once as a record, that is: every package also has a `path: /system/...apk`
# line above its records, and counting those made this check fail on every
# healthy device -- 62 paths for 31 records on f, 2026-11-04, the first time it
# was run against a real dump rather than read. The package lines are taken out
# of the count; an orphan record line is still counted, which is what this asks.
sys_named=$(( $(printf '%s' "$dex" | grep -oE '/system/' | grep -c .) -
              $(printf '%s' "$dex" | grep -oE 'path:/system/' | grep -c .) ))
# And the other way round: every /system package has a complete record. A
# package whose line under `path:` is `arm: [Exception]: ...`, or that has no
# line under it at all, carries no `.odex[` and no `invalid[]`, so none of the
# counts above ever sees it, and the other packages answer for it (11-04
# review, item 4). One block per package, cut at each `path:`.
sys_blocks=$(printf '%s' "$dex" | awk '{ gsub(/path:/, "\npath:"); print }' | grep '^path:/system/')
sys_pkgs=$(printf '%s\n' "$sys_blocks" | grep -c '^path:/system/')
sys_pkgs_ok=$(printf '%s\n' "$sys_blocks" | grep -cE '\.odex\[status=[^][]*\]')
# Everything above is counted out of one answer, so a dump cut short after a
# complete record -- the first six lines of it, say -- agrees with itself and
# passes all of it (11-04 review, round two, item 3). So the answer is held to
# a list it did not write: the odex files /system holds for its applications.
# Each of them has to have a record. On f the two sets are the same 31 paths.
odex_disk=$(sh_ 'ls /system/app/*/oat/*/*.odex /system/priv-app/*/oat/*/*.odex'); odex_rc=$(sh_status_)
odex_disk=$(printf '%s\n' "$odex_disk" | grep '^/system/' | sort -u)
odex_disk_n=$(printf '%s\n' "$odex_disk" | grep -c '^/system/')
odex_in_dump=$(printf '%s' "$dex" | grep -oE '/system/[^]:[]*\.odex\[' | sed 's/\[$//' | sort -u)
odex_unrecorded=$(comm -23 <(printf '%s\n' "$odex_disk") <(printf '%s\n' "$odex_in_dump") | grep -c '^/system/')
complete_rc=$dex_rc
sh_failed_ "$odex_rc" && complete_rc=$odex_rc
ckq "/system 里有编译记录的代码文件数 > 0" "$dex_rc" \
    "$([ "${sys_total:-0}" -gt 0 ] && echo 1 || echo 0)" "$sys_total 份"
ckq "/system 里每一份的编译记录都是最新的" "$dex_rc" \
    "$([ "${sys_stale:-1}" = 0 ] && echo 1 || echo 0)" "$sys_stale 份不是 kOatUpToDate"
ckq "/system 里没有一份是非 speed 的" "$dex_rc" \
    "$([ "${sys_other:-1}" = 0 ] && echo 1 || echo 0)" "$sys_other 份"
ckq "没有 ART 读不出状态的记录" "$dex_rc" \
    "$([ "${sys_broken:-1}" = 0 ] && echo 1 || echo 0)" "$sys_broken 份 invalid[]"
ckq "dumpsys 的每一条 /system 记录都是完整的" "$complete_rc" \
    "$([ "${sys_claimed:-0}" = "${sys_total:-0}" ] &&
       [ "${sys_named:-0}" = "${sys_total:-0}" ] &&
       [ "${sys_pkgs:-0}" = "${sys_pkgs_ok:--1}" ] &&
       [ "${odex_disk_n:-0}" -gt 0 ] && [ "${odex_unrecorded:-1}" = 0 ] && echo 1 || echo 0)" \
    "提到 $sys_named 个路径，开了 $sys_claimed 条，读完整的 $sys_total 条；$sys_pkgs 个 /system 包里 $sys_pkgs_ok 个有完整记录；/system 上 $odex_disk_n 个应用 odex 里 $odex_unrecorded 个在输出里没有记录"
# /data apps are a different question: what they carry depends on when they were
# installed, so this reports rather than judges.
echo "     /data 里的（装的时候是什么就是什么）："
printf '%s' "$dex" | grep -oE '/data/[^[]*\.odex\[status=kOatUpToDate,compilation_filter=[a-z-]+' |
  sed 's|.*/\([^/]*\)/oat/.*compilation_filter=|     \1  →  |' | sort -u
cache=$(printf '%s' "$dex" | grep -c '/data/dalvik-cache')
ckq "没有一份来自 /data/dalvik-cache" "$dex_rc" \
    "$([ "${cache:-1}" = 0 ] && echo 1 || echo 0)" "$cache 份"

echo
echo "=== 八、UsageStatsService 没有在刷「时间变了」 ==="
# Filtered here, not on the device, for the same reason as the kernel log above.
recent=$(sh_ 'logcat -d -t 2000'); recent_rc=$(sh_status_)
tc=$(printf '%s\n' "$recent" | grep -c "Time changed in UsageStats")
ckq "最近 2000 行日志里没有这条" "$recent_rc" \
    "$([ "${tc:-1}" = 0 ] && echo 1 || echo 0)" "$tc 条"

echo
echo "=== 九、首页与输入法 ==="
# The selected one, which is what this check is called. `ime list -s` lists the
# *enabled* ones and this read its first line; with one input method enabled the
# two coincide, which is why it never showed, and with two they need not
# (2026-11-03, found by review -- both answers checked on the device).
ime=$(sh_ 'settings get secure default_input_method'); ime_rc=$(sh_status_)
case "$ime" in *inputmethod.pinyin*) ime_ok=1 ;; *) ime_ok=0 ;; esac
ckq "选中的输入法是拼音" "$ime_rc" "$ime_ok" "$ime"
# 过滤放在本机：远端带管道时退出码是 grep 的，"没匹配"和"读失败"同一个非零。
home_raw=$(sh_ 'dumpsys activity activities'); home_rc=$(sh_status_)
home=$(printf '%s\n' "$home_raw" | grep -c "com.android.launcher3/.Launcher")
ckq "桌面在跑" "$home_rc" "$([ "${home:-0}" -gt 0 ] && echo 1 || echo 0)" "$home"

echo
# 这一节是 V80 教的。那一版上任何应用都装不上——adb install、系统的安装器、
# 微信读书那一格，全都停在同一步（PackageInstaller 声明大小之后
# readlink /proc/self/fd 被拒，`patches/android-core/1055-...`）。而装不上这件事
# 在任何别的地方都看不出来：桌面、输入法、刷新、日志全是正常的，上面九节也全过。
# 所以把它变成一条能自己跑的检查：建一个安装会话、写 4 KB 进去、立刻放弃。
# 不需要 root，不装任何东西，放弃之后设备上什么都不留。
echo "=== 十、安装通道还通不通 ==="
# "Failure [INSTALL_FAILED_...]" has no digits between its brackets, so the sed
# came back empty and this reported "could not ask" for a package manager that
# had answered, clearly, that it cannot open a session -- and the exit status
# only ever looked at failures (2026-11-03, found by review). The raw answer is
# kept so the two can be told apart.
created=$(sh_ 'cmd package install-create -r'); create_rc=$(sh_status_)
sess=$(printf '%s' "$created" | sed -n 's/.*\[\([0-9]*\)\].*/\1/p')
if sh_failed_ "$create_rc" && [ -z "$created" ]; then
  na "能往安装会话里写（声明大小的那条路）" "$(sh_why_ "$create_rc")"
  # The same question, answered for the same reason: with no session there is
  # nothing to abandon, and leaving it unreported made it look as if this check
  # had stopped being reached (2026-11-03).
  na "放弃掉了那个安装会话（不留东西在设备上）" "$(sh_why_ "$create_rc")"
elif [ -z "$sess" ]; then
  ck "能往安装会话里写（声明大小的那条路）" 0 "建不出安装会话：$created"
  skip "放弃掉了那个安装会话（不留东西在设备上）" "没有建出会话，没有东西要放弃"
else
  wr=$(sh_ "head -c 4096 /dev/zero | cmd package install-write -S 4096 $sess base - 2>&1"); wr_rc=$(sh_status_)
  sh_ "cmd package install-abandon $sess" >/dev/null; abandon_rc=$(sh_status_)
  case "$wr" in
    Success*) ckq "能往安装会话里写（声明大小的那条路）" "$wr_rc" 1 ;;
    *) ckq "能往安装会话里写（声明大小的那条路）" "$wr_rc" 0 "$wr" ;;
  esac
  # And the session is gone again, which is what lets the header say this
  # leaves nothing behind. The result used to be thrown away (2026-11-03).
  ckq "放弃掉了那个安装会话（不留东西在设备上）" "$abandon_rc" 1
fi

echo
# 同一个教训的推广：装不上应用是"一条通道整条不通，而别处全是正常的"。
# 这一节走另外三条使用者真的要用、而且坏了同样看不出来的通道。都不占屏幕；
# 唯一的写操作是在 /sdcard/Download 下建一个一看就知道是什么的临时文件，
# 读回来再删掉。
echo "=== 十一、另外三条通道 ==="
# 1. 闹钟。AlarmManager 设不上 RTC 告警，后台任务和唤醒就都不成立，而前台
#    用起来一切正常。有没有在排的告警，是这条通道通不通最直接的证据。
alarm_raw=$(sh_ 'dumpsys alarm'); alarm_rc=$(sh_status_)
alarms=$(printf '%s\n' "$alarm_raw" | grep -cE "RTC_WAKEUP|ELAPSED_WAKEUP")
ckq "AlarmManager 里有在排的告警" "$alarm_rc" \
    "$([ "${alarms:-0}" -gt 0 ] && echo 1 || echo 0)" "$alarms 条"
# 2. 使用者的存储。浏览器下载、文件应用、MTP 看到的都是这一个目录，它走的是
#    FUSE/sdcardfs 那条路，和应用私有目录不是一回事。写在 /sdcard 根下而不是
#    Download 下：后者要等下载管理器建出来，不一定在。
store=$(sh_ 'echo verify-device-temp > /sdcard/verify-device-check.tmp 2>&1 && cat /sdcard/verify-device-check.tmp 2>&1; rm -f /sdcard/verify-device-check.tmp 2>&1'); store_rc=$(sh_status_)
ckq "使用者存储能写能读" "$store_rc" \
    "$([ "$store" = "verify-device-temp" ] && echo 1 || echo 0)" "$store"
# 3. 时钟。差太多的话 HTTPS 直接失败，而设备上只表现为"下载不动"。
dev_epoch=$(sh_ 'date +%s'); epoch_rc=$(sh_status_)
host_epoch=$(date +%s)
# The name is the question, and the question gets one of the three answers.
# Written as "a usable number, or else na", an answer that arrived and was not
# a number came back as "could not read the clock" -- which is the device
# saying something wrong, not the question going unanswered, and it was enough
# to keep a run at exit 2 (2026-11-03, found by this file's own no-root case).
if sh_failed_ "$epoch_rc"; then
  na "设备时钟与本机相差在 5 分钟内" "$(sh_why_ "$epoch_rc")"
# Through num_in_range_ rather than `[ -le 0 ] 2>/dev/null`, which answered a
# clock of 9223372036854775808 with `ok (差 -9223372035061061944 秒)` -- signed
# overflow wrapped the difference negative and a negative difference is inside
# five minutes -- and lost the check altogether on 01800000000 (2026-11-03,
# found by review). The bound is eleven digits, which is the year 5138; the
# normalised value is what the arithmetic below sees, because `$(( ))` reads a
# leading zero as octal.
elif ! num_in_range_ "$dev_epoch" 99999999999 || [ "$(dec_ "$dev_epoch")" = 0 ]; then
  ck "设备时钟与本机相差在 5 分钟内" 0 "设备答的不是一个纪元秒数：'$dev_epoch'"
else
  dev_epoch=$(dec_ "$dev_epoch")
  skew=$(( dev_epoch > host_epoch ? dev_epoch - host_epoch : host_epoch - dev_epoch ))
  ck "设备时钟与本机相差在 5 分钟内" \
     "$([ "$skew" -le 300 ] && echo 1 || echo 0)" "差 $skew 秒"
fi

echo
# 2026-11-04 教的。11-03 13:35 设备从发行签名的镜像换到测试签名的镜像，平台签名
# 一变，那次开机里框架的权限定义缺席，微信读书的安装时权限只剩 INTERNET 和
# BLUETOOTH（这两个来自 platform.xml 的 gid 映射），之后系统按 installPermissionsFixed
# 不再补发。从那以后它每次启动都在 Application.onCreate 里崩，而这个脚本在那十个小时
# 里跑了好几次，每次都报"0 失败"——上面没有一节问过"使用者要用的那个应用起不起得来"。
# 所以两问：每个第三方应用申请的普通权限是不是都拿到了（直接对着那个根因，不占屏幕）；
# 以及微信读书能不能冷启动出第一帧而且不崩（**这一项会占屏幕约一分钟，期间别碰设备**，
# 做完回到桌面）。没装微信读书的设备上第二问按设计跳过。
echo "=== 十二、第三方应用：普通权限都在，主应用起得来（会占屏幕约一分钟）==="
perm_raw=$(sh_ 'pm list permissions -f'); perm_rc=$(sh_status_)
pkg_raw=$(sh_ 'pm list packages -3'); pkg_rc=$(sh_status_)
# Two answers the third-party list is held to, because an empty list that
# exits 0 used to skip both checks below as "nothing installed" (11-04 review,
# round two, item 4): every package (the system's own are always there, 34 on
# f), and WeRead asked for by name. `pm path` answers 0 with a package: line
# when it is installed and 1 with nothing when it is not (measured on f) --
# and 1 with nothing is also what a pm that fails says, so the same command
# first asks for `android`, which is always installed: only when that answers
# does WeRead's 1 mean "not installed".
all_raw=$(sh_ 'pm list packages'); all_rc=$(sh_status_)
n_all=$(printf '%s\n' "$all_raw" | grep -c '^package:')
WR=com.tencent.weread.eink
wr_probe=$(sh_ "pm path android; echo \"ctl=\$?\"; pm path $WR; echo \"wr=\$?\""); wr_probe_rc=$(sh_status_)
wr_ctl=$(printf '%s\n' "$wr_probe" | sed -n '1,/^ctl=/p')
wr_rc=$(printf '%s\n' "$wr_probe" | sed -n 's/^wr=\([0-9]*\)$/\1/p')
wr_path=$(printf '%s\n' "$wr_probe" | sed -n '/^ctl=/,/^wr=/p' | grep '^package:')
if sh_failed_ "$wr_probe_rc" || ! printf '%s\n' "$wr_ctl" | grep -q '^package:' ||
   ! printf '%s\n' "$wr_ctl" | grep -qx 'ctl=0'; then
  wr_installed=unknown; wr_rc=${wr_rc:-?}
elif [ "$wr_rc" = 0 ] && [ -n "$wr_path" ]; then wr_installed=yes
elif [ "$wr_rc" = 1 ] && [ -z "$wr_path" ]; then wr_installed=no
else wr_installed=unknown
fi
if sh_failed_ "$perm_rc" || sh_failed_ "$pkg_rc"; then
  na "第三方应用申请的普通权限都已授予" "$(sh_why_ "$(sh_failed_ "$perm_rc" && echo "$perm_rc" || echo "$pkg_rc")")"
else
  # `pm list permissions -f` 每个权限一段："+ permission:<名字>" 之后有
  # "protectionLevel:normal"（可能带 |instant 之类的后缀）。只认 normal：
  # 危险权限要使用者点同意，没授予是正常的。
  normal=$(printf '%s\n' "$perm_raw" | awk '
    /permission:/ { p = $0; sub(/.*permission:/, "", p); sub(/[ \t].*/, "", p) }
    /protectionLevel:/ { l = $0; sub(/.*protectionLevel:/, "", l); split(l, t, "|"); if (t[1] == "normal" && p != "") print p }' | sort -u)
  n_normal=$(printf '%s\n' "$normal" | grep -c .)
  pkgs=$(printf '%s\n' "$pkg_raw" | sed -n 's/^package://p')
  n_pkgs=$(printf '%s\n' "$pkgs" | grep -c .)
  if [ "$n_normal" -lt 20 ]; then
    # 框架自己定义的普通权限有上百个；读出来不到 20 个说明解析没对上，不是设备对。
    ck "第三方应用申请的普通权限都已授予" 0 "只解析出 $n_normal 个普通权限，解析和输出格式对不上"
  elif sh_failed_ "$all_rc" || [ "$n_all" = 0 ]; then
    na "第三方应用申请的普通权限都已授予" "pm 一个包都没列出来，连系统自带的都没有，所以第三方的那张表也不可信"
  elif [ "$wr_installed" = yes ] && ! printf '%s\n' "$pkgs" | grep -qxF "$WR"; then
    na "第三方应用申请的普通权限都已授予" "pm path 找得到微信读书，第三方应用的列表里却没有它——列表不全"
  elif [ "$n_pkgs" = 0 ] && [ "$wr_installed" = no ]; then
    skip "第三方应用申请的普通权限都已授予" "设备上没有第三方应用"
  elif [ "$n_pkgs" = 0 ]; then
    na "第三方应用申请的普通权限都已授予" "第三方应用的列表是空的，而问不出微信读书装没装（pm path 以 $wr_rc 退出）"
  else
    missing_all=""; asked_rc=0
    for pk in $pkgs; do
      d=$(sh_ "dumpsys package $pk"); d_rc=$(sh_status_)
      if sh_failed_ "$d_rc"; then asked_rc=$d_rc; continue; fi
      # A successful empty or cut-short dump is not "this app asks for nothing":
      # with no `requested permissions:` lines the loop below never runs and the
      # app passes (11-04 review, item 5). The section is printed only when the
      # app requests something, so its absence proves nothing; what proves the
      # dump got past it is the package's own header and the per-user line that
      # dumpPackageLPr prints after both permission sections (Settings.java,
      # measured on f: Package [ at line 113, requested 137, install 162, User 0 174).
      if ! printf '%s\n' "$d" | grep -qF "  Package [$pk]" ||
         ! printf '%s\n' "$d" | grep -qE '^    User 0: '; then
        asked_rc=incomplete; continue
      fi
      req=$(printf '%s\n' "$d" | awk '/^    requested permissions:/{f=1; next} f && /^      [a-zA-Z]/{print $1; next} f{exit}')
      got=$(printf '%s\n' "$d" | awk '/install permissions:/{f=1; next} f && /granted=true/{sub(/:.*/, "", $1); print $1; next} f && !/^      /{f=0}')
      for r in $req; do
        printf '%s\n' "$normal" | grep -qxF "$r" || continue
        printf '%s\n' "$got" | grep -qxF "$r" || missing_all="$missing_all $pk:${r##*.}"
      done
    done
    if [ -n "$missing_all" ]; then
      ck "第三方应用申请的普通权限都已授予" 0 "缺：$missing_all（换过平台签名的升级会这样；重装那个应用可恢复）"
    elif [ "$asked_rc" = incomplete ]; then
      na "第三方应用申请的普通权限都已授予" "有应用的 dumpsys package 输出不完整（没有它的 Package [ 头或 User 0: 行），读不出它申请了什么"
    elif [ "$asked_rc" != 0 ]; then
      na "第三方应用申请的普通权限都已授予" "$(sh_why_ "$asked_rc")"
    else
      ck "第三方应用申请的普通权限都已授予" 1 "$n_pkgs 个应用，对照 $n_normal 个普通权限"
    fi
  fi
fi
if [ "$wr_installed" = unknown ]; then
  na "微信读书冷启动出第一帧、没有崩" "问不出微信读书装没装（pm path 以 $wr_rc 退出）"
elif [ "$wr_installed" = no ]; then
  skip "微信读书冷启动出第一帧、没有崩" "微信读书还没装（全新 /data 上要使用者先点首页那一格）"
else
  # 一个设备上的进程等那条事件，不在本机轮询（单核上周期性起进程本身就占 CPU）。
  # 起点用设备自己的时钟，所以只认这次启动之后的日志。
  #
  # events 是二进制缓冲区，-e 只匹配格式化后的值、不匹配标签：写成
  # -e 'am_activity_launch_time.*包名' 什么都等不到，120 秒后报"没有画出第一帧"
  # （11-04 在 f 版上实测，连已经在缓冲区里的那条都匹配不上）。标签放进过滤
  # 表达式，-e 只管包名。答案也只认那条事件本身：-T 有时先打一行
  # "--------- beginning of"，只问"输出非空"会把它当成画出来了。
  #
  # 起点精确到毫秒，用纪元秒（logcat -T 认 "秒.毫秒"）：原来写成
  # "MM-DD hh:mm:ss.000"，向下取整到整秒，同一秒里上一次启动留下的那条首帧
  # 事件也算在内，新进程卡住时照样报 ok（11-04 复审第 6 条）。toybox 的 date
  # 有 %N（11-04 在 f 上实测）。读不到就不问，不拿一个不知道是什么的起点去等。
  t0=$(sh_ 'date +%s.%N'); t0_rc=$(sh_status_)
  t0=$(printf '%s' "$t0" | sed -n 's/^\([0-9][0-9]*\.[0-9][0-9][0-9]\)[0-9]*$/\1/p')
  if sh_failed_ "$t0_rc" || [ -z "$t0" ]; then
    # Not a start this check can tie anything to, so nothing is started: the
    # status stands for "could not ask", and so do the two below.
    st_rc=$t0_rc; [ "$st_rc" = 0 ] && st_rc=1
    shown=""; shown_rc=$st_rc; crashed=0; crash_rc=$st_rc
  else
    sh_ "am force-stop $WR; input keyevent KEYCODE_WAKEUP; am start -n $WR/com.tencent.weread.LauncherActivity" >/dev/null; st_rc=$(sh_status_)
    shown_raw=$(sh_ "timeout 120 logcat -b events -T '$t0' -e '$WR' -m 1 am_activity_launch_time:I '*:S'"); shown_rc=$(sh_status_)
    shown=$(printf '%s\n' "$shown_raw" | grep 'am_activity_launch_time' | head -1)
    # 崩溃写进缓冲区比 am start 返回晚好几秒（11-04 实测 am crash 之后约 6 秒），
    # 所以放在等首帧之后问。
    crash_raw=$(sh_ "logcat -b crash -d -T '$t0'"); crash_rc=$(sh_status_)
    crashed=$(printf '%s\n' "$crash_raw" | grep -c "Process: $WR")
  fi
  sh_ 'input keyevent KEYCODE_HOME' >/dev/null
  if sh_failed_ "$st_rc"; then
    na "微信读书冷启动出第一帧、没有崩" "$(sh_why_ "$st_rc")"
  elif sh_failed_ "$crash_rc"; then
    na "微信读书冷启动出第一帧、没有崩" "$(sh_why_ "$crash_rc")"
  elif [ "${crashed:-0}" -gt 0 ]; then
    ck "微信读书冷启动出第一帧、没有崩" 0 "启动后崩了 $crashed 次（logcat -b crash）"
  elif [ "$shown_rc" != 0 ] && [ "$shown_rc" != 124 ]; then
    # 只有两种退出是答案：0（等到了那一条，-m 1 让它退出）和 124（timeout
    # 到点）。连接断在半路、logcat 自己出错，拿到的哪怕有一截像那条事件也不作数，
    # 没拿到也不能说成"等过了"（11-04 复审第 7 条、第二轮第 6 条）。
    na "微信读书冷启动出第一帧、没有崩" "$(sh_why_ "$shown_rc")"
  elif [ -z "$shown" ] || [ "$shown_rc" = 124 ]; then
    ck "微信读书冷启动出第一帧、没有崩" 0 "120 秒内没有画出第一帧（logcat 退出码 $shown_rc）"
  else
    # 只认完整的一条：[用户,记录,包名/活动,本次毫秒,总毫秒]。
    ms=$(printf '%s' "$shown" | sed -n "s/.*am_activity_launch_time: \[[0-9]*,[0-9]*,$WR\/[^,]*,\([0-9][0-9]*\),[0-9][0-9]*\]\$/\1/p")
    if [ -z "$ms" ]; then
      na "微信读书冷启动出第一帧、没有崩" "等到的那一行不是完整的首帧事件：$shown"
    else
      ck "微信读书冷启动出第一帧、没有崩" 1 "首帧 $ms ms"
    fi
  fi
fi

echo
echo "=== 十三、装一个应用要多久（唯一真正的验收，需要你动手）==="
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
[ "$skipped" -gt 0 ] && printf '，%d 项这个镜像上按设计查不了（上面标 ~~ 的，每一项都写了替代问法）' "$skipped"
echo
# Three outcomes in the exit status as well, for the same reason the report has
# three: "nothing was found wrong" is not "everything was checked". The status
# used to be `[ "$fail" = 0 ]`, so a run in which every single question failed
# -- no cable, wrong adb server, a `user` image where the readings need root --
# exited 0 and read as an acceptance pass (2026-11-03).
if [ "$fail" != 0 ]; then
  echo "退出码 1：有检查失败"
  exit 1
elif [ "$unknown" != 0 ]; then
  echo "退出码 2：没有发现错的，但有 $unknown 项没能查，所以这一轮不构成验收通过"
  exit 2
fi
echo "退出码 0：该在这个镜像上问的都问到了，而且都对${skipped:+（另有 $skipped 项按设计查不了）}"
exit 0
