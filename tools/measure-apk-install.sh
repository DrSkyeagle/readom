#!/bin/bash
# 装一个应用到底要多久：在你点安装的时候，把该记的都记下来。
#
# 这是 V63 的验收。属性对了只说明设置生效了，不说明体验变了，而"感觉快了"
# 不是一个可以写进文档的数。所以这个脚本把四件事量出来：
#
#   1. 从提交安装到装完，实际多少秒
#   2. 这段时间里 dex2oat 跑了多久、用的什么编译级别
#   3. 装完之后这个包记录的安装原因（4 = INSTALL_REASON_USER，Launcher3 只给
#      "人主动装的"加图标；V61 改的就是这个，从没在设备上验证过）
#   4. 首页有没有真的多出一个图标
#
# 怎么做到不打扰被测的那件事：**全程只起一个常驻进程**（一条 adb logcat 流），
# 前后各取一次快照。这台机器是单核，周期性起进程的测量脚本自己就要占
# 6%–22% 的 CPU，那样量出来的时间不是安装的时间
# （docs/design/performance.md 第五节）。
#
# 只读。不写 Kindle 盘，不写 /data，不改电源状态，不碰你的数据。
#
# 用法：
#   tools/measure-apk-install.sh                 # 等你用"文件"应用去装
#   ADB=/path/to/adb tools/measure-apk-install.sh
set -u
ADB=${ADB:-/opt/homebrew/share/android-commandlinetools/platform-tools/adb}
# --apk <本机路径>：推到设备上，并按它的包名来量。给了它，升级安装（装回同一个
# 包）也量得到——否则只能靠"包列表多了一项"来认，而升级不会多出一项。
APK=""
while [ $# -gt 0 ]; do
  case "$1" in
    --apk) APK=${2:-}; shift 2 || die "--apk 后面要跟一个路径";;
    *) echo "不认识的参数：$1" >&2; exit 2;;
  esac
done
WORK=$(mktemp -d "${TMPDIR:-/tmp}/pw3-install-measure.XXXXXX")
LOG="$WORK/logcat.txt"
LOGCAT_PID=""

cleanup() {
  [ -n "$LOGCAT_PID" ] && kill "$LOGCAT_PID" 2>/dev/null
  wait "$LOGCAT_PID" 2>/dev/null
}
trap cleanup EXIT INT TERM

sh_() { $ADB shell "$@" 2>/dev/null | tr -d '\r'; }
die() { echo "停下：$1" >&2; exit 1; }

# ---------------------------------------------------------------- 前置
echo "=== 一、设备与设置 ==="
[ -n "$($ADB devices | sed -n '2p')" ] || die "adb 看不到设备。先插线，并确认 Android 正在跑。"
echo "   指纹 $(sh_ 'getprop ro.build.fingerprint')"
inst=$(sh_ 'getprop pm.dexopt.install')
echo "   pm.dexopt.install = $inst"
[ "$inst" = quicken ] || echo "   ！这不是期望的 quicken。下面量出来的是 '$inst' 的时间。"

# 这个脚本最要紧的两项读数——安装原因和首页图标——都在 /data 里，要 root 才读得到。
# 不是 root 的话它们会读成空，而"读不到"和"读到了但不对"是两个完全不同的结论：
# 前者是工具的问题，后者说明 V61 那个改动没生效。所以在你动手装之前就问清楚，
# 而不是装完一次才发现白装。
#
# 取 root 的办法是 `su 0`，不是 `adb root`。`adb root` 之后 `adb devices` 会变空。
# 这台 Mac 上有两个 adb 服务（5038、5097），设备在它们之间跳：2026-10-29 实测过的
# 是**框架重启**那一种，设备从 5097 跳到 5038，`ioreg` 全程看得到它。`adb root`
# 那一次当时没有查别的端口，所以「同一件事」是按奥卡姆剃刀推的，不是实测
# （docs/known-issues.md 第三节）。这里原来写的「init.pw3.usb.rc 两步式没有重跑」
# 那套解释是错的，已经在源码里核掉：init 对属性的每次成功写入都会触发，不和旧值
# 比较。`su 0` 不重启 adbd，所以不碰这件事。
ROOT_PREFIX=""
REASON_XML=/data/system/users/0/package-restrictions.xml
uid=$(sh_ 'id -u')
if [ "$uid" != 0 ]; then
  if [ "$(sh_ 'su 0 id -u')" = 0 ]; then
    ROOT_PREFIX="su 0 "
    echo "   adb 是 shell，用 su 0 提权（不重启 adbd）"
  else
    echo "   adb 不是 root（id -u = ${uid:-读不到}），su 0 也不行"
    echo
    echo "   停下，因为这样跑的话最要紧的两项会读成空，而那看起来会像是"
    echo "   V61 的改动没生效。"
    echo
    echo "   不要用 adb root：在这台机器上它之后 adb devices 会变空（见本脚本开头的"
    echo "   注释）。先查别的端口（-P 5038、-P 5097），不要先拔插。userdebug 构建"
    echo "   应当有 su；没有的话就是构建变体不对，"
    echo "   先确认 ro.build.type 是 userdebug。"
    exit 1
  fi
