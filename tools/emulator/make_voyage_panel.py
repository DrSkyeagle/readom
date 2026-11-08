#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate a synthetic Kindle Voyage (Wario / icewine) EPDC panel flash image
and the two waveform files readom's device-data path needs, for the WSL QEMU
emulator.

Outputs (default directory: <repo>/../readom-port/emu-voyage/panel):

  panel.img      exactly 512 KiB, an mx25l4005a (4 Mbit) NOR image:
                 * WBF (waveform binary file) at 0x886, file_length 0x10000
                 * panel information area at 0x70000 (E Ink alphabet encoded)
  synth.wrf.gz   gzip(stored) of the 2048-byte WRF the stock 5.13.6
                 mxc_epdc_fb loads through the udev waveform.sh helper from the
                 hidden waveform FAT (source=stored)
  synth.pw3.gz   gzip(stored) of the PW3WFP1 envelope readom's
                 boot/devicedata/devicedata.c dd_envelope() builds from the
                 panel's own WBF plus that WRF (android-core patch 0700)

The generator is deterministic: every byte is derived from the constants below,
the gzip is the stored-block form dd_gzip_stored() emits (no mtime, no name),
so repeated runs are byte-identical.

Sources for every constant (all checked against the 5.13.6 GPL kernel tree,
linux-3.0.35-stock, not guessed):

  drivers/video/mxc/mxc_epdc_fb.c            struct waveform_data_header
                                             (bit-field byte layout)
  drivers/video/mxc/mxc_epdc_fb_lab126.c     DEFAULT_WFM_ADDR, WFM_HDR_SIZE,
                                             panel addrs (0x886/0x70000),
                                             PNL_BASE_*, panel_data_translate()
                                             alphabet, sum8/crc32 checksums,
                                             panel_get_id/vcom/bcd, mode tables
  arch/arm/mach-mx6/board-mx6sl_wario.c      ED060TC1_3CE vmode ("ED060TC1-3CE"),
                                             EBA barcode -> PANEL_MODE_ED060TC1_3CE
  patches/android-core/0400 + 0700            android_wf_header_valid,
                                             android_wrf_layout, android_wf_container,
                                             android_wf_matches_panel
  boot/devicedata/devicedata.c               dd_envelope, dd_gzip_stored

Usage:
  python3 make_voyage_panel.py [--out DIR] [--quiet]
