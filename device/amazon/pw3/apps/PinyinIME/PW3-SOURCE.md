# PinyinIME

Unmodified copy of `https://android.googlesource.com/platform/packages/inputmethods/PinyinIME`,
branch `main`, commit `49aebad1c1cfbbcaa9288ffed5161e79e57c3679` (2014-05-14), Apache License 2.0
(`NOTICE`, `MODULE_LICENSE_APACHE2`). Fetched 2026-10-13 with the user's approval: the AOSP 8.1
manifest no longer carries the project and the port has no other Chinese input method.

Changes made for PW3:

- `jni/android/com_android_inputmethod_pinyin_PinyinDecoderService.cpp`: `GetStringChars(fixed_str, false)`
  became `GetStringChars(fixed_str, NULL)`; the AOSP 8.1 clang rejects `false` as a null pointer.
- `res/raw/dict_pinyin.dat`: regenerated (2026-10-13) from the unchanged `jni/data/rawdict_utf16_65105_freq.txt`
  plus the 82,114 (word, reading) pairs of rime-ice's `cn_dicts/base.dict.yaml` with a weight of 5000 or more
  that the AOSP list lacks; 143,446 entries after the builder's own filters, 2,723,666 bytes,
  sha256 `c8ec5e22de6abb31269cd31cfcf0769585c22601fd893a752b50dc0ab94d34b1` (upstream file: 65,105 lines,
  1,068,442 bytes, sha256 `6bf0bbde4e3134cce38d08524a9f4dc1af40435243c8e36b38eb68d1e14462b2`). Regenerate with `tools/pinyin-dict/build-dict.sh`.
  Why: on a fixed list of 110 inputs the first candidate was right 84 times with the upstream dictionary, 97 times
  with this one (librime's luna_pinyin_simp: 88); what was missing were words newer than the list and names.
  The user chose this size over larger ones (up to 891,143 entries, 104 of 110) because the engine holds the
  whole dictionary in memory. Measurements: `work/android-port/build-share/ime-eval/pinyin-dict/README.md`.
  rime-ice: `https://github.com/iDvel/rime-ice`, commit `da1fbe602e38f26db846fa10120ee64c2b0324c0`, GPL-3.0.
  The dictionary is derived from it: a system image handed to anyone else has to carry that licence and this
  provenance for the dictionary.
- User interface for the e-paper panel (2026-10-14), in `src/` and `res/`. Why: the panel takes 134 ms (A2) or 216 ms (DU) for an
  update in which every changed pixel goes between pure black and pure white, and 463 ms plus a second pass when a changed pixel
  is or becomes a grey; the time grows with the area. The upstream skin (dark gradient keys, a balloon per key press, animated
  candidate pages) updated about 910,000 of the screen's 1.55 M pixels per key press, and its candidate bar lay over the
  application. The device has no speaker and no vibrator. Built, not yet seen on the device.
  - Two colours only, `res/values/colors.xml` (`black`, `white`); every other colour resource is gone.
  - Keys: `normal_key_bg.xml`, `normal_key_hl_bg.xml`, `light_key_up_bg.xml`, `light_key_up_hl_bg.xml` are layer-lists of solid
    rectangles with `px` insets (no anti-aliased edge) in place of the 9-patches of the same names: white with a 2 px black frame,
    4 px between the frames of neighbours; pressed, the frame is 8 px; a toggle key that is on (key type 3) is black with a white
    label and, pressed, shows a 6 px white ring. Key type 2 (light off) looks like a normal key. A pressed key keeps the colour
    of its label (`color_highlight` is gone), so that a press changes no glyph pixel. `SoftKeyboardView` invalidates the one key.
  - Key icons are the upstream bitmaps (light, for dark keys), drawn through a `SRC_IN` colour filter in the label colour of
    their key type (`SoftKeyType.mIconFilter`). The arrows of the candidate row are tinted black in the layout.
  - No balloons: `BalloonHint.java`, the attributes `balloon`, `balloon_bg`, `color_balloon`, `icon_popup` and all `*_popup`
    bitmaps are removed. The popup mini keyboard for a long press stays (no keyboard of this package declares one with
    `popup_skb`); it now takes `popup_bg` (`miniskb_bg.xml`, white with a 4 px black frame) as its background, which upstream
    loaded but never applied, and the keyboard under it is no longer dimmed to grey.
  - Key labels are centred on the ascent and descent of their own text size. Upstream placed them with the metrics of the paint's
    default 12 px size, which fitted its 320 px wide screens only.
  - Candidate row: no page animation (the other `CandidateView` is shown at once), black text, the active candidate marked by a
    4 px black line under it, 2 px black separators, a disabled arrow not drawn instead of faint.
  - Composing string: a row of fixed height at the top of the candidates view (`candidates_frame.xml`) in place of the floating
    `PopupWindow` (`floating_container.xml`, `PopupTimer`); black on white, the highlighted string white on black. Its text size
    follows the row height (`composing_height` is gone). The frame has a 2 px black line on top (`candidates_frame_bg.xml`).
  - The candidates view is shown whenever the soft keyboard is, with or without content, and `PinyinIME.onComputeInsets` puts the
    end of the application's content at its top (`contentTopInsets = visibleTopInsets`, not in fullscreen mode).
    `dismissCandidateWindow` is gone; `resetCandidateWindow` clears the content as before.
  - Sizes in `Environment.java`, portrait: key row 0.075 of the screen height (was 0.105), candidate row 0.055 (was 0.084),
    composing row 0.03 (landscape 0.045); key text 0.05 of the width (was 0.075), function key text 0.036 (was 0.055).
  - Sound and vibration are removed: `SoundManager.java`, the two settings and their strings, the `VIBRATE` permission. Upstream
    played a sound per key by default.
- Candidate row filled (2026-10-14, user: the candidates were crowded together and did not spread over the row):
  `DecodingInfo.MAX_PAGE_SIZE_DISPLAY` 10 -> 30, so that the row's width, not a phone's count, ends a page and the
  space left over is spread over the candidates; `candidate_margin_left_right` (portrait) 3dip -> 8dip, a wider
  gap and target per candidate.
- Key icons `delete_icon`, `enter_icon`, `search_icon`, `space_icon`: the bitmaps (light glyphs with a shadow, for the
  dark keyboard) replaced by vector paths of the Material Design icons (Apache License 2.0; backspace outlined,
  keyboard return, search, space bar), 32dp. Tinted black, the shadows had made the glyphs look bold (user,
  2026-10-14: the DEL key was too thick).
- Name (2026-10-15, user): `ime_name`, `ime_settings_activity_name` and `setting` in `res/values/strings.xml` say
  独角兽输入法 instead of 谷歌拼音输入法; this is not Google's input method. The package name stays
  `com.android.inputmethod.pinyin`, so the choice of input method on a device survives the update.
- A released key takes effect one frame later (2026-10-16, `SkbContainer.responseKeyEventAfterRelease`): the frame
  that shows the key released comes first, the candidate row and composing string change in the next. Both views are
  in one window, and a frame reports one changed area, so the key and the candidate row went to the panel as one
  1000×398 area in the grey-level waveform (463 ms); measured on Test V40.1 with the EPDC log, a key held for 120 ms
  showed pressed in 138 ms (A2, the key alone) and released only with that update. Apart, the key is again a
  black-and-white update of its own, and a key pressed next no longer waits for the candidate row.
- The Enter key moved under the delete key (2026-10-30, user, `res/xml/skb_template1.xml` key id 1,
  `start_pos_x` 80%p to 85.715%p and `width` 20%p to 14.285%p). Upstream puts Enter at 80%p–100%p while M
  above it runs 75.516%p–85.721%p, so M's right 5.715%p sits directly over Enter's left 5.715%p: 61 px on
  this panel, with only the key drawable's margin between the rows. The user hit the wrong one of the two
  with a finger. Measured on the device first, so that the layout rather than the hit test was what moved:
  at x=880, taps at y=1200, 1235, 1250 and 1258 all reached M and taps from y=1265 down reached Enter. The
  period key of every bottom row (`skb_template1.xml` id 7, and the one written out in `skb_qwerty.xml`,
  `skb_sym1.xml`, `skb_sym2.xml` and `skb_smiley.xml`) grows from 14.696%p to 20.411%p to fill what Enter
  gives up, so each key in the bottom row now lines up with the one above it. The phone keyboard's own
  Enter (id 2) is a different key and is unchanged.