else
  echo "   adb 已经是 root"
fi
# 提权之后的读；只读，不写 /data。
root_sh_() { sh_ "$ROOT_PREFIX$1"; }
if [ "$(root_sh_ "test -r $REASON_XML && echo yes || echo no")" != yes ]; then
  die "$REASON_XML 读不到。安装原因这一项是这次验收的核心判据，读不到就不要白装一次。"
fi
echo "   $(basename $REASON_XML) 读得到"

# 装机前的三份快照。每一份就是一次 adb 调用，之后在安装期间一次都不再调。
echo
echo "=== 二、记下装之前的样子 ==="
sh_ 'pm list packages' | sed 's/^package://' | sort > "$WORK/packages.before"
echo "   已装的包 $(wc -l < "$WORK/packages.before" | tr -d ' ') 个"
LDB=/data/data/com.android.launcher3/databases/launcher.db
# -readonly 打不开时（热 WAL 需要建 -shm）退回普通打开；只读查询不改库。
# Through root_sh_, which is what the precheck above proved can read these.
# Reading them as shell passed the precheck and then came back empty.
ldb_query() { root_sh_ "sqlite3 -readonly $LDB \"$1\" 2>/dev/null || sqlite3 $LDB \"$1\"" | tr -d ' '; }
favs_before=$(ldb_query "select count(*) from favorites")
case "$favs_before" in
  ''|*[!0-9]*) echo "   首页图标数：读不到（$LDB 打不开，连 su 0 也打不开——不要用 adb root，见开头的注释）"; favs_before="" ;;
  *)           echo "   首页图标数 $favs_before" ;;
esac

# ---------------------------------------------------------------- 送 APK 过去
TARGET=""
echo
echo "=== 三、把 APK 送到设备上 ==="
if [ -z "$APK" ]; then
  echo "   没给 --apk，所以不推。请自己确保设备上有一个能用「文件」打开的 APK。"
  echo "   （装回一个已经装着的包时必须给 --apk：升级不会让包列表多出一项，"
  echo "   只靠差集认不出来。）"
fi
if [ -n "$APK" ]; then
  [ -f "$APK" ] || die "找不到 $APK"
  # 包名在本机读，不在设备上读：设备上没有 aapt。解析的是二进制清单里 manifest
  # 元素的 package 属性——"清单里第一个点分名字"那种猜法是错的，字符串池不按
  # 文档顺序排，微信读书那样猜出来是 androidx.core.app（tools/apk-package-name.py）。
  TARGET=$(python3 -I "$(dirname "$0")/apk-package-name.py" "$APK") \
    || die "从 $APK 里读不出包名"
  [ -n "$TARGET" ] || die "从 $APK 里读不出包名"
  printf '   %s（%d 字节，包名 %s）\n' "$(basename "$APK")" "$(wc -c < "$APK" | tr -d ' ')" "$TARGET"
  DEST=/sdcard/$(basename "$APK")
  $ADB push "$APK" "$DEST" > /dev/null 2>&1 || die "推不过去：$DEST"
  pushed=$(sh_ "wc -c < $DEST" | tr -d ' ')
  [ "$pushed" = "$(wc -c < "$APK" | tr -d ' ')" ] || die "推过去的字节数不对（$pushed）"
  echo "   已放到 $DEST"
  if grep -qxF "$TARGET" "$WORK/packages.before"; then
    echo "   这个包已经装着——下面量的是一次升级安装（拷贝、安装会话、dex2oat 全都会跑）"
  else
    echo "   这个包还没装过——下面量的是一次全新安装"
  fi
fi

# ---------------------------------------------------------------- 开始记录
echo
echo "=== 四、现在去装 ==="
$ADB logcat -c 2>/dev/null
# -v year：不带年份的 "MM-DD" 没法无歧义地解析（闰日），而 logcat
# 自己支持带上（system/core/liblog/logprint.c）。
$ADB logcat -v year -v threadtime > "$LOG" 2>/dev/null &
LOGCAT_PID=$!
sleep 1
kill -0 "$LOGCAT_PID" 2>/dev/null || die "logcat 起不来。"
cat <<'EOF'
   日志已经在记了（只有这一个进程，不会去抢 CPU）。

   请在 Kindle 上：
     1. 打开"文件"应用，点一个 APK；
     2. 点"安装"；
     3. 等它装完（装完会显示"已安装"或直接回到文件列表）。

   注意这三件事，一会儿要和量出来的数对一下：
     · 进度条下那行字是不是"正在为本机准备应用…"，下一行"较大的应用可能要几分钟。"
     · 屏幕有没有在装的过程中灭掉（熄屏超时是 10 分钟，正常不该灭）
     · 装完首页有没有自动多出一个图标

   装完之后回到这里按 Enter。