"""

import argparse
import hashlib
import os
import struct
import sys
import zlib

# ---------------------------------------------------------------------------
# Geometry and offsets (stock 5.13.6, waveform_addrs_AA_AC_AE_BD)
# ---------------------------------------------------------------------------

PANEL_FLASH_SIZE = 0x80000        # mx25l4005a = 4 Mbit = 512 KiB
WFM_HDR_SIZE = 0x30               # struct waveform_data_header
DEFAULT_WFM_ADDR = 0x886          # waveform_addrs_AA_AC_AE_BD.waveform_addr
PNL_INFO_ADDR = 0x70000           # waveform_addrs_AA_AC_AE_BD.pnl_info_addr
WAVEFORM_LEN = PNL_INFO_ADDR - DEFAULT_WFM_ADDR      # 456570 bytes
WBF_FILE_LENGTH = 0x10000         # 65536 bytes, the WBF's own file_length
XWI_OFFSET = 0x200                # header xwia field: XWI lives here in the WBF
XWI_STRING = b"V220_EBAL_ED060TC1_3CE"

# struct waveform_data_header byte layout. The kernel declares it as
#   unsigned int checksum:32; file_length:32; serial_number:32;
#   run_type:8; fpl_platform:8; fpl_lot:16;
#   mode_version:8; wf_version:8; wf_subversion:8; wf_type:8;
#   panel_size:8; amepd_part_number:8; wf_revision:8; frame_rate:8;
#   reserved1_0:8; vcom_shifted:8; reserved1_1:16;
#   xwia:24; cs1:8; wmta:24; fvsn:8; luts:8; mc:8; trc:8;
#   advanced_wfm_flags:8; eb:8; sb:8; reserved0_1:8; reserved0_2:8;
#   reserved0_3:8; reserved0_4:8; reserved0_5:8; cs2:8;
# which GCC packs into twelve 32-bit words without crossing a word boundary.
WBF_FIELDS = {
    "file_length": 0x10000,
    "serial": 0xBEEF,
    "run_type": 0,
    "fpl_platform": 3,
    "fpl_lot": 0,
    "mode_version": 7,
    "wf_version": 0,
    "wf_subversion": 0,
    "wf_type": 0x4B,            # EINK_WAVEFORM_TYPE_AC
    "panel_size": 60,
    "amepd_part_number": 1,
    "wf_revision": 0,
    "frame_rate": 0,
    "reserved1_0": 0,
    "vcom_shifted": 0,
    "reserved1_1": 0,
    "xwia": XWI_OFFSET,
    "wmta": 58,
    "fvsn": 1,
    "luts": 0,
    "mc": 6,
    "trc": 1,
    "advanced_wfm_flags": 0,
    "eb": 0xFF,
    "sb": 0xFC,
    "reserved0": 0,
}

# TRT (temperature range table): trc + 1 = 2 entries plus a terminator byte,
# always in the WBF and in the first 51 bytes of the WRF.
TRT = bytes([0x00, 0x28, 0x00])

# WRF payload: 8 x u64 mode pointers (mc + advanced + 1 entries). Each points,
# relative to the payload (the WRF byte at wv_data_offs), at the start of that
# mode's waveform data. 0x200 is the XWI record the stock driver's
# eink_get_wfm_human_version() dereferences at
# ((u64*)payload)[mc + 1 + advanced] and the Android layout checks.
WRF_POINTERS = (0x40, 0x80, 0xC0, 0x100, 0x140, 0x180, 0x1C0, 0x200)
WRF_XWI = b"V220_EBAL_3CE"        # 13 bytes, preceded by its length byte at 0x200

WRF_SIZE = 2048

# Panel information area (E Ink custom alphabet, panel_data_translate()).
# Offsets are within the 0x70000 page; "+ 0x20" is the 0x20 byte the task
# specifies after each string: it is not in the alphabet, so
# panel_data_translate(strip=true) turns it into the string terminator. 0x00
# must NOT be used there: in this alphabet 0x00 decodes to '0'.
PANEL_INFO = {
    0x00: (b"ED060TC1", None),            # PNL_BASE_PART_NUMBER (16 bytes)
    0x10: (b"-2.05", None),               # PNL_BASE_VCOM (5 bytes exactly)
    0x20: (b"V220E_EBAL_ED060TC1", None),  # PNL_BASE_WAVEFORM (23 bytes)
    0x40: (b"A123", None),                # PNL_BASE_FPL (panel_get_id reads +1..+3)
    0x50: (b"EBA", 0x20),                 # PNL_BASE_BCD (33 bytes)
    0x80: (b"1072x1448", 0x20),           # PNL_BASE_RESOLUTION (16 bytes)
    0x90: (b"107.2x144.8", 0x20),         # PNL_BASE_DIMENSIONS (32 bytes)
    0xB0: (b"3.3", 0x20),                 # PNL_BASE_VDD (16 bytes)
    0x300: (b"1.0", 0x20),                # PNL_BASE_VERSION (16 bytes)
}
PANEL_INFO_FILL = 0xFF
PANEL_ERASED = 0xFF


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

def le16(v):
    return struct.pack("<H", v)


def le24(v):
    return struct.pack("<I", v)[:3]


def le32(v):
    return struct.pack("<I", v)


def le64(v):
    return struct.pack("<Q", v)


def get32(b, off=0):
    return struct.unpack_from("<I", b, off)[0]


def get64(b, off=0):
    return struct.unpack_from("<Q", b, off)[0]


def sum8(data):
    return sum(data) & 0xFF


def crc32(data):
    """zlib's CRC-32, the same polynomial/initial value the stock
    update_crc()/crc32() and readom's dd_crc32() use."""
    return zlib.crc32(data) & 0xFFFFFFFF


def wbf_header_crc(wbf):
    """eink_get_computed_waveform_checksum(): CRC-32 over the whole WBF with
    the embedded checksum field (bytes 0..3) read as zero."""
    return crc32(b"\x00\x00\x00\x00" + wbf[4:])


def align8(n):
    return (n + 7) & ~7


# ---------------------------------------------------------------------------
# E Ink "custom alphabet" for the panel information page
# ---------------------------------------------------------------------------

def encode_panel_string(text):
    """panel_data_translate() reversed (mxc_epdc_fb_lab126.c:2169):
         '0'..'9' -> 0x00..0x09, '_' -> 0x0A, '.' -> 0x0B, '-' -> 0x0C,
         'a'..'z' -> 0xCB..0xE4, 'A'..'Z' -> 0xE5..0xFE
    """
    out = bytearray()
    for ch in text:
        if 0x30 <= ch <= 0x39:
            out.append(ch - 0x30)
        elif ch == 0x5F:
            out.append(0x0A)
        elif ch == 0x2E:
            out.append(0x0B)
        elif ch == 0x2D:
            out.append(0x0C)
        elif 0x61 <= ch <= 0x7A:
            out.append(0xCB + (ch - 0x61))
        elif 0x41 <= ch <= 0x5A:
            out.append(0xE5 + (ch - 0x41))
        else:
            raise ValueError("character %r is not in the panel alphabet" % chr(ch))
    return bytes(out)


