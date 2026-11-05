#!/bin/bash
# 重影阈值当场对比：
#
# 这个脚本在仓库里，和 verify-device.sh 同一个原因：它曾经只存在于某一版的
# build-share/ 目录下，而按版本各存一份的脚本会各自继承上一份的毛病（第十八节）。
# 换机型的人也要用它——每块屏的波形不同，阈值要各自看过再定。
#
# 原理：同一段翻页动作，分别在 full_every = 30 / 20 / 12 下跑一遍，
# 每一遍结束后停下来让人看屏幕，并打印这一遍实际发生了多少次整屏清除。
#
# 屏幕上会有动作（打开浏览器、连续翻页），每个阈值约 40 秒，期间请不要碰设备。
# 只改一个 debug 属性，不写 /data、不重启任何服务：gralloc 每 16 帧重读
# debug.pw3.epdc.*（framebuffer.cpp 的 readPolicy），所以改完翻一页就生效。
#
# 用法：ADB=…/adb bash ghosting-ab.sh [阈值…]      默认 30 20 12
#   ADB_PORT=  adb 服务端口，设备被另一个 adb 服务占着时用（verify-device.sh 同名变量）
set -u
ADB=${ADB:-/opt/homebrew/share/android-commandlinetools/platform-tools/adb}
ADB_PORT=${ADB_PORT:-}
PAGES=${PAGES:-20}
BROWSER=info.plateaukao.einkbro
adb_() { if [ -n "$ADB_PORT" ]; then "$ADB" -P "$ADB_PORT" "$@"; else "$ADB" "$@"; fi; }
sh_() { adb_ shell "$@" 2>/dev/null | tr -d '\r'; }
root() { adb_ shell "su 0 sh -c '$1'" 2>/dev/null | tr -d '\r'; }

counters() { sh_ 'dumpsys SurfaceFlinger' | grep -o "PW3 panel:.*"; }
field() { echo "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"; }

[ -n "$(adb_ devices | sed -n '2p')" ] || { echo "设备不在，先确认 adb 能看到它"; exit 1; }

# 一页密排中文正文：翻页时整屏都在变，和看书一样，这才问得出重影
page=$(mktemp /tmp/pw3-ghost.XXXX.html)
python3 - "$page" <<'PY'
import sys
para = ("墨水屏的残影来自上一幅画面没有被完全清除的那一部分，"
        "所以判断它只能靠眼睛：同样翻二十页，清得勤一些更干净、"
        "但屏幕黑一下的次数也更多。这一页的字排得很密，翻页时整屏都在变。")
with open(sys.argv[1], 'w', encoding='utf-8') as f:
    f.write('<!doctype html><meta charset="utf-8">'
            '<body style="font:16px/1.7 sans-serif;margin:16px">')
    for i in range(1, 121):
        f.write('<p>%d. %s</p>' % (i, para * 2))
    f.write('</body>')
PY
adb_ push "$page" /sdcard/pw3-ghost.html >/dev/null 2>&1
rm -f "$page"

echo "屏幕上要有动作了：打开浏览器、翻 $PAGES 页，每个阈值约 40 秒。期间请不要碰设备。"
sleep 2
adb_ shell "am start -a android.intent.action.VIEW -d file:///sdcard/pw3-ghost.html -n $BROWSER/.activity.BrowserActivity" >/dev/null 2>&1 \
  || adb_ shell "am start -a android.intent.action.VIEW -d file:///sdcard/pw3-ghost.html" >/dev/null 2>&1
sleep 6

# "${@:-30 20 12}" 会在没有参数时展开成一个词，循环只跑一轮；用数组。
if [ "$#" -gt 0 ]; then THRESHOLDS=("$@"); else THRESHOLDS=(30 20 12); fi
# What to put back at the end: the value the device had before this script,
# not a number written here. The build default moved from 30 to 20 (Panel.h)
# while this line still wrote 30 back, which left the device one A/B run away
# from the shipped behaviour (2026-11-04).
was=$(adb_ shell getprop debug.pw3.epdc.full_every 2>/dev/null | tr -d '\r')
for every in "${THRESHOLDS[@]}"; do
	root "setprop debug.pw3.epdc.full_every $every"
	# 先翻两页让新阈值被读到（16 帧一次），再从这里开始计数
	for _ in 1 2; do adb_ shell input swipe 536 1200 536 300 120 >/dev/null 2>&1; sleep 1; done
	before=$(counters)
	for _ in $(seq "$PAGES"); do adb_ shell input swipe 536 1200 536 300 120 >/dev/null 2>&1; sleep 1; done
	sleep 2
	after=$(counters)
	gl=$(( $(field "$after" gl16) - $(field "$before" gl16) ))
	gc=$(( $(field "$after" gc16) - $(field "$before" gc16) ))
	printf 'full_every=%-3s 翻 %s 页：不闪的刷新 %s 次，整屏清除 %s 次' "$every" "$PAGES" "$gl" "$gc"
	[ "$gc" -gt 0 ] && printf '（约每 %s 页清一次）' "$(( PAGES / gc ))"
	echo
	echo "  现在看一眼屏幕：上一页的字还看得出痕迹吗？清除的那一下接受得了吗？"
	read -r -p "  记一句（直接回车跳过）： " note
	[ -n "$note" ] && echo "  >> $every: $note" >> ghosting-ab-notes.txt
done

root "setprop debug.pw3.epdc.full_every ${was:-\"\"}"
adb_ shell rm -f /sdcard/pw3-ghost.html >/dev/null 2>&1
echo
echo "阈值已恢复成测试前的值（${was:-空，即构建默认}）。"
[ -f ghosting-ab-notes.txt ] && { echo "记下的："; cat ghosting-ab-notes.txt; }