EOF
printf '   > '
read -r _ || true
sleep 2
cleanup; LOGCAT_PID=""

# ---------------------------------------------------------------- 装了什么
echo
echo "=== 五、装了什么 ==="
sh_ 'pm list packages' | sed 's/^package://' | sort > "$WORK/packages.after"
if [ -n "$TARGET" ]; then
  # 给了 --apk 就按包名量，不靠"多了一项"——升级安装不会多出一项。
  NEW=$TARGET
  grep -qxF "$TARGET" "$WORK/packages.after" || die "$TARGET 装完之后不在包列表里；日志在 $LOG"
  echo "   $TARGET 在包列表里"
else
  NEW=$(comm -13 "$WORK/packages.before" "$WORK/packages.after")
  count=$(printf '%s' "$NEW" | grep -c . )
  if [ "$count" = 0 ]; then
    echo "   没有新包。要么没装成，要么装的是已有包的升级版——升级的话用 --apk <路径> 再跑一次。"
    echo "   日志留在 $LOG，自己看看吧。"
    exit 1
  fi
  [ "$count" -gt 1 ] && echo "   装了 $count 个包，下面逐个报。"
fi

# ---------------------------------------------------------------- 时间
# 一次安装在日志里的两端：PackageInstaller 提交会话，和包管理器报告装完。
# 用 logcat 自己的时间戳，不用我们这边的墙上时钟——两边的时钟不必一致。
secs_between() {  # $1 起始 $2 结束，都是 "YYYY-MM-DD HH:MM:SS.mmm"
  python3 -I -c '
import sys, datetime
def t(s):
    s = s.strip()
    for fmt in ("%Y-%m-%d %H:%M:%S.%f", "%Y-%m-%d %H:%M:%S"):
        try: return datetime.datetime.strptime(s, fmt)
        except ValueError: pass
    # 万一这台机器的 logcat 不认 -v year，补一个闰年再解，日期差照样对。
    for fmt in ("%m-%d %H:%M:%S.%f", "%m-%d %H:%M:%S"):
        try: return datetime.datetime.strptime("2024-" + s, "%Y-" + fmt)
        except ValueError: pass
    raise ValueError(s)
try:
    d = (t(sys.argv[2]) - t(sys.argv[1])).total_seconds()
    print(f"{d:.1f}" if d >= 0 else "")
except Exception:
    print("")
' "$1" "$2"
}
# 带年份是 23 个字符，不带是 18；两种都切得到完整的时间戳。
stamp_of() { sed -n "${1}p" "$LOG" | grep -o -E "^([0-9]{4}-)?[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}"; }