def decode_panel_string(raw, strip):
    """panel_data_translate() as the stock driver calls it."""
    out = bytearray()
    for i, b in enumerate(raw):
        if 0xCB <= b <= 0xE4:
            out.append(0x61 + (b - 0xCB))
        elif 0xE5 <= b <= 0xFE:
            out.append(0x41 + (b - 0xE5))
        elif b <= 0x09:
            out.append(0x30 + b)
        elif b == 0x0A:
            out.append(0x5F)
        elif b == 0x0B:
            out.append(0x2E)
        elif b == 0x0C:
            out.append(0x2D)
        else:
            if strip:
                break
            out.append(0x21)  # PNL_CHAR_UNKNOWN '!'
    return bytes(out)


# ---------------------------------------------------------------------------
# WBF (panel waveform binary file)
# ---------------------------------------------------------------------------

def build_wbf_header():
    h = bytearray(WFM_HDR_SIZE)
    h[0:4] = le32(0)                    # checksum, filled in last
    h[4:8] = le32(WBF_FIELDS["file_length"])
    h[8:12] = le32(WBF_FIELDS["serial"])
    h[12] = WBF_FIELDS["run_type"]
    h[13] = WBF_FIELDS["fpl_platform"]
    h[14:16] = le16(WBF_FIELDS["fpl_lot"])
    h[16] = WBF_FIELDS["mode_version"]
    h[17] = WBF_FIELDS["wf_version"]
    h[18] = WBF_FIELDS["wf_subversion"]
    h[19] = WBF_FIELDS["wf_type"]
    h[20] = WBF_FIELDS["panel_size"]
    h[21] = WBF_FIELDS["amepd_part_number"]
    h[22] = WBF_FIELDS["wf_revision"]
    h[23] = WBF_FIELDS["frame_rate"]
    h[24] = WBF_FIELDS["reserved1_0"]
    h[25] = WBF_FIELDS["vcom_shifted"]
    h[26:28] = le16(WBF_FIELDS["reserved1_1"])
    h[28:31] = le24(WBF_FIELDS["xwia"])
    # cs1 (byte 31) = sum8 over bytes 0..30, cs2 (byte 47) = sum8 over bytes
    # 32..46. They are only read when file_length == 0; a writer still fills
    # them. They are computed over the checksum-zeroed header, then the CRC-32
    # is taken over the finished file (with bytes 0..3 still zero) as
    # eink_get_computed_waveform_checksum() does, and stored at 0..3.
    h[31] = sum8(h[0:31])
    h[32:35] = le24(WBF_FIELDS["wmta"])
    h[35] = WBF_FIELDS["fvsn"]
    h[36] = WBF_FIELDS["luts"]
    h[37] = WBF_FIELDS["mc"]
    h[38] = WBF_FIELDS["trc"]
    h[39] = WBF_FIELDS["advanced_wfm_flags"]
    h[40] = WBF_FIELDS["eb"]
    h[41] = WBF_FIELDS["sb"]
    h[42:47] = bytes(5)                 # reserved0_1..reserved0_5
    h[47] = sum8(h[32:47])
    return bytes(h)


def build_wbf():
    """Return exactly WBF_FILE_LENGTH bytes: header + TRT, XWI record at
    0x200, everything else zero (this synthetic WBF is read for its header,
    checksum and XWI only; the executable waveform data is the WRF)."""
    wbf = bytearray(WBF_FILE_LENGTH)
    wbf[0:WFM_HDR_SIZE] = build_wbf_header()
    off = WFM_HDR_SIZE
    wbf[off:off + len(TRT)] = TRT
    off += len(TRT)                     # 51: WBF payload start
    assert off == 51
    xwi = bytes([len(XWI_STRING)]) + XWI_STRING
    assert XWI_OFFSET + len(xwi) <= WBF_FILE_LENGTH
    wbf[XWI_OFFSET:XWI_OFFSET + len(xwi)] = xwi
    # CRC-32 over the whole file with the embedded checksum field zeroed.
    wbf[0:4] = le32(crc32(wbf))
    return bytes(wbf)


# ---------------------------------------------------------------------------
# WRF (Freescale waveform the stock driver loads from the hidden FAT)
# ---------------------------------------------------------------------------

