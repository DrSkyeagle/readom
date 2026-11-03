LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
# The monochrome Noto Emoji, installed in place of AOSP's NotoColorEmoji.ttf.
# NotoColorEmoji is 7.28 MB of CBDT colour bitmaps, and this panel has 16 grey
# levels: every emoji arrives as a mid-grey shape with little contrast against
# the page, and at 14 sp they are hard to tell apart. Outlines keep their
# contrast at every size. See docs/design/performance.md for the renders at
# this device's own text sizes.
#
# The file is fetched, not kept in the source tree (tools/fetch-prebuilts.py,
# device/amazon/pw3/apps/prebuilts.json); PW3-SOURCE.md says where from.
# Named -VF for the same reason AOSP's NotoSansKhmer-VF.ttf is: it carries a
# weight axis, which fonts.xml pins.
LOCAL_MODULE := NotoEmoji-VF.ttf
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_PATH := $(TARGET_OUT)/fonts

# What is installed is the fetched font plus the cmap format 14 subtable
# Paint.hasGlyph() reads for an emoji written with U+FE0F. Without it
# FontFamily::hasGlyph returns false for all 351 of those sequences while the
# font draws them perfectly well, and an application that asks before drawing
# -- EmojiCompat does -- substitutes its own font. The upstream font has no
# such table; AOSP's colour font has all 351. The sequences come from the
# Unicode file AOSP's own font follows, so the result is the fetched bytes and
# in-tree data and nothing else. add-emoji-variation-sequences.py says why in
# full.
pw3_emoji_source := $(LOCAL_PATH)/NotoEmoji-VF.ttf
pw3_emoji_script := $(LOCAL_PATH)/add-emoji-variation-sequences.py
pw3_emoji_sequences := external/unicode/emoji-variation-sequences.txt
pw3_emoji_built := $(call local-generated-sources-dir)/NotoEmoji-VF.ttf

$(pw3_emoji_built): PRIVATE_SCRIPT := $(pw3_emoji_script)
$(pw3_emoji_built): PRIVATE_SOURCE := $(pw3_emoji_source)
$(pw3_emoji_built): PRIVATE_SEQUENCES := $(pw3_emoji_sequences)
$(pw3_emoji_built): $(pw3_emoji_script) $(pw3_emoji_source) $(pw3_emoji_sequences)
	@mkdir -p $(dir $@)
	$(hide) rm -f $@
	$(hide) python3 $(PRIVATE_SCRIPT) --font $(PRIVATE_SOURCE) \
		--sequences $(PRIVATE_SEQUENCES) --output $@

LOCAL_PREBUILT_MODULE_FILE := $(pw3_emoji_built)
include $(BUILD_PREBUILT)

pw3_emoji_source :=
pw3_emoji_script :=
pw3_emoji_sequences :=
pw3_emoji_built :=
