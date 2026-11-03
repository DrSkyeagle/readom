#!/bin/bash
# In the build VM: regenerate the PinyinIME dictionary of the PW3 product.
#
#   build-dict.sh RIME_ICE_DIR OUT.dat
#
# AOSP's 65105-line word list, kept as it is, plus the words of rime-ice's
# "base" list with a weight of 5000 or more that AOSP's list lacks (see
# make_rawdict.py for how they are given frequencies). The user's choice of
# 2026-10-13 among four sizes: device/amazon/pw3/apps/PinyinIME/PW3-SOURCE.md.
# The dictionary builder is the engine's own, unchanged in what it keeps
# (words of up to four characters, frequency 60 and up); only three counters it
# writes with sizeof(size_t) - right on a 32-bit host, and read back as uint32
# by the engine - are written as uint32 here, the build VM being 64-bit.
set -euo pipefail
R=${1:?rime-ice directory}; OUT=${2:?output file}
J=/mnt/port/device/amazon/pw3/apps/PinyinIME/jni; S=$(cd "$(dirname "$0")" && pwd)
echo "0418d5103d8e40c8fff4b686a873ad03e9836cd0419ecae0b7e1e2526312dba0  $R/base.dict.yaml" | sha256sum -c --quiet
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
mkdir "$W/share"; cp "$J"/share/*.cpp "$W/share/"
[ "$(grep -c 'sizeof(size_t)' "$W/share/dictlist.cpp")" = 3 ]
sed -i 's/sizeof(size_t)/sizeof(uint32)/' "$W/share/dictlist.cpp"
( cd "$W/share" && g++ -O2 -w -D___BUILD_MODEL___ -I"$J/include" -o ../builder "$S/builder.cpp" dictbuilder.cpp dictlist.cpp dicttrie.cpp \
    lpicache.cpp mystdlib.cpp ngram.cpp searchutility.cpp spellingtable.cpp spellingtrie.cpp splparser.cpp utf16char.cpp utf16reader.cpp -lpthread )
python3 -I "$S/make_rawdict.py" "$J/data/rawdict_utf16_65105_freq.txt" "$J/data/valid_utf16.txt" "$R" "$W/raw.txt" --base-min-weight 5000 > "$W/stats"
"$W/builder" "$W/raw.txt" "$J/data/valid_utf16.txt" "$W/dict.dat" > "$W/build.log"
grep -E "lemma num" "$W/build.log"
python3 -c "import ast,sys; d=ast.literal_eval(open(sys.argv[1]).read()); print({k: d[k] for k in ('aosp', 'added', 'total')})" "$W/stats"
cp "$W/dict.dat" "$OUT"; sha256sum "$OUT"; stat -c '%s bytes' "$OUT"