echo
echo "=== 六、量出来的 ==="
for pkg in $NEW; do
  echo "   ── $pkg ──"

  # 两端。提交那一行在 8.1 上由 PackageInstaller 的会话打出，装完那一行由
  # PackageManager 打出，而且带包名。
  #
  # 找不到就说找不到。原来这里退回"日志里第一条和最后一条提到这个包的行"，
  # 那不是测量：只剩一条提到它时两端落在同一行，输出 0.0 秒，而真实安装用了
  # 多久完全没有进来。一次记录里装了两个包时，没按包名过滤的起点还会被共用。
  commits=$(grep -n -E "PackageInstaller|PackageManager" "$LOG" \
            | grep -E "Commit|commit|installPackage|INSTALL" || true)
  begin=$(printf '%s\n' "$commits" | grep -F "$pkg" | head -1 | cut -d: -f1)
  if [ -z "$begin" ] && [ "$(printf '%s\n' "$commits" | grep -c .)" = 1 ]; then
    # 整个日志里只有一条提交行，所以它不可能是别的包的。
    begin=$(printf '%s\n' "$commits" | cut -d: -f1)
  fi
  end=$(grep -n -E "PackageManager|PackageInstaller" "$LOG" \
            | grep -F "$pkg" | grep -E -i "success|installed|finish" | tail -1 | cut -d: -f1)
  if [ -n "$begin" ] && [ -n "$end" ]; then
    d=$(secs_between "$(stamp_of "$begin")" "$(stamp_of "$end")")
    echo "   安装耗时         ${d:-读不出} 秒   （日志第 $begin 行到第 $end 行）"
  elif [ -z "$begin" ]; then
    echo "   安装耗时         测不到——日志里没有这个包的提交行（共 $(printf '%s\n' "$commits" | grep -c .) 条提交行）"
  else
    echo "   安装耗时         测不到——日志里没有这个包的"装完"行"
  fi

  # dex2oat：它自己会打出来，包括用的编译级别。
  d2o_first=$(grep -n "dex2oat" "$LOG" | head -1 | cut -d: -f1)
  d2o_last=$(grep -n "dex2oat" "$LOG" | tail -1 | cut -d: -f1)
  d2o_n=$(grep -c "dex2oat" "$LOG")
  if [ -n "$d2o_first" ]; then
    dd=$(secs_between "$(stamp_of "$d2o_first")" "$(stamp_of "$d2o_last")")
    pkgs_installed=$(printf '%s\n' $NEW | grep -c .)
    if [ "$pkgs_installed" -gt 1 ]; then
      echo "   dex2oat          ${dd:-?} 秒，$d2o_n 行日志——**这一轮装了 $pkgs_installed 个包，这个跨度不归任何一个**"
    else
      echo "   dex2oat          ${dd:-?} 秒，$d2o_n 行日志（整份日志里第一条到最后一条 dex2oat 行）"
    fi
    grep -o -- "--compiler-filter=[a-z-]*" "$LOG" | sort -u | sed 's/^/   编译级别（日志） /'
  else
    echo "   dex2oat          日志里一行都没有"
  fi

  # 实际留在盘上的编译级别，由包管理器自己报告。
  filt=$(sh_ "dumpsys package dexopt" | tr '\n' '\001' | sed 's/\001      / /g' | tr '\001' '\n' \
         | grep -F "$pkg" -A2 | grep -o "compilation_filter=[a-z-]*" | sort -u | tr '\n' ' ')
  echo "   编译级别（产物） ${filt:-读不到}"

  # 安装原因。Settings.java 把它写进这个 XML，4 = INSTALL_REASON_USER。
  # 这个包在 XML 里有没有自己的条目，和它的 install-reason 是多少，是两件事：
  # Settings.java 只在 reason != UNKNOWN 时才写这个属性，所以"有条目但没属性"
  # 就是 UNKNOWN（0），而"连条目都没有"说明读法不对——两者不能都报成"没生效"。
  entry=$(root_sh_ "grep -c 'name=\"$pkg\"' $REASON_XML")
  reason=$(root_sh_ "grep -o 'name=\"$pkg\"[^>]*install-reason=\"[0-9]*\"' $REASON_XML" \
           | grep -o 'install-reason=\"[0-9]*\"' | grep -o '[0-9]*' | head -1)
  if [ "${entry:-0}" = 0 ]; then
    echo "   安装原因         读不到——$REASON_XML 里没有这个包的条目。"
    echo "                    不是「没生效」，是这个判据没读到；包管理器可能还没把它落盘，"
    echo "                    过一会儿再看，或者 '$ADB shell am force-stop com.android.settings' 之后重试。"
  else
    case "${reason:-}" in
      4)  echo "   安装原因         4 = INSTALL_REASON_USER  ✓（Launcher3 会给它加图标）" ;;
      '') echo "   安装原因         没写这个属性 = UNKNOWN(0)  ✗ Launcher3 不会加图标，V61 的改动没生效" ;;
      *)  echo "   安装原因         $reason  ✗ 期望 4（INSTALL_REASON_USER）" ;;
    esac
  fi

  # 首页图标。
  if [ -n "$favs_before" ]; then
    favs_after=$(ldb_query "select count(*) from favorites")
    hit=$(ldb_query "select count(*) from favorites where intent like '%$pkg%'")
    echo "   首页图标数       $favs_before → ${favs_after:-?}"
    case "${hit:-0}" in
      0) echo "   它的图标         没有  ✗" ;;
      *) echo "   它的图标         有（$hit 个）  ✓" ;;
    esac
  else
    echo "   首页图标         读不到（连 su 0 也读不到）；请自己看一眼首页"
  fi
done

# ---------------------------------------------------------------- 副作用
echo
echo "=== 七、这段时间里有没有别的毛病 ==="
for pat in "Time changed in UsageStats" "ANR in" "FATAL EXCEPTION" "lowmemorykiller" "Out of memory"; do
  n=$(grep -c "$pat" "$LOG")
  [ "$n" = 0 ] && echo "   ok   没有 \"$pat\"" || echo "   ！    \"$pat\" 出现 $n 次"
done
slept=$(grep -c "Sending display state" "$LOG")
echo "   （屏幕状态变化 $slept 次——装的过程中不该灭屏，熄屏超时是 10 分钟）"

echo
echo "完整日志：$LOG"
echo "把上面「量出来的」那一段贴回来就行。"