def build_wrf(wbf):
    """2048-byte WRF: the same 51-byte header+TRT prefix as the WBF, then the
    mode-pointer table at payload offset 0 and the XWI record at 0x200."""
    prefix = WFM_HDR_SIZE + (WBF_FIELDS["trc"] + 1) + 1
    assert prefix == 51
    off = prefix
    assert prefix <= len(wbf)

    wrf = bytearray(WRF_SIZE)
    wrf[0:prefix] = wbf[0:prefix]
    payload = prefix
    for i, p in enumerate(WRF_POINTERS):
        wrf[payload + i * 8:payload + i * 8 + 8] = le64(p)
    xwi_record = bytes([len(WRF_XWI)]) + WRF_XWI
    wrf[payload + 0x200:payload + 0x200 + len(xwi_record)] = xwi_record
    return bytes(wrf)


# ---------------------------------------------------------------------------
# Panel flash image
# ---------------------------------------------------------------------------

def build_panel_info_page():
    page = bytearray(0x80000 - PNL_INFO_ADDR)   # 0x10000; image extends to 0x80000
    page[:] = bytes([PANEL_INFO_FILL]) * len(page)
    for offset, (text, term) in PANEL_INFO.items():
        raw = encode_panel_string(text)
        if term is not None:
            raw += bytes([term])
        end = offset + len(raw)
        assert end <= len(page), "panel info field 0x%x overflows" % offset
        page[offset:end] = raw
    return bytes(page)


def build_panel_img(wbf):
    img = bytearray([PANEL_ERASED]) * PANEL_FLASH_SIZE
    assert len(wbf) == WBF_FILE_LENGTH
    assert DEFAULT_WFM_ADDR + len(wbf) <= PNL_INFO_ADDR
    img[DEFAULT_WFM_ADDR:DEFAULT_WFM_ADDR + len(wbf)] = wbf
    page = build_panel_info_page()
    img[PNL_INFO_ADDR:PNL_INFO_ADDR + len(page)] = page
    return bytes(img)


def panel_waveform_export(panel_img):
    """The bytes /proc/eink/panel/waveform/data returns: the driver's whole
    waveform region (waveform_len = 0x70000 - 0x886)."""
    assert len(panel_img) == PANEL_FLASH_SIZE
    return panel_img[DEFAULT_WFM_ADDR:PNL_INFO_ADDR]


# ---------------------------------------------------------------------------
# gzip(stored) — byte-for-byte the same member dd_gzip_stored() emits
# ---------------------------------------------------------------------------

