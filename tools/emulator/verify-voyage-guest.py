#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify the bytes the running emulator guest reports against the generated
synthetic panel/waveform files.

Run after tools/emulator/prepare-voyage-guest.sh and a reboot, after copying

  /proc/eink/panel/waveform/data  (456570 bytes)  -> READSDIR/panel-waveform-data.bin
  /proc/eink/waveform/data        (2048 bytes)    -> READSDIR/waveform-data.bin

out of the guest (see tools/emulator/README.md).  This checks, on the bytes the
guest really returned rather than on the generator's own copies:

  * /proc/eink/panel/waveform/data == panel.img[0x886:0x70000]
  * /proc/eink/waveform/data       == gunzip(synth.wrf.gz)
  * dd_envelope() on those two     == gunzip(synth.pw3.gz) == gzip_stored(dd_envelope(...))
  * android_wf_container()/android_wrf_layout() accept synth.pw3.gz
  * the sizes read back are exactly 456570 and 2048

Usage:
  python3 verify-voyage-guest.py [--panel DIR] [--reads DIR]
"""

import argparse
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_voyage_panel as gen  # noqa: E402  (same directory)


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--panel", default=os.path.join(
        os.path.dirname(repo), "readom-port", "emu-voyage", "panel"))
    parser.add_argument("--reads", default=os.path.join(
        os.path.dirname(repo), "readom-port", "emu-voyage", "reads"))
    args = parser.parse_args(argv)

    def in_panel(name):
        return os.path.join(args.panel, name)

    def in_reads(name):
        return os.path.join(args.reads, name)

    read_panel = open(in_panel("panel.img"), "rb").read()
    synth_wrf_gz = open(in_panel("synth.wrf.gz"), "rb").read()
    synth_pw3_gz = open(in_panel("synth.pw3.gz"), "rb").read()
    guest_panel = open(in_reads("panel-waveform-data.bin"), "rb").read()
    guest_wrf = open(in_reads("waveform-data.bin"), "rb").read()

    expected_panel = gen.panel_waveform_export(read_panel)
    expected_wrf = gen.gzip_decompress_stored_member(synth_wrf_gz)

    failures = []

    def check(cond, message):
        if cond:
            print("ok   %s" % message)
        else:
            print("FAIL %s" % message)
            failures.append(message)

    print("panel.img              sha256 %s" % hashlib.sha256(read_panel).hexdigest())
    print("synth.wrf.gz           sha256 %s" % hashlib.sha256(synth_wrf_gz).hexdigest())
    print("synth.pw3.gz           sha256 %s" % hashlib.sha256(synth_pw3_gz).hexdigest())
    print("guest panel read       %d bytes sha256 %s" %
          (len(guest_panel), hashlib.sha256(guest_panel).hexdigest()))
    print("guest WRF read         %d bytes sha256 %s" %
          (len(guest_wrf), hashlib.sha256(guest_wrf).hexdigest()))

    check(len(guest_panel) == 456570, "/proc/eink/panel/waveform/data is 456570 bytes")
    check(len(guest_wrf) == 2048, "/proc/eink/waveform/data is 2048 bytes")
    check(guest_panel == expected_panel,
          "/proc/eink/panel/waveform/data == panel.img[0x886:0x70000]")
    check(guest_wrf == expected_wrf,
          "/proc/eink/waveform/data == gunzip(synth.wrf.gz)")

    envelope = gen.dd_envelope(guest_panel, guest_wrf)
    check(gen.gzip_stored(envelope) == synth_pw3_gz,
          "dd_envelope(actual 456570 + 2048) gzipped == synth.pw3.gz")
    envelope_raw = gen.gzip_decompress_stored_member(synth_pw3_gz)
    check(envelope == envelope_raw,
          "dd_envelope(actual reads) == gunzip(synth.pw3.gz)")
    env_wbf, env_wrf, (vc_off, vc_len) = gen.android_wf_container(envelope_raw)
    check(env_wbf == guest_panel[:65536],
          "android_wf_container envelope WBF == first 65536 bytes of the panel read")
    check(env_wrf == guest_wrf, "android_wf_container envelope WRF == guest WRF")
    check(vc_off == 0 and vc_len == 0, "advanced_wfm_flags=0 so there is no VC section")

    checks = gen.panel_driver_checks(read_panel)
    check(checks["bcd"] == "EBA", "panel bcd reads EBA")
    check(checks["vcom"] == "-2.05", "panel VCOM reads -2.05")
    check(checks["xwi"] == gen.XWI_STRING.decode(),
          "panel XWI reads %s" % gen.XWI_STRING.decode())
    check(gen.stock_process_fw_checks(expected_wrf, synth_wrf_gz) == gen.WRF_XWI,
          "stock process_fw accepts gunzip(synth.wrf.gz); human version %s" %
          gen.WRF_XWI.decode())

    if failures:
        print("\n%d check(s) failed" % len(failures))
        return 1
    print("\nall checks passed on the bytes the guest returned")
    return 0


if __name__ == "__main__":
    sys.exit(main())