def gzip_stored(data):
    out = bytearray(b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff")
    at = 0
    while True:
        n = min(65535, len(data) - at)
        block = bytearray(5)
        block[0] = 1 if at + n == len(data) else 0
        block[1] = n & 0xFF
        block[2] = (n >> 8) & 0xFF
        block[3] = ~n & 0xFF
        block[4] = (~n >> 8) & 0xFF
        out += block
        out += data[at:at + n]
        at += n
        if at >= len(data):
            break
    out += le32(crc32(data))
    out += le32(len(data) & 0xFFFFFFFF)
    return bytes(out)


# ---------------------------------------------------------------------------
# Replicas of the accept rules (readom 0700 + devicedata + stock driver)
# ---------------------------------------------------------------------------

class CheckError(AssertionError):
    pass


def check(cond, message):
    if not cond:
        raise CheckError(message)


ANDROID_WF_ENVELOPE_SIZE = 64
WFM_TEMP_BUF_LEN = 10 * 1024 * 1024


def android_wf_header_valid(h):
    limit = {
        0x15: 0x30000 - DEFAULT_WFM_ADDR,
        0x2B: 0x30000 - DEFAULT_WFM_ADDR,
        0x3C: 0x70000 - DEFAULT_WFM_ADDR,
        0x4B: 0x70000 - DEFAULT_WFM_ADDR,
        0x4C: 0x70000 - DEFAULT_WFM_ADDR,
        0x50: 0x70000 - DEFAULT_WFM_ADDR,
    }.get(h[19])
    if limit is None:
        return False
    if h[16] not in (7, 18, 19, 24, 25):
        return False
    file_length = get32(h, 4)
    return WFM_HDR_SIZE < file_length <= limit


def android_wrf_layout(wbf, wrf):
    """android_wrf_layout() from patch 0700, returns (vc_offset, vc_length)."""
    check(len(wbf) >= WFM_HDR_SIZE, "wbf shorter than header")
    check(android_wf_header_valid(wbf), "wbf header rejected by android_wf_header_valid")
    check(get32(wbf, 4) == len(wbf), "wbf file_length != wbf length")
    check(wbf_header_crc(wbf) == get32(wbf, 0), "wbf CRC-32 mismatch")
    modes = wbf[37] + 1
    temps = wbf[38] + 1
    off = WFM_HDR_SIZE + temps + 1
    check(off < len(wbf), "wbf payload offset past end")
    check(off < len(wrf), "wrf shorter than payload offset")
    check(modes >= 3, "fewer than 3 waveform modes")
    check(temps >= 2, "fewer than 2 temperature ranges")
    check(wbf[39] <= 3, "advanced_wfm_flags > 3")
    check(wbf[:off] == wrf[:off], "wrf does not start with the wbf header+TRT")
    payload = wrf[off:]
    bytes_ = len(payload)
    advanced = wbf[39]
    entries = modes + advanced + 1
    check(entries > 0 and entries <= bytes_ // 8, "pointer entries exceed payload")
    for i in range(entries):
        pointer = get64(payload, i * 8)
        check(pointer >= entries * 8 and pointer < bytes_,
              "mode pointer %d = 0x%x out of range" % (i, pointer))
    last = get64(payload, (entries - 1) * 8)
    check(payload[last] <= bytes_ - last - 1, "XWI length runs past the payload")
    vc_offset = vc_length = 0
    if advanced in (1, 3):
        start = get64(payload, modes * 8)
        end = get64(payload, (modes + 1) * 8)
        check(start < end and end <= bytes_, "VC range invalid")
        record = 8  # sizeof(struct epd_vc_data) == 8 on this ABI
        check(end - start >= modes * temps * record, "VC range too small")
        for i in range(modes * temps):
            rec = payload[start + i * record:start + (i + 1) * record]
            check(sum(rec[:-1]) & 0xFF == rec[-1], "VC record checksum mismatch")
        vc_offset, vc_length = off + start, end - start
    return vc_offset, vc_length


def android_wf_container(envelope):
    """android_wf_container() from patch 0700."""
    check(len(envelope) >= ANDROID_WF_ENVELOPE_SIZE, "envelope shorter than header")
    check(len(envelope) <= WFM_TEMP_BUF_LEN, "envelope larger than the temp buffer")
    check(envelope[:8] == b"PW3WFP1\x00", "envelope magic mismatch")
    check(get32(envelope, 8) == 1, "envelope version != 1")
    check(get32(envelope, 12) == ANDROID_WF_ENVELOPE_SIZE, "envelope header size")
    check(envelope[40:64] == bytes(24), "envelope reserved bytes not zero")
    header = bytearray(envelope[:ANDROID_WF_ENVELOPE_SIZE])
    header[36:40] = bytes(4)
    check(crc32(header) == get32(envelope, 36), "envelope header CRC-32 mismatch")
    raw_length = get32(envelope, 16)
    wrf_length = get32(envelope, 20)
    wrf_offset = get32(envelope, 24)
    check(WFM_HDR_SIZE < raw_length <= 0x70000 - DEFAULT_WFM_ADDR, "raw_length invalid")
    check(wrf_length > WFM_HDR_SIZE, "wrf_length invalid")
    check(wrf_offset == ANDROID_WF_ENVELOPE_SIZE + align8(raw_length),
          "wrf_offset != header + aligned raw_length")
    check(wrf_offset <= len(envelope), "wrf_offset past end")
    check(wrf_length == len(envelope) - wrf_offset, "wrf_length does not match file")
    check(envelope[ANDROID_WF_ENVELOPE_SIZE + raw_length:wrf_offset] ==
          bytes(wrf_offset - ANDROID_WF_ENVELOPE_SIZE - raw_length),
          "raw_length .. wrf_offset padding not zero")
    wbf = envelope[ANDROID_WF_ENVELOPE_SIZE:ANDROID_WF_ENVELOPE_SIZE + raw_length]
    wrf = envelope[wrf_offset:]
    check(crc32(wbf) == get32(envelope, 28), "envelope WBF CRC-32 mismatch")
    check(crc32(wrf) == get32(envelope, 32), "envelope WRF CRC-32 mismatch")
    return wbf, wrf, android_wrf_layout(wbf, wrf)


def dd_envelope(panel, wrf):
    """boot/devicedata/devicedata.c dd_envelope(), byte for byte."""
    check(len(panel) >= 48, "panel waveform truncated")
    size = get32(panel, 4)
    limit = (0x30000 - DEFAULT_WFM_ADDR
             if panel[19] in (0x15, 0x2B)
             else WAVEFORM_LEN if panel[19] in (0x3C, 0x4B, 0x4C, 0x50) else 0)
    check(size > 48, "panel waveform header unsupported (size)")
    check(size <= limit, "panel waveform header unsupported (limit)")
    check(size <= len(panel), "panel waveform header unsupported (length)")
    check(panel[16] in (7, 18, 19, 24, 25), "panel waveform header unsupported (version)")
    check(len(panel) in (size, limit), "unexpected panel waveform export size")
    check(crc32(b"\x00\x00\x00\x00" + panel[4:size]) == get32(panel, 0),
          "panel waveform checksum mismatch")
    modes = panel[37] + 1
    temps = panel[38] + 1
    advanced = panel[39]
    prefix = 48 + temps + 1
    check(modes >= 3 and temps >= 2 and advanced <= 3 and prefix < size,
          "panel waveform dimensions invalid")
    offset = ANDROID_WF_ENVELOPE_SIZE + align8(size)
    check(len(wrf) > prefix, "stock waveform does not belong to this panel (short)")
    check(len(wrf) <= WFM_TEMP_BUF_LEN - offset,
          "stock waveform does not belong to this panel (long)")
    check(wrf[:prefix] == panel[:prefix], "stock waveform does not belong to this panel")
    head = bytearray(ANDROID_WF_ENVELOPE_SIZE)
    head[0:8] = b"PW3WFP1\x00"
    head[8:12] = le32(1)
    head[12:16] = le32(ANDROID_WF_ENVELOPE_SIZE)
    head[16:20] = le32(size)
    head[20:24] = le32(len(wrf))
    head[24:28] = le32(offset)
    head[28:32] = le32(crc32(panel[:size]))
    head[32:36] = le32(crc32(wrf))
    head[36:40] = le32(crc32(head))
    return (bytes(head) + panel[:size] + bytes(offset - ANDROID_WF_ENVELOPE_SIZE - size) +
            wrf)


def gzip_decompress_stored_member(gz):
    """Replicate the stock gunzip() wrapper: skip the fixed 10-byte gzip
    header (flags == 0, ctype == 8), then raw-inflate; verify the trailer."""
    check(len(gz) >= 18, "gzip too short")
    check(gz[0:2] == b"\x1f\x8b", "gzip magic")
    check(gz[2] == 8, "gzip method != deflate")
    check(gz[3] & 0xE0 == 0, "gzip reserved flags set")
    check(gz[3] == 0, "gzip has optional fields the stored writer does not emit")
    body = gz[10:-8]
    raw = zlib.decompress(body, -zlib.MAX_WBITS)
    check(len(gz) >= 18 + len(body), "gzip trailer missing")
    check(get32(gz, len(gz) - 8) == crc32(raw), "gzip CRC-32 mismatch")
    check(get32(gz, len(gz) - 4) == len(raw) & 0xFFFFFFFF, "gzip ISIZE mismatch")
    return raw


def stock_process_fw_checks(wrf, wrf_gz):
    """The rules the shipped 5.13.6 mxc_epdc_fb_process_fw() and its helpers
    apply to the waveform the udev waveform.sh helper feeds it."""
    check(gzip_decompress_stored_member(wrf_gz) == wrf,
          "synth.wrf.gz does not decompress to the WRF")
    h = wrf
    trt_entries = h[38] + 1                       # wv_file->wdh.trc + 1
    wv_data_offs = WFM_HDR_SIZE + trt_entries + 1
    check(trt_entries >= 2, "trt table too small")
    check(wv_data_offs <= len(wrf), "waveform data offset past end")
    check((h[36] & 0xC) in (0x0, 0x4), "luts bit depth field invalid")
    # eink_get_wfm_voltage_control(): reads le64 at payload[0] and the
    # advanced_wfm_flags switch; advanced 0 means "no voltage control".
    payload = wrf[wv_data_offs:]
    modes = h[37] + 1
    check(get64(payload, 0) > 8 * modes, "first mode pointer <= pointer table")
    check(h[39] <= 3, "advanced_wfm_flags > 3")
    # eink_get_wfm_human_version(): last pointer is mc + 1 + advanced.
    xwi = get64(payload, (modes + h[39]) * 8)
    check(xwi < len(payload), "XWI pointer past end")
    length = payload[xwi]
    check(xwi + 1 + length <= len(payload), "XWI runs past end")
    human = payload[xwi + 1:xwi + 1 + length]
    return human


def panel_driver_checks(panel_img):
    """panel_get_format()/panel_get_bcd()/panel_get_vcom()/panel_get_id()/
    eink_get_pnl_wfm_human_version() as the stock driver runs them against the
    panel flash image."""
    wbf = panel_img[DEFAULT_WFM_ADDR:DEFAULT_WFM_ADDR + WBF_FILE_LENGTH]
    info = panel_img[PNL_INFO_ADDR:]
    check(wbf[19] in (0x15, 0x2B, 0x3C, 0x4B, 0x4C, 0x50),
          "panel_get_format() would BUG() on wf_type 0x%02x" % wbf[19])
    check(wbf_header_crc(wbf) == get32(wbf, 0), "WBF CRC-32 mismatch")
    check(android_wf_header_valid(wbf), "WBF header not acceptable to 0400 patch")
    bcd = decode_panel_string(info[0x50:0x50 + 33], strip=True).decode()
    check(bcd == "EBA", "panel bcd %r != EBA" % bcd)

    vcom = decode_panel_string(info[0x10:0x10 + 5], strip=True).decode()
    check(vcom == "-2.05", "panel vcom %r != -2.05" % vcom)
    check(vcom[0] == "-" and vcom[2] == "." and vcom[1:2].isdigit()
          and vcom[3:].isdigit(), "VCOM is not -d.dd volts")

    pt = decode_panel_string(info[0x00:0x10], strip=True).decode()
    check(pt == "ED060TC1", "part number %r != ED060TC1" % pt)
    wf = decode_panel_string(info[0x20:0x20 + 23], strip=False)
    check(wf[:19] == b"V220E_EBAL_ED060TC1", "waveform name %r" % wf)
    fpl = decode_panel_string(info[0x40:0x44], strip=False)
    check(fpl == b"A123", "FPL bytes %r != A123" % fpl)
    res = decode_panel_string(info[0x80:0x80 + 16], strip=True).decode()
    check(res == "1072x1448", "resolution %r" % res)
    dim = decode_panel_string(info[0x90:0x90 + 32], strip=True).decode()
    check(dim == "107.2x144.8", "dimensions %r" % dim)
    vdd = decode_panel_string(info[0xB0:0xB0 + 16], strip=True).decode()
    check(vdd == "3.3", "vdd %r" % vdd)
    ver = decode_panel_string(info[0x300:0x300 + 16], strip=True).decode()
    check(ver == "1.0", "panel info version %r" % ver)

    # panel_get_id(): V220 + '_' + fpl[1:4] + '_' + waveform[10:12] + '_' + part number
    pid = bytearray()
    pid += wf[0:4]
    pid += b"_"
    pid += fpl[1:4]
    pid += b"_"
    pid += wf[10:12]
    pid += b"_"
    pid += pt.encode()
    check(b"!" not in pid, "panel_get_id() would report an unrecognised character")
    # eink_get_pnl_wfm_human_version(): the flash fallback reads
    # waveform_addr + xwia, first byte is the length.
    xwia = get32(wbf, 28) & 0xFFFFFF
    check(xwia >= WFM_HDR_SIZE, "xwia inside the header")
    xwi_len = wbf[xwia]
    xwi = wbf[xwia + 1:xwia + 1 + xwi_len].decode()
    check(xwi == XWI_STRING.decode(), "panel XWI %r" % xwi)
    # panel_get_info(header_only=false) / eink_waveform_valid()
    check(48 < get32(wbf, 4) <= WAVEFORM_LEN, "eink_waveform_valid() would reject")
    return {"bcd": bcd, "vcom": vcom, "panel_id": pid.decode(), "xwi": xwi}


def verify(panel_img, wbf, wrf, wrf_gz, pw3_gz):
    """Assert every rule the consumer applies, then return a summary dict."""
    check(len(panel_img) == PANEL_FLASH_SIZE, "panel.img is not 512 KiB")
    check(len(wbf) == WBF_FILE_LENGTH, "WBF is not 65536 bytes")
    check(len(wrf) == WRF_SIZE, "WRF is not 2048 bytes")

    header = {
        "file_length": get32(wbf, 4),
        "serial": get32(wbf, 8),
        "run_type": wbf[12],
        "fpl_platform": wbf[13],
        "fpl_lot": struct.unpack_from("<H", wbf, 14)[0],
        "mode_version": wbf[16],
        "wf_version": wbf[17],
        "wf_subversion": wbf[18],
        "wf_type": wbf[19],
        "panel_size": wbf[20],
        "amepd_part_number": wbf[21],
        "xwia": get32(wbf, 28) & 0xFFFFFF,
        "wmta": get32(wbf, 32) & 0xFFFFFF,
        "fvsn": wbf[35],
        "luts": wbf[36],
        "mc": wbf[37],
        "trc": wbf[38],
        "advanced_wfm_flags": wbf[39],
        "eb": wbf[40],
        "sb": wbf[41],
        "cs1": wbf[31],
        "cs2": wbf[47],
        "checksum": get32(wbf, 0),
    }
    expected = {
        "file_length": 0x10000, "serial": 0xBEEF, "run_type": 0,
        "fpl_platform": 3, "fpl_lot": 0, "mode_version": 7,
        "wf_version": 0, "wf_subversion": 0, "wf_type": 0x4B,
        "panel_size": 60, "amepd_part_number": 1, "xwia": 0x200,
        "wmta": 58, "fvsn": 1, "luts": 0, "mc": 6, "trc": 1,
        "advanced_wfm_flags": 0, "eb": 0xFF, "sb": 0xFC,
    }
    for key, value in expected.items():
        check(header[key] == value,
              "WBF header %s = 0x%x, expected 0x%x" % (key, header[key], value))

    panel_export = panel_waveform_export(panel_img)
    check(len(panel_export) == WAVEFORM_LEN, "panel waveform export size")

    panel_side = panel_driver_checks(panel_img)
    human = stock_process_fw_checks(wrf, wrf_gz)
    check(human == WRF_XWI, "WRF human version %r" % human)

    envelope = dd_envelope(panel_export, wrf)
    check(gzip_stored(envelope) == pw3_gz,
          "synth.pw3.gz != gzip(dd_envelope(panel export, wrf))")
    env_wbf, env_wrf, (vc_offset, vc_length) = android_wf_container(pw3_gz_decompress(pw3_gz))
    check(env_wbf == wbf, "envelope WBF != generated WBF")
    check(env_wrf == wrf, "envelope WRF != generated WRF")
    check(vc_offset == 0 and vc_length == 0, "advanced=0 must have no VC data")

    return {
        "header": header,
        "panel_side": panel_side,
        "wrf_human_version": human.decode(),
        "envelope": envelope,
    }


def pw3_gz_decompress(gz):
    return gzip_decompress_stored_member(gz)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

OUTPUT_NAMES = ("panel.img", "synth.wrf.gz", "synth.pw3.gz")


def default_out():
    here = os.path.dirname(os.path.abspath(__file__))
    # <repo>/tools/emulator -> <repo>/../readom-port/emu-voyage/panel
    repo = os.path.dirname(os.path.dirname(here))
    return os.path.join(os.path.dirname(repo), "readom-port", "emu-voyage", "panel")


def write_atomic(path, data):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=None,
                        help="output directory (default: readom-port/emu-voyage/panel next to this repo)")
    parser.add_argument("--verify-only", action="store_true",
                        help="only re-verify the files already in --out")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)

    out = args.out or default_out()
    paths = {name: os.path.join(out, name) for name in OUTPUT_NAMES}

    if args.verify_only:
        with open(paths["panel.img"], "rb") as f:
            panel_img = f.read()
        with open(paths["synth.wrf.gz"], "rb") as f:
            wrf_gz = f.read()
        with open(paths["synth.pw3.gz"], "rb") as f:
            pw3_gz = f.read()
        wbf = panel_img[DEFAULT_WFM_ADDR:DEFAULT_WFM_ADDR + WBF_FILE_LENGTH]
        wrf = gzip_decompress_stored_member(wrf_gz)
    else:
        wbf = build_wbf()
        wrf = build_wrf(wbf)
        panel_img = build_panel_img(wbf)
        wrf_gz = gzip_stored(wrf)
        pw3_gz = gzip_stored(dd_envelope(panel_waveform_export(panel_img), wrf))
        os.makedirs(out, exist_ok=True)
        write_atomic(paths["panel.img"], panel_img)
        write_atomic(paths["synth.wrf.gz"], wrf_gz)
        write_atomic(paths["synth.pw3.gz"], pw3_gz)

    summary = verify(panel_img, wbf, wrf, wrf_gz, pw3_gz)

    if not args.quiet:
        print("Voyage synthetic panel/waveform: %s" % out)
        for name in OUTPUT_NAMES:
            with open(paths[name], "rb") as f:
                data = f.read()
            print("  %-14s %8d bytes  sha256 %s" %
                  (name, len(data), hashlib.sha256(data).hexdigest()))
        h = summary["header"]
        print("  WBF header: file_length=0x%05X serial=0x%04X type=0x%02X "
              "mode_version=%d panel_size=%d mc=%d trc=%d advanced=%d "
              "xwia=0x%03X crc32=0x%08X" %
              (h["file_length"], h["serial"], h["wf_type"], h["mode_version"],
               h["panel_size"], h["mc"], h["trc"], h["advanced_wfm_flags"],
               h["xwia"], h["checksum"]))
        print("  panel: bcd=%s vcom=%s id=%s xwi=%s" %
              (summary["panel_side"]["bcd"], summary["panel_side"]["vcom"],
               summary["panel_side"]["panel_id"], summary["panel_side"]["xwi"]))
        print("  WRF human version (stock eink_get_wfm_human_version): %s" %
              summary["wrf_human_version"])
        print("  checks passed: dd_envelope + dd_gzip_stored + "
              "android_wf_header_valid/container/wrf_layout/matches_panel "
              "+ stock process_fw + panel driver decode")
    return 0


if __name__ == "__main__":
    sys.exit(main())
